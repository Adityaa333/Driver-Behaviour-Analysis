// Design notes:

// This version replaces the TWAI/CAN transport with a Bluetooth Classic
// SPP link to an ELM327-compatible OBD-II dongle. The public contract in
// obd.h (obd_data_t, obd_init(), obd_read()) is UNCHANGED so sensor_manager.c
// and everything downstream of it needs no modification at all.
//
// Requires the following new macros in config.h (not shown here):
//   OBD_BT_TARGET_MAC        - 6-byte esp_bd_addr_t of the paired dongle
//   OBD_BT_SPP_SCN           - remote SPP server channel number. Many
//                              cheap ELM327 clones fix this at 1; if yours
//                              doesn't, run an SDP discovery once during
//                              bring-up to find the real channel and hard
//                              code it here.
//   OBD_BT_CONNECT_TIMEOUT_MS
//   OBD_BT_RECONNECT_DELAY_MS
//   OBD_REQUEST_TIMEOUT_MS   - must be raised from the CAN-era 100ms;
//                              Bluetooth SPP round trips are far slower.
//                              300-500ms is a more realistic starting point.
//
// A single background task (obd_bt_connection_task) owns the SPP
// connection lifecycle: it connects at startup and reconnects
// automatically (bounded retry + backoff, mirroring wifi_manager.c's
// approach) whenever the link drops - a Bluetooth link to a dongle
// riding around in a vehicle will drop, and this module must recover
// without requiring a device reboot.
//
// Bytes arriving from the dongle are pushed onto a FreeRTOS queue by the
// SPP data-indication callback (which runs in the Bluedroid callback
// context, not our own task) and consumed by whichever task is currently
// waiting on a command's response inside obd_request_pid_locked(). Only
// one request is ever in flight at a time, enforced by s_bus_mutex - this
// plays the same role s_bus_mutex played in the CAN version.
// ============================================================================

/* ============================================================================
 * obd.c
 *
 * Implementation of the OBD-II driver declared in obd.h, using a
 * Bluetooth Classic SPP connection to an ELM327-compatible dongle
 * instead of a directly-wired CAN transceiver.
 *
 * ELM327 "AT" commands and Mode 01 PID requests are plain ASCII text
 * terminated by '\r'; responses are ASCII hex terminated by a '>' prompt
 * character. This module hides all of that framing/parsing behind the
 * same obd_read() contract the rest of the firmware already depends on.
 * ========================================================================= */

#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "obd.h"
#include "config.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_spp_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "obd";

#define OBD_RX_QUEUE_LEN            32     /* SPP data chunks buffered */
#define OBD_RX_CHUNK_MAX_LEN        128    /* Max bytes per queued chunk */
#define OBD_RESPONSE_BUF_LEN        256    /* Accumulated ASCII response */
#define OBD_AT_RETRY_COUNT          3

typedef struct {
    uint8_t data[OBD_RX_CHUNK_MAX_LEN];
    uint16_t len;
} obd_rx_chunk_t;

static SemaphoreHandle_t s_bus_mutex = NULL;   /* Serializes one request at a time */
static QueueHandle_t s_rx_queue = NULL;        /* SPP callback -> waiting task */

static uint32_t s_spp_handle = 0;
static volatile bool s_spp_connected = false;
static volatile bool s_elm327_configured = false;
static bool s_initialized = false;

static const esp_bd_addr_t s_target_addr = OBD_BT_TARGET_MAC;

/* ---------------------------------------------------------------------------
 * SPP Response Reading
 * ------------------------------------------------------------------------- */

/**
 * @brief Drain the RX queue, accumulating bytes into out_buf until a '>'
 *        prompt character is seen (ELM327's end-of-response marker) or
 *        the timeout elapses. Caller must hold s_bus_mutex.
 *
 * @return ESP_OK if a full response (terminated by '>') was captured,
 *         ESP_ERR_TIMEOUT otherwise.
 */
