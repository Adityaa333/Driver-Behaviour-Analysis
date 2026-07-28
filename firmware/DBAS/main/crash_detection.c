/* ============================================================================
 * crash_detection.c
 *
 * Implementation of the crash detection module declared in
 * crash_detection.h.
 *
 * A crash is confirmed when either the total (vector-magnitude)
 * acceleration or total angular rate exceeds its respective threshold
 * from config.h. After a confirmed crash, detection is suppressed for
 * CRASH_COOLDOWN_MS so that the violent post-impact vibration/bouncing
 * of a real crash does not generate a flood of duplicate reports for
 * what is a single event.
 * ========================================================================= */

#include <math.h>
#include "crash_detection.h"
#include "config.h"
#include "sensor_manager.h"
#include "driver_score.h"
#include "dbas_mqtt_client.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "crash_detection";

#define CRASH_TASK_LOOP_DELAY_MS        50      /* ~2x MPU6050 sample rate */
#define CRASH_COOLDOWN_MS               10000   /* Suppress duplicates for 10s */

static bool s_initialized = false;

/* ---------------------------------------------------------------------------
 * Detection
 * ------------------------------------------------------------------------- */

/**
 * @brief Evaluate whether a sample represents a confirmed crash impact.
 *
 * @param[in]  sample         Latest merged vehicle sample.
 * @param[out] out_total_accel_g Vector-magnitude acceleration, in g.
 * @param[out] out_total_gyro_dps Vector-magnitude angular rate, in deg/s.
 * @return true if either threshold is exceeded.
 */
static bool crash_is_confirmed(const vehicle_sample_t *sample,
                                float *out_total_accel_g, float *out_total_gyro_dps)
{
    float total_accel_g = sqrtf((sample->accel_x_g * sample->accel_x_g) +
                                 (sample->accel_y_g * sample->accel_y_g) +
                                 (sample->accel_z_g * sample->accel_z_g));

    float total_gyro_dps = sqrtf((sample->gyro_x_dps * sample->gyro_x_dps) +
                                  (sample->gyro_y_dps * sample->gyro_y_dps) +
                                  (sample->gyro_z_dps * sample->gyro_z_dps));

    *out_total_accel_g = total_accel_g;
    *out_total_gyro_dps = total_gyro_dps;

    return (total_accel_g >= THRESHOLD_CRASH_ACCEL_G) ||
           (total_gyro_dps >= THRESHOLD_CRASH_GYRO_DPS);
}

/* ---------------------------------------------------------------------------
 * Reporting
 * ------------------------------------------------------------------------- */

/**
 * @brief Report a confirmed crash: publish a detailed MQTT crash alert
 *        (highest QoS in the system) and notify driver_score.
 */
static void crash_detection_report(const vehicle_sample_t *sample,
                                    float total_accel_g, float total_gyro_dps)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to allocate cJSON object for crash payload");
    } else {
        cJSON_AddStringToObject(root, "device_id", mqtt_client_get_device_id());
        cJSON_AddNumberToObject(root, "timestamp_ms", (double)(sample->timestamp_us / 1000));
        cJSON_AddNumberToObject(root, "total_accel_g", total_accel_g);
        cJSON_AddNumberToObject(root, "total_gyro_dps", total_gyro_dps);
        cJSON_AddNumberToObject(root, "accel_x_g", sample->accel_x_g);
        cJSON_AddNumberToObject(root, "accel_y_g", sample->accel_y_g);
        cJSON_AddNumberToObject(root, "accel_z_g", sample->accel_z_g);
        cJSON_AddNumberToObject(root, "gyro_x_dps", sample->gyro_x_dps);
        cJSON_AddNumberToObject(root, "gyro_y_dps", sample->gyro_y_dps);
        cJSON_AddNumberToObject(root, "gyro_z_dps", sample->gyro_z_dps);
        cJSON_AddBoolToObject(root, "gps_fix_valid", sample->gps_fix_valid);
        cJSON_AddNumberToObject(root, "latitude_deg", sample->latitude_deg);
        cJSON_AddNumberToObject(root, "longitude_deg", sample->longitude_deg);
        cJSON_AddNumberToObject(root, "speed_kmh", sample->gps_speed_kmh);
        cJSON_AddNumberToObject(root, "heading_deg", sample->heading_deg);

        char *json_str = cJSON_PrintUnformatted(root);
        if (json_str != NULL) {
            esp_err_t err = mqtt_client_publish_crash(json_str);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to enqueue crash publish: %s", esp_err_to_name(err));
            }
            cJSON_free(json_str);
        } else {
            ESP_LOGE(TAG, "Failed to serialize crash payload");
        }
        cJSON_Delete(root);
    }

    driver_event_t evt = {
        .type = DRIVER_EVENT_CRASH,
        .magnitude = total_accel_g,
        .timestamp_us = sample->timestamp_us,
    };
    esp_err_t err = driver_score_submit_event(&evt);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to submit crash event to driver_score: %s", esp_err_to_name(err));
    }
}

/* ---------------------------------------------------------------------------
 * Background Task
 * ------------------------------------------------------------------------- */

static void crash_detection_task(void *arg)
{
    (void)arg;

    int64_t cooldown_until_us = 0;

    ESP_LOGI(TAG, "Crash detection task started");

    for (;;) {
        vehicle_sample_t sample;
        if (sensor_manager_get_latest(&sample) == ESP_OK) {
            int64_t now_us = esp_timer_get_time();

            if (now_us >= cooldown_until_us) {
                float total_accel_g = 0.0f;
                float total_gyro_dps = 0.0f;

                if (crash_is_confirmed(&sample, &total_accel_g, &total_gyro_dps)) {
                    ESP_LOGE(TAG, "CRASH DETECTED: total_accel=%.2fg total_gyro=%.1fdps",
                             total_accel_g, total_gyro_dps);
                    crash_detection_report(&sample, total_accel_g, total_gyro_dps);
                    cooldown_until_us = now_us + ((int64_t)CRASH_COOLDOWN_MS * 1000);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(CRASH_TASK_LOOP_DELAY_MS));
    }
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

esp_err_t crash_detection_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    BaseType_t task_created = xTaskCreatePinnedToCore(
        crash_detection_task, "crash_detection_task", TASK_STACK_SIZE_CRASH_DETECTION, NULL,
        TASK_PRIORITY_CRASH_DETECTION, NULL, TASK_CORE_PROCESSING);

    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create crash detection task");
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Crash detection module initialized (accel>=%.1fg or gyro>=%.0fdps)",
             THRESHOLD_CRASH_ACCEL_G, THRESHOLD_CRASH_GYRO_DPS);
    return ESP_OK;
}
