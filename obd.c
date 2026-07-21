// Design notes:

// Bus-off detection/recovery runs at the start of every obd_read() call rather than only at init 
// — a real vehicle CAN bus can enter bus-off mid-trip (e.g. a loose harness connector), 
// and a commercial fleet device shouldn't need a power cycle to recover from that.
// The response-matching loop discards unrelated CAN traffic instead of assuming the very next frame is the answer 
// — other modules on the bus (ABS, airbag, etc.) are also broadcasting, and a naive "read one frame" implementation 
// would misparse those as PID responses.
// valid_count == 0 (all four PIDs timed out) is treated as a distinct error case from partial success,
//  since that specific pattern usually means "ignition off / adapter unplugged" rather than "one sensor glitched."

/* ============================================================================
 * obd.c
 *
 * Implementation of the OBD-II driver declared in obd.h.
 *
 * Uses the ESP-IDF TWAI driver to send standard Mode 01 PID requests as a
 * functional broadcast (identifier 0x7DF) and waits for the ECU's response
 * (identifier 0x7E8, the standard single-ECU reply address). Includes
 * bus-off detection and automatic recovery, since a vehicle CAN bus is a
 * noisy electrical environment compared to a lab bench.
 * ========================================================================= */

#include <string.h>
#include "obd.h"
#include "config.h"
#include "driver/twai.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "obd";

/* Standard OBD-II CAN identifiers (11-bit addressing). */
#define OBD_REQUEST_CAN_ID              0x7DF   /* Functional broadcast */
#define OBD_RESPONSE_CAN_ID_MIN         0x7E8   /* ECU #1 response */
#define OBD_RESPONSE_CAN_ID_MAX         0x7EF   /* ECU #8 response */

#define OBD_MODE_SHOW_CURRENT_DATA      0x01
#define OBD_MODE_RESPONSE_OFFSET        0x40    /* Response mode = request + 0x40 */

static SemaphoreHandle_t s_bus_mutex = NULL;
static bool s_initialized = false;

/* ---------------------------------------------------------------------------
 * Bus Health
 * ------------------------------------------------------------------------- */

/**
 * @brief Check the TWAI controller state and attempt recovery if the bus
 *        has entered the bus-off state (e.g. after sustained electrical
 *        noise or a disconnected harness). Caller must hold s_bus_mutex.
 */
static void obd_check_bus_health_locked(void)
{
    twai_status_info_t status;
    esp_err_t err = twai_get_status_info(&status);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "twai_get_status_info failed: %s", esp_err_to_name(err));
        return;
    }

    if (status.state == TWAI_STATE_BUS_OFF) {
        ESP_LOGW(TAG, "CAN bus is in BUS_OFF state, initiating recovery");
        err = twai_initiate_recovery();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "twai_initiate_recovery failed: %s", esp_err_to_name(err));
            return;
        }
        /* Recovery takes 128 occurrences of bus-idle condition per the CAN
         * spec; give it a moment before attempting to restart. */
        vTaskDelay(pdMS_TO_TICKS(100));
        err = twai_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "twai_start after recovery failed: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "CAN bus recovered and restarted");
        }
    }
}

/* ---------------------------------------------------------------------------
 * PID Request / Response
 * ------------------------------------------------------------------------- */

/**
 * @brief Send a Mode 01 request for a single PID and wait for the matching
 *        response, discarding any unrelated frames received in the
 *        meantime. Caller must hold s_bus_mutex.
 *
 * @param[in]  pid        The OBD-II PID to request.
 * @param[out] out_byte_a First data byte (A) of the response.
 * @param[out] out_byte_b Second data byte (B) of the response, only valid
 *                         if the response frame contained one (i.e. its
 *                         DLC indicates at least 5 payload bytes).
 * @param[out] out_has_byte_b Set true if out_byte_b was populated.
 * @return ESP_OK on a valid matching response, ESP_ERR_TIMEOUT otherwise.
 */
