/* ============================================================================
 * idling_detection.c
 *
 * Implementation of the idling detection module declared in
 * idling_detection.h.
 *
 * An MQTT alert fires once per idle session, at the moment the session
 * crosses IDLING_DURATION_THRESHOLD_SEC (not repeatedly while it
 * continues, to avoid alert spam for a vehicle idling for an extended
 * period, e.g. running auxiliary equipment). driver_score, however,
 * receives a small incremental event every check cycle for as long as
 * excessive idling continues, so the cumulative idling penalty
 * accurately reflects total wasted idle time rather than a single flat
 * deduction regardless of how long the idling lasted.
 * ========================================================================= */

#include "idling_detection.h"
#include "config.h"
#include "sensor_manager.h"
#include "driver_score.h"
#include "dbas_mqtt_client.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "idling_detection";

/* Idling develops slowly relative to driving dynamics; a 5-second check
 * period is frequent enough to catch sessions accurately without the
 * overhead of checking at IMU/GPS sample rates. */
#define IDLING_CHECK_PERIOD_MS   5000

static bool s_initialized = false;

/* ---------------------------------------------------------------------------
 * Alert Reporting
 * ------------------------------------------------------------------------- */

static void idling_report_threshold_exceeded(const vehicle_sample_t *sample, float elapsed_sec)
{
    ESP_LOGW(TAG, "Excessive idling detected: %.0f seconds, RPM=%u",
             elapsed_sec, sample->engine_rpm);

    cJSON *root = cJSON_CreateObject();
    if (root != NULL) {
        cJSON_AddStringToObject(root, "device_id", mqtt_client_get_device_id());
        cJSON_AddNumberToObject(root, "timestamp_ms", (double)(sample->timestamp_us / 1000));
        cJSON_AddStringToObject(root, "alert_type", "excessive_idling");
        cJSON_AddNumberToObject(root, "duration_seconds", elapsed_sec);
        cJSON_AddNumberToObject(root, "engine_rpm", sample->engine_rpm);
        if (sample->gps_fix_valid) {
            cJSON_AddNumberToObject(root, "latitude_deg", sample->latitude_deg);
            cJSON_AddNumberToObject(root, "longitude_deg", sample->longitude_deg);
        }

        char *json_str = cJSON_PrintUnformatted(root);
        if (json_str != NULL) {
            esp_err_t err = mqtt_client_publish_alert(json_str);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to enqueue idling alert: %s", esp_err_to_name(err));
            }
            cJSON_free(json_str);
        } else {
            ESP_LOGE(TAG, "Failed to serialize idling alert payload");
        }
        cJSON_Delete(root);
    } else {
        ESP_LOGE(TAG, "Failed to allocate cJSON object for idling alert");
    }
}

/* ---------------------------------------------------------------------------
 * Background Task
 * ------------------------------------------------------------------------- */

static void idling_detection_task(void *arg)
{
    (void)arg;

    bool idling_active = false;
    bool threshold_crossed = false;
    int64_t idle_start_time_us = 0;

    ESP_LOGI(TAG, "Idling detection task started (threshold=%ds, RPM>=%d, speed<=%.1fkm/h)",
             IDLING_DURATION_THRESHOLD_SEC, IDLING_RPM_THRESHOLD, IDLING_SPEED_THRESHOLD_KMH);

    for (;;) {
        vehicle_sample_t sample;
        esp_err_t sample_err = sensor_manager_get_latest(&sample);

        if (sample_err == ESP_OK && sample.engine_rpm_valid &&
            (sample.obd_speed_valid || sample.gps_fix_valid)) {

            float speed_kmh = sample.obd_speed_valid ? sample.obd_speed_kmh : sample.gps_speed_kmh;
            bool is_idling_now = (sample.engine_rpm >= IDLING_RPM_THRESHOLD) &&
                                  (speed_kmh <= IDLING_SPEED_THRESHOLD_KMH);

            int64_t now_us = esp_timer_get_time();

            if (is_idling_now && !idling_active) {
                /* New idle session begins. */
                idling_active = true;
                threshold_crossed = false;
                idle_start_time_us = now_us;
            } else if (!is_idling_now && idling_active) {
                /* Idle session ends (vehicle moved or engine stopped). */
                idling_active = false;
                if (threshold_crossed) {
                    float total_sec = (float)(now_us - idle_start_time_us) / 1000000.0f;
                    ESP_LOGI(TAG, "Idle session ended after %.0f seconds total", total_sec);
                }
            }

            if (idling_active) {
                float elapsed_sec = (float)(now_us - idle_start_time_us) / 1000000.0f;

                if (elapsed_sec >= (float)IDLING_DURATION_THRESHOLD_SEC) {
                    if (!threshold_crossed) {
                        threshold_crossed = true;
                        idling_report_threshold_exceeded(&sample, elapsed_sec);
                    }

                    /* Continuously feed driver_score in small increments
                     * matching the check interval, so the cumulative
                     * idling penalty tracks actual wasted time rather
                     * than a single flat deduction. */
                    driver_event_t evt = {
                        .type = DRIVER_EVENT_IDLING,
                        .magnitude = (float)IDLING_CHECK_PERIOD_MS / 1000.0f,
                        .timestamp_us = now_us,
                    };
                    esp_err_t err = driver_score_submit_event(&evt);
                    if (err != ESP_OK) {
                        ESP_LOGW(TAG, "Failed to submit idling event to driver_score: %s",
                                 esp_err_to_name(err));
                    }
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(IDLING_CHECK_PERIOD_MS));
    }
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

esp_err_t idling_detection_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    BaseType_t task_created = xTaskCreatePinnedToCore(
        idling_detection_task, "idling_detection_task", TASK_STACK_SIZE_IDLING, NULL,
        TASK_PRIORITY_IDLING_DETECTION, NULL, TASK_CORE_PROCESSING);

    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create idling detection task");
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Idling detection module initialized");
    return ESP_OK;
}