static esp_err_t obd_read_response_until_prompt_locked(char *out_buf, size_t out_buf_len,
                                                        uint32_t timeout_ms)
{
    size_t written = 0;
    out_buf[0] = '\0';

    int64_t deadline_us = esp_timer_get_time() + ((int64_t)timeout_ms * 1000);

    for (;;) {
        int64_t remaining_ms = (deadline_us - esp_timer_get_time()) / 1000;
        if (remaining_ms <= 0) {
            ESP_LOGW(TAG, "Timed out waiting for ELM327 prompt");
            return ESP_ERR_TIMEOUT;
        }

        obd_rx_chunk_t chunk;
        if (xQueueReceive(s_rx_queue, &chunk, pdMS_TO_TICKS(remaining_ms)) != pdTRUE) {
            continue; /* Loop re-checks the overall deadline */
        }

        for (uint16_t i = 0; i < chunk.len && written < (out_buf_len - 1); i++) {
            out_buf[written++] = (char)chunk.data[i];
            if (chunk.data[i] == '>') {
                out_buf[written] = '\0';
                return ESP_OK;
            }
        }
    }
}

/**
 * @brief Send a raw ASCII command (already including its trailing '\r')
 *        to the connected dongle and wait for the '>' terminated
 *        response. Caller must hold s_bus_mutex.
 */
static esp_err_t obd_send_command_locked(const char *cmd, char *resp_buf, size_t resp_buf_len,
                                          uint32_t timeout_ms)
{
    if (!s_spp_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Drain any stale bytes left over from a previous exchange (e.g. a
     * late duplicate response) before sending a new command, so they
     * can't be misread as part of this command's response. */
    obd_rx_chunk_t stale;
    while (xQueueReceive(s_rx_queue, &stale, 0) == pdTRUE) {
        /* discard */
    }

    esp_err_t err = esp_spp_write(s_spp_handle, (int)strlen(cmd), (uint8_t *)cmd);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_spp_write failed for command \"%s\": %s", cmd, esp_err_to_name(err));
        return ESP_ERR_TIMEOUT;
    }

    return obd_read_response_until_prompt_locked(resp_buf, resp_buf_len, timeout_ms);
}

/* ---------------------------------------------------------------------------
 * ELM327 Setup (AT Commands)
 * ------------------------------------------------------------------------- */

/**
 * @brief Run the standard ELM327 reset/configuration sequence after a
 *        fresh SPP connection: reset, echo off, linefeeds off, auto
 *        protocol select. Caller must hold s_bus_mutex.
 */
static esp_err_t obd_elm327_configure_locked(void)
{
    static const char *const setup_cmds[] = {
        "ATZ\r",    /* Reset */
        "ATE0\r",   /* Echo off - avoids re-parsing our own command text */
        "ATL0\r",   /* Linefeeds off - simplifies response parsing */
        "ATSP0\r",  /* Auto-select OBD-II protocol */
    };

    char resp[OBD_RESPONSE_BUF_LEN];

    for (size_t i = 0; i < sizeof(setup_cmds) / sizeof(setup_cmds[0]); i++) {
        esp_err_t err = ESP_ERR_TIMEOUT;
        for (int attempt = 1; attempt <= OBD_AT_RETRY_COUNT && err != ESP_OK; attempt++) {
            err = obd_send_command_locked(setup_cmds[i], resp, sizeof(resp),
                                           OBD_REQUEST_TIMEOUT_MS);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ELM327 setup command \"%s\" failed", setup_cmds[i]);
            return err;
        }
        /* ATZ reboots the adapter's own microcontroller; give it a beat
         * before sending the next command. */
        if (i == 0) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }

    ESP_LOGI(TAG, "ELM327 adapter configured");
    return ESP_OK;
}

/* ---------------------------------------------------------------------------
 * PID Request / Response Parsing
 * ------------------------------------------------------------------------- */

/**
 * @brief Parse an ELM327 Mode 01 response line such as "41 0C 1A F8" into
 *        its data bytes. Tolerant of the "SEARCHING..." line some
 *        adapters prepend and of embedded whitespace/CR.
 *
 * @return true if a well-formed "41 <pid> ..." response was found.
 */
