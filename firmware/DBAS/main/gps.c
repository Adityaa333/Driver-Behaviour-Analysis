/* ============================================================================
 * gps.c
 *
 * Implementation of the NMEA-0183 GPS driver declared in gps.h.
 *
 * Parses $--RMC (recommended minimum: position, speed, heading, fix
 * status) and $--GGA (fix quality, satellite count, altitude) sentences.
 * The "--" talker ID is accepted generically (GP, GN, GL, etc.) so the
 * driver works with both single- and multi-constellation modules.
 *
 * Every sentence is validated against its NMEA checksum before being
 * parsed; malformed or corrupted sentences (common on noisy vehicle
 * wiring) are discarded rather than risking a bad fix being reported.
 * ========================================================================= */

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "gps.h"
#include "config.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "gps";

#define GPS_UART_READ_TIMEOUT_MS       100
#define GPS_UART_TX_BUF_DISABLED       0   /* Module is read-only for us */

/* ---------------------------------------------------------------------------
 * Module State
 * ------------------------------------------------------------------------- */
static SemaphoreHandle_t s_data_mutex = NULL;
static bool s_initialized = false;

/* Merged snapshot exposed to consumers via gps_get_latest(). */
static gps_data_t s_latest = {
    .latitude_deg = 0.0,
    .longitude_deg = 0.0,
    .speed_kmh = 0.0f,
    .heading_deg = 0.0f,
    .altitude_m = 0.0f,
    .satellites_in_use = 0,
    .fix_valid = false,
    .timestamp_us = 0,
};

/* Working fields carried over between sentences. GGA fields (altitude,
 * satellite count) persist across RMC updates since the two sentences
 * arrive separately within the same reporting cycle. */
static float s_working_altitude_m = 0.0f;
static uint8_t s_working_satellites = 0;

static int64_t s_last_valid_fix_time_us = -1;

/* ---------------------------------------------------------------------------
 * NMEA Parsing Helpers
 * ------------------------------------------------------------------------- */

/**
 * @brief Convert an NMEA coordinate field (ddmm.mmmm or dddmm.mmmm) and
 *        hemisphere character into signed decimal degrees.
 */
static double nmea_coord_to_decimal(const char *field, char hemisphere)
{
    if (field == NULL || field[0] == '\0') {
        return 0.0;
    }

    double raw = atof(field);
    double degrees_whole = floor(raw / 100.0);
    double minutes = raw - (degrees_whole * 100.0);
    double decimal = degrees_whole + (minutes / 60.0);

    if (hemisphere == 'S' || hemisphere == 'W') {
        decimal = -decimal;
    }
    return decimal;
}

/**
 * @brief Validate the NMEA checksum of a sentence of the form
 *        "$TALKERID,field,field,...*HH" (without trailing CR/LF).
 *
 * @return true if the checksum is present and matches, false otherwise.
 */
static bool nmea_checksum_valid(const char *sentence)
{
    if (sentence == NULL || sentence[0] != '$') {
        return false;
    }

    const char *star = strchr(sentence, '*');
    if (star == NULL || strlen(star) < 3) {
        return false; /* No checksum present */
    }

    uint8_t computed = 0;
    for (const char *p = sentence + 1; p < star; p++) {
        computed ^= (uint8_t)(*p);
    }

    uint8_t reported = (uint8_t)strtol(star + 1, NULL, 16);
    return computed == reported;
}

/**
 * @brief Split a NUL-terminated, comma-delimited NMEA sentence into
 *        fields, preserving empty fields. strtok_r cannot be used here:
 *        it merges consecutive delimiters and silently drops empty
 *        tokens, which is fatal for NMEA since sentences routinely
 *        contain empty fields (e.g. no fix yet, no altitude) at fixed
 *        field positions the parser depends on.
 */
static int nmea_split_fields(char *sentence, char *fields[], int max_fields)
{
    int count = 0;
    char *p = sentence;
    fields[count++] = p;
    while (*p != '\0' && count < max_fields) {
        if (*p == ',' || *p == '*') {
            *p = '\0';
            fields[count++] = p + 1;
        }
        p++;
    }
    return count;
}

/**
 * @brief Parse an RMC sentence and update working/latest fix state.
 *        Expects a mutable, NUL-terminated copy of the sentence.
 */
