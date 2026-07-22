/* ============================================================================
 * driver_score.c
 *
 * Implementation of the driver safety scoring engine declared in
 * driver_score.h.
 *
 * The score is cumulative-trip: it starts at 100.0 and is decremented by
 * weighted penalties as events accumulate over the life of the device's
 * uptime, recomputed and republished every DRIVER_SCORE_CALC_PERIOD_MS.
 * This is a deliberate design choice over a rolling/windowed score - a
 * fleet operator reviewing a completed trip wants a single number
 * reflecting the whole trip, not a value that "forgives" earlier events
 * as time passes.
 *
 * Event weights (points deducted) are policy constants specific to this
 * scoring algorithm and are kept local to this file rather than in
 * config.h, which is reserved for hardware/timing/threshold constants
 * shared across modules.
 * ========================================================================= */

#include <string.h>
#include <stdlib.h>
#include "driver_score.h"
#include "config.h"
#include "sensor_manager.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "driver_score";

#define QUEUE_LEN_DRIVER_EVENTS         20
#define SCORE_TASK_LOOP_DELAY_MS        100
#define SCORE_MIN                       0.0f
#define SCORE_MAX                       100.0f

/* Scoring policy: points deducted per event. Chosen so that a genuinely
 * dangerous trip (frequent harsh events, sustained overspeeding, or any
 * crash) visibly and quickly drags the score down, while a small number
 * of incidental events does not overly punish an otherwise safe trip. */
#define WEIGHT_HARSH_BRAKING_PTS        2.0f
#define WEIGHT_HARSH_ACCEL_PTS          2.0f
#define WEIGHT_HARSH_CORNERING_PTS      1.5f
#define WEIGHT_OVERSPEED_PTS            3.0f
#define WEIGHT_IDLING_PTS_PER_MINUTE    0.05f
#define WEIGHT_CRASH_PTS                50.0f
#define WEIGHT_GEOFENCE_PTS             5.0f

typedef struct {
    uint32_t harsh_braking_count;
    uint32_t harsh_accel_count;
    uint32_t harsh_cornering_count;
    uint32_t overspeed_count;
    uint32_t crash_count;
    uint32_t geofence_violation_count;
    float idling_seconds_total;
    float current_score;
} driver_score_state_t;

static SemaphoreHandle_t s_state_mutex = NULL;
static QueueHandle_t s_event_queue = NULL;
static driver_score_state_t s_state = {
    .harsh_braking_count = 0,
    .harsh_accel_count = 0,
    .harsh_cornering_count = 0,
    .overspeed_count = 0,
    .crash_count = 0,
    .geofence_violation_count = 0,
    .idling_seconds_total = 0.0f,
    .current_score = 100.0f,
};
static bool s_initialized = false;

/* Edge-detection state for internally-detected harsh events. These are
 * only ever touched by driver_score_task, so no mutex is required. */
static bool s_braking_active = false;
static bool s_accel_active = false;
static bool s_cornering_active = false;
static bool s_overspeed_active = false;

/* ---------------------------------------------------------------------------
 * Scoring State Updates
 * ------------------------------------------------------------------------- */

/**
 * @brief Apply one event's effect on the cumulative counters. Caller
 *        must hold s_state_mutex.
 */
static void driver_score_apply_event_locked(const driver_event_t *event)
{
    switch (event->type) {
        case DRIVER_EVENT_HARSH_BRAKING:
            s_state.harsh_braking_count++;
            break;
        case DRIVER_EVENT_HARSH_ACCELERATION:
            s_state.harsh_accel_count++;
            break;
        case DRIVER_EVENT_HARSH_CORNERING:
            s_state.harsh_cornering_count++;
            break;
        case DRIVER_EVENT_OVERSPEED:
            s_state.overspeed_count++;
            break;
        case DRIVER_EVENT_IDLING:
            s_state.idling_seconds_total += event->magnitude;
            break;
        case DRIVER_EVENT_CRASH:
            s_state.crash_count++;
            break;
        case DRIVER_EVENT_GEOFENCE_VIOLATION:
            s_state.geofence_violation_count++;
            break;
        default:
            ESP_LOGW(TAG, "Unknown driver_event_type_t value: %d", (int)event->type);
            break;
    }
}

/**
 * @brief Recompute s_state.current_score from the current cumulative
 *        counters. Caller must hold s_state_mutex.
 */