static bool obd_parse_pid_response(const char *resp, uint8_t pid,
                                    uint8_t *out_byte_a, uint8_t *out_byte_b,
                                    bool *out_has_byte_b)
{
    char work_buf[OBD_RESPONSE_BUF_LEN];
    strncpy(work_buf, resp, sizeof(work_buf) - 1);
    work_buf[sizeof(work_buf) - 1] = '\0';

    char *saveptr = NULL;
    char *tok = strtok_r(work_buf, " \r\n", &saveptr);

    uint8_t bytes[8];
    int byte_count = 0;
    bool found_header = false;

    while (tok != NULL) {
        /* Skip non-hex noise such as "SEARCHING..." or the trailing '>'. */
        if (strlen(tok) == 2 && isxdigit((unsigned char)tok[0]) && isxdigit((unsigned char)tok[1])) {
            uint8_t val = (uint8_t)strtol(tok, NULL, 16);

            if (!found_header) {
                if (val == 0x41) {
                    found_header = true; /* Mode 01 response marker */
                }
            } else if (byte_count == 0 && val != pid) {
                /* First byte after 0x41 must echo the requested PID;
                 * otherwise this is a response to something else. */
                found_header = false;
            } else {
                if (byte_count < (int)sizeof(bytes)) {
                    bytes[byte_count++] = val;
                }
            }
        }
        tok = strtok_r(NULL, " \r\n", &saveptr);
    }

    /* bytes[0] is the echoed PID itself; data starts at bytes[1]. */
    if (!found_header || byte_count < 2) {
        return false;
    }

    *out_byte_a = bytes[1];
    if (byte_count >= 3) {
        *out_byte_b = bytes[2];
        *out_has_byte_b = true;
    } else {
        *out_has_byte_b = false;
    }
    return true;
}

/**
 * @brief Request a single Mode 01 PID over the SPP link and parse its
 *        response. Caller must hold s_bus_mutex.
 */
