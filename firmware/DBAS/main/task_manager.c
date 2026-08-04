/* ============================================================================
 * task_manager.c
 *
 * Implementation of the orchestration logic declared in task_manager.h.
 * ========================================================================= */

#include <stdio.h>
#include "task_manager.h"
#include "config.h"
#include "wifi_manager.h"
#include "dbas_mqtt_client.h"
#include "sensor_manager.h"
#include "driver_score.h"
#include "crash_detection.h"
#include "idling_detection.h"
#include "geofence.h"
#include "led_indicator.h"

#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_log.h"

static const char *TAG = "task_manager";

#define INIT_RETRY_COUNT             3
#define INIT_RETRY_DELAY_MS          2000
#define REBOOT_DELAY_MS              3000  /* Give logs time to flush before reboot */

/* ---------------------------------------------------------------------------
 * Init Helpers
 * ------------------------------------------------------------------------- */

typedef esp_err_t (*module_init_fn_t)(void);

/**
 * @brief Call an init function up to INIT_RETRY_COUNT times, with a delay
 *        between attempts, returning the first success or the final
 *        failure.
 */
static esp_err_t init_with_retry(module_init_fn_t init_fn, const char *module_name)
{
    for (int attempt = 1; attempt <= INIT_RETRY_COUNT; attempt++) {
        esp_err_t err = init_fn();
        if (err == ESP_OK) {
            if (attempt > 1) {
                ESP_LOGI(TAG, "%s initialized successfully on attempt %d", module_name, attempt);
            }
            return ESP_OK;
        }
        ESP_LOGE(TAG, "%s init failed (attempt %d/%d): %s",
                 module_name, attempt, INIT_RETRY_COUNT, esp_err_to_name(err));
        if (attempt < INIT_RETRY_COUNT) {
            vTaskDelay(pdMS_TO_TICKS(INIT_RETRY_DELAY_MS));
        }
    }
    return ESP_FAIL;
}

/**
 * @brief Log a fatal startup failure and reboot the device after a short
 *        delay (to give the log message a chance to be flushed/seen).
 */
static void fatal_init_failure_reboot(const char *module_name)
{
    ESP_LOGE(TAG, "FATAL: %s could not be initialized after %d attempts. "
             "Rebooting in %d ms.", module_name, INIT_RETRY_COUNT, REBOOT_DELAY_MS);
    vTaskDelay(pdMS_TO_TICKS(REBOOT_DELAY_MS));
    esp_restart();
}

/* ---------------------------------------------------------------------------
 * Telemetry Publishing Task
 * ------------------------------------------------------------------------- */

static void task_manager_telemetry_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "Telemetry publishing task started (period=%dms)", TELEMETRY_PUBLISH_PERIOD_MS);

    for (;;) {
        vehicle_sample_t sample;
        if (sensor_manager_get_latest(&sample) == ESP_OK) {
            cJSON *root = cJSON_CreateObject();
            if (root != NULL) {
                cJSON_AddStringToObject(root, "device_id", mqtt_client_get_device_id());
                cJSON_AddNumberToObject(root, "timestamp_ms", (double)(sample.timestamp_us / 1000));

                cJSON_AddNumberToObject(root, "accel_x_g", sample.accel_x_g);
                cJSON_AddNumberToObject(root, "accel_y_g", sample.accel_y_g);
                cJSON_AddNumberToObject(root, "accel_z_g", sample.accel_z_g);
                cJSON_AddNumberToObject(root, "gyro_x_dps", sample.gyro_x_dps);
                cJSON_AddNumberToObject(root, "gyro_y_dps", sample.gyro_y_dps);
                cJSON_AddNumberToObject(root, "gyro_z_dps", sample.gyro_z_dps);

                cJSON_AddBoolToObject(root, "gps_fix_valid", sample.gps_fix_valid);
                cJSON_AddNumberToObject(root, "latitude_deg", sample.latitude_deg);
                cJSON_AddNumberToObject(root, "longitude_deg", sample.longitude_deg);
                cJSON_AddNumberToObject(root, "gps_speed_kmh", sample.gps_speed_kmh);
                cJSON_AddNumberToObject(root, "heading_deg", sample.heading_deg);

                cJSON_AddBoolToObject(root, "engine_rpm_valid", sample.engine_rpm_valid);
                cJSON_AddNumberToObject(root, "engine_rpm", sample.engine_rpm);
                cJSON_AddBoolToObject(root, "obd_speed_valid", sample.obd_speed_valid);
                cJSON_AddNumberToObject(root, "obd_speed_kmh", sample.obd_speed_kmh);
                cJSON_AddBoolToObject(root, "throttle_position_valid", sample.throttle_position_valid);
                cJSON_AddNumberToObject(root, "throttle_position_pct", sample.throttle_position_pct);

                char *json_str = cJSON_PrintUnformatted(root);
                if (json_str != NULL) {
                    esp_err_t err = mqtt_client_publish_telemetry(json_str);
                    if (err != ESP_OK) {
                        ESP_LOGW(TAG, "Failed to enqueue telemetry publish: %s", esp_err_to_name(err));
                    }
                    cJSON_free(json_str);
                } else {
                    ESP_LOGE(TAG, "Failed to serialize telemetry payload");
                }
                cJSON_Delete(root);
            } else {
                ESP_LOGE(TAG, "Failed to allocate cJSON object for telemetry payload");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(TELEMETRY_PUBLISH_PERIOD_MS));
    }
}

/* ---------------------------------------------------------------------------
 * Status Publishing Task
 * ------------------------------------------------------------------------- */