static void gps_parse_rmc(char *sentence)
{
    /* Field layout: $--RMC,time,status,lat,NS,lon,EW,speed,course,date,... */
    char *fields[12] = { NULL };
    int count = nmea_split_fields(sentence, fields, 12);

    if (count < 9) {
        ESP_LOGW(TAG, "RMC sentence has too few fields (%d), discarding", count);
        return;
    }

    char status = fields[2][0];
    bool fix_valid = (status == 'A');

    double lat = 0.0, lon = 0.0;
    float speed_kmh = 0.0f, heading_deg = 0.0f;

    if (fix_valid) {
        lat = nmea_coord_to_decimal(fields[3], fields[4][0]);
        lon = nmea_coord_to_decimal(fields[5], fields[6][0]);
        speed_kmh = (float)(atof(fields[7]) * 1.852); /* knots -> km/h */
        heading_deg = (float)atof(fields[8]);
    }

    if (xSemaphoreTake(s_data_mutex, pdMS_TO_TICKS(MUTEX_MAX_WAIT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Timed out acquiring GPS data mutex in RMC handler");
        return;
    }

    if (fix_valid) {
        s_latest.latitude_deg = lat;
        s_latest.longitude_deg = lon;
        s_latest.speed_kmh = speed_kmh;
        s_latest.heading_deg = heading_deg;
        s_last_valid_fix_time_us = esp_timer_get_time();
    }
    s_latest.fix_valid = fix_valid;
    s_latest.altitude_m = s_working_altitude_m;
    s_latest.satellites_in_use = s_working_satellites;
    s_latest.timestamp_us = esp_timer_get_time();

    xSemaphoreGive(s_data_mutex);
}

/**
 * @brief Parse a GGA sentence and update the working altitude/satellite
 *        fields. These are merged into s_latest on the next RMC update.
 *        Expects a mutable, NUL-terminated copy of the sentence.
 */
static void gps_parse_gga(char *sentence)
{
    /* Field layout:
     * $--GGA,time,lat,NS,lon,EW,fixquality,numSV,HDOP,alt,M,... */
    char *fields[10] = { NULL };
    int count = nmea_split_fields(sentence, fields, 10);

    if (count < 10) {
        ESP_LOGW(TAG, "GGA sentence has too few fields (%d), discarding", count);
        return;
    }

    int fix_quality = atoi(fields[6]);
    uint8_t satellites = (uint8_t)atoi(fields[7]);
    float altitude_m = (fix_quality > 0) ? (float)atof(fields[9]) : s_working_altitude_m;

    if (xSemaphoreTake(s_data_mutex, pdMS_TO_TICKS(MUTEX_MAX_WAIT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Timed out acquiring GPS data mutex in GGA handler");
        return;
    }

    s_working_altitude_m = altitude_m;
    s_working_satellites = satellites;

    xSemaphoreGive(s_data_mutex);
}

/**
 * @brief Dispatch a complete, NUL-terminated NMEA sentence to the
 *        appropriate parser after validating its checksum.
 */
static void gps_parse_sentence(const char *sentence)
{
    if (!nmea_checksum_valid(sentence)) {
        ESP_LOGW(TAG, "Discarding sentence with invalid/missing checksum");
        return;
    }

    /* Sentence type occupies characters [3:6), e.g. "RMC" in "$GPRMC" or
     * "$GNRMC". strtok_r requires a mutable buffer, so copy first. */
    if (strlen(sentence) < 6) {
        return;
    }

    char work_buf[GPS_NMEA_MAX_SENTENCE_LEN];
    strncpy(work_buf, sentence, sizeof(work_buf) - 1);
    work_buf[sizeof(work_buf) - 1] = '\0';

    if (strncmp(&sentence[3], "RMC", 3) == 0) {
        gps_parse_rmc(work_buf);
    } else if (strncmp(&sentence[3], "GGA", 3) == 0) {
        gps_parse_gga(work_buf);
    }
    /* Other sentence types (GSV, GSA, VTG, ...) are intentionally ignored;
     * RMC + GGA together provide every field this system requires. */
}

/* ---------------------------------------------------------------------------
 * Background UART Reader Task
 * ------------------------------------------------------------------------- */

static void gps_uart_task(void *arg)
{
    (void)arg;
    char line[GPS_NMEA_MAX_SENTENCE_LEN];
    size_t line_len = 0;
    uint8_t byte;

    ESP_LOGI(TAG, "GPS UART task started");

    for (;;) {
        int len = uart_read_bytes(GPS_UART_PORT, &byte, 1,
                                   pdMS_TO_TICKS(GPS_UART_READ_TIMEOUT_MS));
        if (len <= 0) {
            continue; /* No byte available within timeout; keep waiting */
        }

        if (byte == '\r') {
            continue;
        }

        if (byte == '\n') {
            if (line_len > 0) {
                line[line_len] = '\0';
                ESP_LOGD(TAG, "RAW: %s", line);   // once per sentence, debug level
                if (line[0] == '$') {
                    gps_parse_sentence(line);
                }
                line_len = 0;
            }
        continue;
}

        if (line_len < (sizeof(line) - 1)) {
            line[line_len++] = (char)byte;
        } else {
            /* Sentence exceeded expected NMEA length; likely corrupted
             * data. Discard and resynchronize on the next newline. */
            ESP_LOGW(TAG, "NMEA sentence exceeded buffer size, discarding");
            line_len = 0;
        }
    }
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

esp_err_t gps_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_data_mutex = xSemaphoreCreateMutex();
    if (s_data_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create GPS data mutex");
        return ESP_ERR_NO_MEM;
    }

    uart_config_t uart_conf = {
        .baud_rate = GPS_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_param_config(GPS_UART_PORT, &uart_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_set_pin(GPS_UART_PORT, GPS_UART_TX_GPIO, GPS_UART_RX_GPIO,
                        UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_driver_install(GPS_UART_PORT, GPS_UART_RX_BUF_SIZE,
                               GPS_UART_TX_BUF_DISABLED, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }

    BaseType_t task_created = xTaskCreatePinnedToCore(
        gps_uart_task, "gps_uart_task", TASK_STACK_SIZE_GPS, NULL,
        TASK_PRIORITY_GPS, NULL, TASK_CORE_SENSOR_ACQUISITION);

    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create GPS UART task");
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "GPS driver initialized on UART%d at %d baud",
             GPS_UART_PORT, GPS_UART_BAUD_RATE);
    return ESP_OK;
}

esp_err_t gps_get_latest(gps_data_t *data)
{
    if (data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_data_mutex, pdMS_TO_TICKS(MUTEX_MAX_WAIT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Timed out acquiring GPS data mutex in gps_get_latest");
        return ESP_ERR_TIMEOUT;
    }

    memcpy(data, &s_latest, sizeof(gps_data_t));

    xSemaphoreGive(s_data_mutex);
    return ESP_OK;
}

int64_t gps_get_fix_age_ms(void)
{
    if (s_last_valid_fix_time_us < 0) {
        return -1;
    }
    return (esp_timer_get_time() - s_last_valid_fix_time_us) / 1000;
}