static esp_err_t obd_request_pid_locked(uint8_t pid, uint8_t *out_byte_a,
                                         uint8_t *out_byte_b, bool *out_has_byte_b)
{
    twai_message_t tx_msg = {0};
    tx_msg.identifier = OBD_REQUEST_CAN_ID;
    tx_msg.extd = 0;              /* Standard 11-bit identifier */
    tx_msg.rtr = 0;
    tx_msg.data_length_code = 8;
    tx_msg.data[0] = 0x02;        /* 2 additional bytes follow: mode + pid */
    tx_msg.data[1] = OBD_MODE_SHOW_CURRENT_DATA;
    tx_msg.data[2] = pid;
    tx_msg.data[3] = 0x00;
    tx_msg.data[4] = 0x00;
    tx_msg.data[5] = 0x00;
    tx_msg.data[6] = 0x00;
    tx_msg.data[7] = 0x00;

    esp_err_t err = twai_transmit(&tx_msg, pdMS_TO_TICKS(OBD_REQUEST_TIMEOUT_MS));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "twai_transmit failed for PID 0x%02X: %s", pid, esp_err_to_name(err));
        return ESP_ERR_TIMEOUT;
    }

    int64_t deadline_us = esp_timer_get_time() + ((int64_t)OBD_REQUEST_TIMEOUT_MS * 1000);

    while (esp_timer_get_time() < deadline_us) {
        int64_t remaining_ms = (deadline_us - esp_timer_get_time()) / 1000;
        if (remaining_ms <= 0) {
            break;
        }

        twai_message_t rx_msg;
        err = twai_receive(&rx_msg, pdMS_TO_TICKS(remaining_ms));
        if (err != ESP_OK) {
            continue; /* Timeout on this attempt; loop checks overall deadline */
        }

        bool id_in_range = (rx_msg.identifier >= OBD_RESPONSE_CAN_ID_MIN) &&
                            (rx_msg.identifier <= OBD_RESPONSE_CAN_ID_MAX);
        bool mode_matches = (rx_msg.data_length_code >= 3) &&
                             (rx_msg.data[1] == (OBD_MODE_SHOW_CURRENT_DATA + OBD_MODE_RESPONSE_OFFSET)) &&
                             (rx_msg.data[2] == pid);

        if (id_in_range && mode_matches) {
            *out_byte_a = rx_msg.data[3];
            if (rx_msg.data_length_code >= 5) {
                *out_byte_b = rx_msg.data[4];
                *out_has_byte_b = true;
            } else {
                *out_has_byte_b = false;
            }
            return ESP_OK;
        }
        /* Frame did not match (unrelated bus traffic); keep waiting until
         * the deadline. */
    }

    ESP_LOGW(TAG, "Timed out waiting for response to PID 0x%02X", pid);
    return ESP_ERR_TIMEOUT;
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

esp_err_t obd_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_bus_mutex = xSemaphoreCreateMutex();
    if (s_bus_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create OBD bus mutex");
        return ESP_ERR_NO_MEM;
    }

    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(
        OBD_TWAI_TX_GPIO, OBD_TWAI_RX_GPIO, TWAI_MODE_NORMAL);

    /* OBD_CAN_BITRATE_BPS is fixed at 500000 in config.h, the standard
     * ISO 15765-4 (CAN) OBD-II bus speed used by the vast majority of
     * passenger vehicles built since 2008. */
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    esp_err_t err = twai_driver_install(&g_config, &t_config, &f_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "twai_driver_install failed: %s", esp_err_to_name(err));
        return ESP_ERR_INVALID_STATE;
    }

    err = twai_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "twai_start failed: %s", esp_err_to_name(err));
        return ESP_ERR_INVALID_STATE;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "OBD-II TWAI driver initialized at %d bps", OBD_CAN_BITRATE_BPS);
    return ESP_OK;
}

esp_err_t obd_read(obd_data_t *data)
{
    if (data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_bus_mutex, pdMS_TO_TICKS(MUTEX_MAX_WAIT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Timed out acquiring OBD bus mutex");
        return ESP_ERR_TIMEOUT;
    }

    obd_check_bus_health_locked();

    memset(data, 0, sizeof(obd_data_t));
    uint8_t byte_a = 0, byte_b = 0;
    bool has_byte_b = false;
    uint8_t valid_count = 0;

    /* Engine RPM: ((A * 256) + B) / 4 */
    if (obd_request_pid_locked(OBD_PID_ENGINE_RPM, &byte_a, &byte_b, &has_byte_b) == ESP_OK
        && has_byte_b) {
        data->engine_rpm = (uint16_t)(((uint16_t)byte_a * 256 + byte_b) / 4);
        data->engine_rpm_valid = true;
        valid_count++;
    }

    /* Vehicle speed: A, already in km/h */
    if (obd_request_pid_locked(OBD_PID_VEHICLE_SPEED, &byte_a, &byte_b, &has_byte_b) == ESP_OK) {
        data->vehicle_speed_kmh = (float)byte_a;
        data->vehicle_speed_valid = true;
        valid_count++;
    }

    /* Throttle position: A * 100 / 255, percent */
    if (obd_request_pid_locked(OBD_PID_THROTTLE_POSITION, &byte_a, &byte_b, &has_byte_b) == ESP_OK) {
        data->throttle_position_pct = ((float)byte_a * 100.0f) / 255.0f;
        data->throttle_position_valid = true;
        valid_count++;
    }

    /* Coolant temperature: A - 40, Celsius */
    if (obd_request_pid_locked(OBD_PID_ENGINE_COOLANT_TEMP, &byte_a, &byte_b, &has_byte_b) == ESP_OK) {
        data->coolant_temp_c = (int16_t)byte_a - 40;
        data->coolant_temp_valid = true;
        valid_count++;
    }

    xSemaphoreGive(s_bus_mutex);

    data->timestamp_us = esp_timer_get_time();

    if (valid_count == 0) {
        ESP_LOGW(TAG, "No OBD-II PIDs responded this cycle (bus disconnected?)");
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}