static void driver_score_recompute_locked(void)
{
    float penalty =
        (s_state.harsh_braking_count * WEIGHT_HARSH_BRAKING_PTS) +
        (s_state.harsh_accel_count * WEIGHT_HARSH_ACCEL_PTS) +
        (s_state.harsh_cornering_count * WEIGHT_HARSH_CORNERING_PTS) +
        (s_state.overspeed_count * WEIGHT_OVERSPEED_PTS) +
        ((s_state.idling_seconds_total / 60.0f) * WEIGHT_IDLING_PTS_PER_MINUTE) +
        (s_state.crash_count * WEIGHT_CRASH_PTS) +
        (s_state.geofence_violation_count * WEIGHT_GEOFENCE_PTS);

    float score = SCORE_MAX - penalty;
    if (score < SCORE_MIN) {
        score = SCORE_MIN;
    } else if (score > SCORE_MAX) {
        score = SCORE_MAX;
    }
    s_state.current_score = score;
}

/**
 * @brief Detect harsh braking/acceleration/cornering/overspeed from a raw
 *        sample using edge-triggered threshold comparisons, so a
 *        sustained hard-braking event counts once, not once per sample.
 *        Detected events are applied directly to the cumulative counters.
 */
static void driver_score_detect_harsh_events(const vehicle_sample_t *sample)
{
    /* Longitudinal deceleration -> harsh braking. */
    bool braking_now = (sample->accel_x_g <= -THRESHOLD_HARSH_BRAKING_G);
    if (braking_now && !s_braking_active) {
        driver_event_t evt = {
            .type = DRIVER_EVENT_HARSH_BRAKING,
            .magnitude = -sample->accel_x_g,
            .timestamp_us = sample->timestamp_us,
        };
        if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(MUTEX_MAX_WAIT_MS)) == pdTRUE) {
            driver_score_apply_event_locked(&evt);
            xSemaphoreGive(s_state_mutex);
            ESP_LOGI(TAG, "Harsh braking detected: %.2f g", evt.magnitude);
        }
    }
    s_braking_active = braking_now;

    /* Longitudinal acceleration -> harsh acceleration. */
    bool accel_now = (sample->accel_x_g >= THRESHOLD_HARSH_ACCEL_G);
    if (accel_now && !s_accel_active) {
        driver_event_t evt = {
            .type = DRIVER_EVENT_HARSH_ACCELERATION,
            .magnitude = sample->accel_x_g,
            .timestamp_us = sample->timestamp_us,
        };
        if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(MUTEX_MAX_WAIT_MS)) == pdTRUE) {
            driver_score_apply_event_locked(&evt);
            xSemaphoreGive(s_state_mutex);
            ESP_LOGI(TAG, "Harsh acceleration detected: %.2f g", evt.magnitude);
        }
    }
    s_accel_active = accel_now;

    /* Lateral acceleration -> harsh cornering. */
    bool cornering_now = (sample->accel_y_g >= THRESHOLD_HARSH_CORNERING_G ||
                           sample->accel_y_g <= -THRESHOLD_HARSH_CORNERING_G);
    if (cornering_now && !s_cornering_active) {
        driver_event_t evt = {
            .type = DRIVER_EVENT_HARSH_CORNERING,
            .magnitude = sample->accel_y_g,
            .timestamp_us = sample->timestamp_us,
        };
        if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(MUTEX_MAX_WAIT_MS)) == pdTRUE) {
            driver_score_apply_event_locked(&evt);
            xSemaphoreGive(s_state_mutex);
            ESP_LOGI(TAG, "Harsh cornering detected: %.2f g", evt.magnitude);
        }
    }
    s_cornering_active = cornering_now;

    /* Vehicle speed -> overspeed. Prefer OBD speed (direct wheel-derived
     * reading); fall back to GPS speed only if OBD is unavailable. */
    bool have_speed = sample->obd_speed_valid || sample->gps_fix_valid;
    float speed_kmh = sample->obd_speed_valid ? sample->obd_speed_kmh : sample->gps_speed_kmh;

    bool overspeed_now = have_speed && (speed_kmh >= THRESHOLD_OVERSPEED_KMH);
    if (overspeed_now && !s_overspeed_active) {
        driver_event_t evt = {
            .type = DRIVER_EVENT_OVERSPEED,
            .magnitude = speed_kmh - THRESHOLD_OVERSPEED_KMH,
            .timestamp_us = sample->timestamp_us,
        };
        if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(MUTEX_MAX_WAIT_MS)) == pdTRUE) {
            driver_score_apply_event_locked(&evt);
            xSemaphoreGive(s_state_mutex);
            ESP_LOGI(TAG, "Overspeed detected: %.1f km/h (%.1f over limit)",
                     speed_kmh, evt.magnitude);
        }
    }
    s_overspeed_active = overspeed_now;
}