static void task_manager_status_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "Status publishing task started (period=%dms)", STATUS_PUBLISH_PERIOD_MS);

    char fw_version[16];
    snprintf(fw_version, sizeof(fw_version), "%d.%d.%d",
             FIRMWARE_VERSION_MAJOR, FIRMWARE_VERSION_MINOR, FIRMWARE_VERSION_PATCH);

    for (;;) {
        cJSON *root = cJSON_CreateObject();
        if (root != NULL) {
            cJSON_AddStringToObject(root, "device_id", mqtt_client_get_device_id());
            cJSON_AddStringToObject(root, "device_type", DEVICE_TYPE_STRING);
            cJSON_AddStringToObject(root, "firmware_version", fw_version);
            cJSON_AddNumberToObject(root, "timestamp_ms", (double)(esp_timer_get_time() / 1000));
            cJSON_AddNumberToObject(root, "uptime_sec", (double)(esp_timer_get_time() / 1000000));
            cJSON_AddNumberToObject(root, "free_heap_bytes", esp_get_free_heap_size());
            cJSON_AddNumberToObject(root, "min_free_heap_bytes", esp_get_minimum_free_heap_size());
            cJSON_AddBoolToObject(root, "wifi_connected", wifi_manager_is_connected());
            cJSON_AddBoolToObject(root, "mqtt_connected", mqtt_client_is_connected());

            int8_t rssi_dbm;
            if (wifi_manager_get_rssi(&rssi_dbm) == ESP_OK) {
                cJSON_AddNumberToObject(root, "wifi_rssi_dbm", rssi_dbm);
            }

            float score;
            if (driver_score_get_current(&score) == ESP_OK) {
                cJSON_AddNumberToObject(root, "current_driver_score", score);
            }

            char *json_str = cJSON_PrintUnformatted(root);
            if (json_str != NULL) {
                esp_err_t err = mqtt_client_publish_status(json_str);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to enqueue status publish: %s", esp_err_to_name(err));
                }
                cJSON_free(json_str);
            } else {
                ESP_LOGE(TAG, "Failed to serialize status payload");
            }
            cJSON_Delete(root);
        } else {
            ESP_LOGE(TAG, "Failed to allocate cJSON object for status payload");
        }

        vTaskDelay(pdMS_TO_TICKS(STATUS_PUBLISH_PERIOD_MS));
    }
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

void task_manager_start_all(void)
{
    ESP_LOGI(TAG, "Starting Driver Behaviour Analysis System, firmware v%d.%d.%d",
             FIRMWARE_VERSION_MAJOR, FIRMWARE_VERSION_MINOR, FIRMWARE_VERSION_PATCH);

    /* Connectivity, in dependency order: WiFi must be up before MQTT
     * attempts to connect. Both are treated as fatal-after-retry since
     * this device's purpose is reporting to the cloud; without
     * connectivity it cannot fulfill that purpose. */
    if (init_with_retry(wifi_manager_init, "wifi_manager") != ESP_OK) {
        fatal_init_failure_reboot("wifi_manager");
    }

    if (init_with_retry(mqtt_client_init, "mqtt_client") != ESP_OK) {
        fatal_init_failure_reboot("mqtt_client");
    }

    /* Sensing and scoring pipeline, in dependency order: sensor_manager
     * must exist before any detector reads from it; driver_score must
     * exist before crash_detection/idling_detection/geofence submit
     * events to it. All are safety/scoring-critical and fatal-after-retry. */
    if (init_with_retry(sensor_manager_init, "sensor_manager") != ESP_OK) {
        fatal_init_failure_reboot("sensor_manager");
    }

    if (init_with_retry(driver_score_init, "driver_score") != ESP_OK) {
        fatal_init_failure_reboot("driver_score");
    }

    if (init_with_retry(crash_detection_init, "crash_detection") != ESP_OK) {
        fatal_init_failure_reboot("crash_detection");
    }

    /* Secondary detectors: the vehicle remains safe to operate and score
     * without these, so failure here is logged but non-fatal. */
    if (init_with_retry(idling_detection_init, "idling_detection") != ESP_OK) {
        ESP_LOGE(TAG, "idling_detection failed to start; continuing without it");
    }

    if (init_with_retry(geofence_init, "geofence") != ESP_OK) {
        ESP_LOGE(TAG, "geofence failed to start; continuing without it");
    }

    /* Purely cosmetic/diagnostic - drives the onboard LED off GPS fix
     * quality. Never fatal: a dead LED doesn't affect scoring or safety. */
    if (init_with_retry(led_indicator_init, "led_indicator") != ESP_OK) {
        ESP_LOGE(TAG, "led_indicator failed to start; continuing without it");
    }
    
    /* Telemetry/status publishing tasks. Priorities/stack sizes reuse
     * wifi_manager's constants from config.h: both tasks are low
     * frequency, non-time-critical background reporting work, the same
     * category wifi_manager's own connection task falls into. */
    BaseType_t telemetry_created = xTaskCreatePinnedToCore(
        task_manager_telemetry_task, "telemetry_task", TASK_STACK_SIZE_WIFI_MANAGER, NULL,
        TASK_PRIORITY_WIFI_MANAGER, NULL, TASK_CORE_NETWORKING);
    if (telemetry_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create telemetry publishing task (non-fatal)");
    }

    BaseType_t status_created = xTaskCreatePinnedToCore(
        task_manager_status_task, "status_task", TASK_STACK_SIZE_WIFI_MANAGER, NULL,
        TASK_PRIORITY_WIFI_MANAGER, NULL, TASK_CORE_NETWORKING);
    if (status_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create status publishing task (non-fatal)");
    }

    ESP_LOGI(TAG, "All modules started successfully");
}