static esp_err_t obd_request_pid_locked(uint8_t pid, uint8_t *out_byte_a,
                                         uint8_t *out_byte_b, bool *out_has_byte_b)
{
    if (!s_spp_connected || !s_elm327_configured) {
        return ESP_ERR_INVALID_STATE;
    }

    char cmd[8];
    snprintf(cmd, sizeof(cmd), "01%02X\r", pid);

    char resp[OBD_RESPONSE_BUF_LEN];
    esp_err_t err = obd_send_command_locked(cmd, resp, sizeof(resp), OBD_REQUEST_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Timed out waiting for response to PID 0x%02X", pid);
        return ESP_ERR_TIMEOUT;
    }

    if (!obd_parse_pid_response(resp, pid, out_byte_a, out_byte_b, out_has_byte_b)) {
        ESP_LOGW(TAG, "Unparseable/negative response for PID 0x%02X: \"%s\"", pid, resp);
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

/* ---------------------------------------------------------------------------
 * SPP Event Handling
 * ------------------------------------------------------------------------- */

static void obd_spp_callback(esp_spp_cb_event_t event, esp_spp_cb_param_t *param)
{
    switch (event) {
        case ESP_SPP_INIT_EVT:
            ESP_LOGI(TAG, "SPP initialized");
            break;

        case ESP_SPP_OPEN_EVT:
            ESP_LOGI(TAG, "SPP connection opened to OBD dongle");
            s_spp_handle = param->open.handle;
            s_spp_connected = true;
            s_elm327_configured = false;
            break;

        case ESP_SPP_CLOSE_EVT:
            ESP_LOGW(TAG, "SPP connection closed, will attempt reconnect");
            s_spp_connected = false;
            s_elm327_configured = false;
            break;

        case ESP_SPP_DATA_IND_EVT: {
            /* Runs in the Bluedroid callback context - keep this fast
             * and non-blocking; just hand the bytes off to whichever
             * task is waiting inside obd_read_response_until_prompt_locked(). */
            obd_rx_chunk_t chunk;
            uint16_t len = param->data_ind.len;
            if (len > OBD_RX_CHUNK_MAX_LEN) {
                len = OBD_RX_CHUNK_MAX_LEN;
            }
            memcpy(chunk.data, param->data_ind.data, len);
            chunk.len = len;
            if (xQueueSend(s_rx_queue, &chunk, 0) != pdTRUE) {
                ESP_LOGW(TAG, "OBD RX queue full, dropping data chunk");
            }
            break;
        }

        default:
            break;
    }
}

/* ---------------------------------------------------------------------------
 * Connection Management Task
 * ------------------------------------------------------------------------- */

/**
 * @brief Owns the connect/reconnect lifecycle: attempts an SPP connection
 *        to OBD_BT_TARGET_MAC, waits for it to come up, runs the ELM327
 *        setup sequence, then blocks until the link drops and repeats -
 *        indefinitely, so a mid-trip Bluetooth dropout recovers on its
 *        own without a reboot (mirroring wifi_manager.c's philosophy).
 */
static void obd_bt_connection_task(void *arg)
{
    (void)arg;

    for (;;) {
        if (!s_spp_connected) {
            ESP_LOGI(TAG, "Attempting SPP connection to OBD dongle");
            esp_err_t err = esp_spp_connect(ESP_SPP_SEC_NONE, ESP_SPP_ROLE_MASTER,
                                             OBD_BT_SPP_SCN, (uint8_t *)s_target_addr);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "esp_spp_connect failed: %s", esp_err_to_name(err));
            }

            /* Wait for ESP_SPP_OPEN_EVT (or the timeout) before deciding
             * whether to configure the adapter or back off and retry. */
            int64_t deadline_us = esp_timer_get_time() +
                                   ((int64_t)OBD_BT_CONNECT_TIMEOUT_MS * 1000);
            while (!s_spp_connected && esp_timer_get_time() < deadline_us) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }

        if (s_spp_connected && !s_elm327_configured) {
            if (xSemaphoreTake(s_bus_mutex, pdMS_TO_TICKS(MUTEX_MAX_WAIT_MS)) == pdTRUE) {
                esp_err_t err = obd_elm327_configure_locked();
                s_elm327_configured = (err == ESP_OK);
                xSemaphoreGive(s_bus_mutex);
            }
        }

        if (!s_spp_connected) {
            ESP_LOGW(TAG, "OBD dongle not connected, retrying in %d ms",
                     OBD_BT_RECONNECT_DELAY_MS);
            vTaskDelay(pdMS_TO_TICKS(OBD_BT_RECONNECT_DELAY_MS));
        } else {
            /* Connected and configured; just idle-check periodically so
             * a future disconnect is noticed and re-enters this loop. */
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
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

    s_rx_queue = xQueueCreate(OBD_RX_QUEUE_LEN, sizeof(obd_rx_chunk_t));
    if (s_rx_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create OBD RX queue");
        return ESP_ERR_NO_MEM;
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_bt_controller_init failed: %s", esp_err_to_name(err));
        return ESP_ERR_INVALID_STATE;
    }

    err = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_bt_controller_enable failed: %s", esp_err_to_name(err));
        return ESP_ERR_INVALID_STATE;
    }

    err = esp_bluedroid_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_bluedroid_init failed: %s", esp_err_to_name(err));
        return ESP_ERR_INVALID_STATE;
    }

    err = esp_bluedroid_enable();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_bluedroid_enable failed: %s", esp_err_to_name(err));
        return ESP_ERR_INVALID_STATE;
    }

    err = esp_spp_register_callback(obd_spp_callback);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_spp_register_callback failed: %s", esp_err_to_name(err));
        return ESP_ERR_INVALID_STATE;
    }

    esp_spp_cfg_t spp_cfg = {
        .mode = ESP_SPP_MODE_CB,
        .enable_l2cap_ertm = true,
    };
    err = esp_spp_enhanced_init(&spp_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_spp_enhanced_init failed: %s", esp_err_to_name(err));
        return ESP_ERR_INVALID_STATE;
    }

    BaseType_t task_created = xTaskCreatePinnedToCore(
        obd_bt_connection_task, "obd_bt_conn_task", TASK_STACK_SIZE_OBD, NULL,
        TASK_PRIORITY_OBD, NULL, TASK_CORE_SENSOR_ACQUISITION);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create OBD Bluetooth connection task");
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "OBD-II Bluetooth (SPP/ELM327) driver initialized");
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

    if (!s_spp_connected || !s_elm327_configured) {
        /* Not connected right now - same "report honestly, don't hold
         * stale data" behavior as the CAN version's bus-disconnected
         * case; sensor_manager's OBD task clears its cache on this. */
        return ESP_ERR_TIMEOUT;
    }

    if (xSemaphoreTake(s_bus_mutex, pdMS_TO_TICKS(MUTEX_MAX_WAIT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Timed out acquiring OBD bus mutex");
        return ESP_ERR_TIMEOUT;
    }

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
        ESP_LOGW(TAG, "No OBD-II PIDs responded this cycle (dongle disconnected/ignition off?)");
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}