/* ---------------------------------------------------------------------------
 * Score Publishing
 * ------------------------------------------------------------------------- */

static void driver_score_publish(void)
{
    driver_score_state_t snapshot;

    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(MUTEX_MAX_WAIT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Timed out acquiring state mutex for publish");
        return;
    }
    driver_score_recompute_locked();
    memcpy(&snapshot, &s_state, sizeof(driver_score_state_t));
    xSemaphoreGive(s_state_mutex);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to allocate cJSON object for score payload");
        return;
    }

    cJSON_AddStringToObject(root, "device_id", mqtt_client_get_device_id());
    cJSON_AddNumberToObject(root, "timestamp_ms", (double)(esp_timer_get_time() / 1000));
    cJSON_AddNumberToObject(root, "score", snapshot.current_score);
    cJSON_AddNumberToObject(root, "harsh_braking_count", snapshot.harsh_braking_count);
    cJSON_AddNumberToObject(root, "harsh_accel_count", snapshot.harsh_accel_count);
    cJSON_AddNumberToObject(root, "harsh_cornering_count", snapshot.harsh_cornering_count);
    cJSON_AddNumberToObject(root, "overspeed_count", snapshot.overspeed_count);
    cJSON_AddNumberToObject(root, "idling_seconds_total", snapshot.idling_seconds_total);
    cJSON_AddNumberToObject(root, "crash_count", snapshot.crash_count);
    cJSON_AddNumberToObject(root, "geofence_violation_count", snapshot.geofence_violation_count);

    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str != NULL) {
        esp_err_t err = mqtt_client_publish_score(json_str);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to enqueue score publish: %s", esp_err_to_name(err));
        }
        cJSON_free(json_str);
    } else {
        ESP_LOGE(TAG, "Failed to serialize score payload");
    }

    cJSON_Delete(root);
}

/* ---------------------------------------------------------------------------
 * Background Task
 * ------------------------------------------------------------------------- */

static void driver_score_task(void *arg)
{
    (void)arg;

    int64_t last_calc_time_us = esp_timer_get_time();

    ESP_LOGI(TAG, "Driver score task started");

    for (;;) {
        /* Drain all externally-submitted events without blocking, so the
         * periodic sample check below still runs on schedule. */
        driver_event_t evt;
        while (xQueueReceive(s_event_queue, &evt, 0) == pdTRUE) {
            if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(MUTEX_MAX_WAIT_MS)) == pdTRUE) {
                driver_score_apply_event_locked(&evt);
                xSemaphoreGive(s_state_mutex);
            } else {
                ESP_LOGE(TAG, "Timed out acquiring state mutex for external event");
            }
        }

        vehicle_sample_t sample;
        if (sensor_manager_get_latest(&sample) == ESP_OK) {
            driver_score_detect_harsh_events(&sample);
        }

        int64_t now_us = esp_timer_get_time();
        if ((now_us - last_calc_time_us) >= ((int64_t)DRIVER_SCORE_CALC_PERIOD_MS * 1000)) {
            driver_score_publish();
            last_calc_time_us = now_us;
        }

        vTaskDelay(pdMS_TO_TICKS(SCORE_TASK_LOOP_DELAY_MS));
    }
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

esp_err_t driver_score_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_state_mutex = xSemaphoreCreateMutex();
    if (s_state_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create state mutex");
        return ESP_ERR_NO_MEM;
    }

    s_event_queue = xQueueCreate(QUEUE_LEN_DRIVER_EVENTS, sizeof(driver_event_t));
    if (s_event_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create event queue");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t task_created = xTaskCreatePinnedToCore(
        driver_score_task, "driver_score_task", TASK_STACK_SIZE_DRIVER_SCORE, NULL,
        TASK_PRIORITY_DRIVER_SCORE, NULL, TASK_CORE_PROCESSING);

    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create driver score task");
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Driver score module initialized");
    return ESP_OK;
}

esp_err_t driver_score_submit_event(const driver_event_t *event)
{
    if (event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xQueueSend(s_event_queue, event, pdMS_TO_TICKS(MUTEX_MAX_WAIT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Event queue full, dropping event type %d", (int)event->type);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t driver_score_get_current(float *score_out)
{
    if (score_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(MUTEX_MAX_WAIT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Timed out acquiring state mutex in driver_score_get_current");
        return ESP_ERR_TIMEOUT;
    }
    *score_out = s_state.current_score;
    xSemaphoreGive(s_state_mutex);

    return ESP_OK;
}
