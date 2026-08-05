/* ============================================================================
 * pid_generator.c
 *
 * Implementation of the telemetry model declared in pid_generator.h.
 *
 * Each of the four signals is tracked as an independent "ramp field":
 * a start value, a target value, and a start time + duration over which
 * an eased (smoothstep, not linear) blend from start to target is
 * computed. A single background task recomputes all four fields every
 * PID_TICK_PERIOD_MS, layers a small bounded noise sample on top of the
 * eased value (real sensors are never perfectly smooth even holding a
 * steady target), clamps to each signal's physically valid range, and
 * publishes the result for pid_generator_get_current() to read.
 *
 * Smoothstep (t*t*(3-2t)) rather than linear interpolation is used
 * deliberately: linear ramps have a velocity discontinuity at the start
 * and end of every transition (an instant jump from "not moving" to
 * "moving at constant rate" and back), which is exactly the kind of
 * jump the project's requirements explicitly call out to avoid.
 * Smoothstep's velocity is zero at both endpoints, so consecutive
 * waypoint transitions from scenario_manager chain together without any
 * visible kink.
 * ========================================================================= */

#include <string.h>
#include <math.h>
#include "pid_generator.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_log.h"

static const char *TAG = "pid_generator";

#define PID_TICK_PERIOD_MS      100     /* 10 Hz - smooth enough for eased
                                            transitions, cheap enough to run
                                            continuously */
#define PID_TICK_TASK_STACK     3072
#define PID_TICK_TASK_PRIORITY  4

/* Bounded per-tick jitter amplitudes, layered on top of the eased base
 * value. Chosen to be visible on a live telemetry log without being
 * large enough to look like noise rather than a real sensor. */
#define NOISE_RPM_AMPLITUDE       12.0f     /* +/- RPM */
#define NOISE_SPEED_AMPLITUDE     0.25f     /* +/- km/h */
#define NOISE_COOLANT_AMPLITUDE   0.15f     /* +/- degC */
#define NOISE_THROTTLE_AMPLITUDE  0.4f      /* +/- percent */

/* Physically valid output ranges (also the standard OBD-II Mode 01
 * encodable ranges for these PIDs). */
#define RPM_MIN       0.0f
#define RPM_MAX       8000.0f
#define SPEED_MIN     0.0f
#define SPEED_MAX     255.0f
#define COOLANT_MIN   -40.0f
#define COOLANT_MAX   215.0f
#define THROTTLE_MIN  0.0f
#define THROTTLE_MAX  100.0f

/* ---------------------------------------------------------------------------
 * Ramp Field - one independent smoothly-transitioning signal
 * ------------------------------------------------------------------------- */

typedef struct {
    float ramp_start;         /*!< Value this field was at when the current
                                    transition began */
    float ramp_target;        /*!< Value this field is transitioning toward */
    int64_t ramp_start_us;    /*!< esp_timer time the transition began */
    int64_t ramp_duration_us; /*!< Total duration of the transition */
    float base_current;       /*!< Eased value as of the last tick, BEFORE
                                    noise - this is what re-targeting starts
                                    from, so noise never compounds into the
                                    ramp math itself */
} ramp_field_t;

static ramp_field_t s_rpm_field;
static ramp_field_t s_speed_field;
static ramp_field_t s_coolant_field;
static ramp_field_t s_throttle_field;

static SemaphoreHandle_t s_state_mutex = NULL;
static pid_values_t s_output;   /* Latest published (eased + noise + clamped) values */
static bool s_initialized = false;

/* ---------------------------------------------------------------------------
 * Ramp Math Helpers - caller must hold s_state_mutex
 * ------------------------------------------------------------------------- */

/**
 * @brief Recompute a field's eased base value for the given time and
 *        store it in field->base_current. Does not apply noise or
 *        clamping - those are output-stage concerns applied once by the
 *        tick task, not duplicated in every helper.
 */
static float ramp_field_advance_locked(ramp_field_t *field, int64_t now_us)
{
    if (field->ramp_duration_us <= 0) {
        field->base_current = field->ramp_target;
        return field->base_current;
    }

    int64_t elapsed_us = now_us - field->ramp_start_us;
    float t = (float)elapsed_us / (float)field->ramp_duration_us;
    if (t < 0.0f) {
        t = 0.0f;
    } else if (t > 1.0f) {
        t = 1.0f;
    }

    /* Smoothstep easing - see file header comment for why this is used
     * instead of a plain linear blend. */
    float eased = t * t * (3.0f - 2.0f * t);

    field->base_current = field->ramp_start + (field->ramp_target - field->ramp_start) * eased;
    return field->base_current;
}

/**
 * @brief Begin a new transition for one field, starting from wherever
 *        it actually is right now (not from the previous target).
 */
static void ramp_field_set_target_locked(ramp_field_t *field, float new_target,
                                          float ramp_seconds, int64_t now_us)
{
    /* Bring base_current fully up to date first, so the new ramp starts
     * from the real current position rather than a stale one - this is
     * exactly what prevents a discontinuous jump when re-targeting
     * mid-transition. */
    ramp_field_advance_locked(field, now_us);

    field->ramp_start = field->base_current;
    field->ramp_target = new_target;
    field->ramp_start_us = now_us;

    int64_t duration_us = (int64_t)(ramp_seconds * 1000000.0f);
    int64_t floor_us = (int64_t)PID_TICK_PERIOD_MS * 1000;
    field->ramp_duration_us = (duration_us > floor_us) ? duration_us : floor_us;
}

/**
 * @brief Immediately snap a field to a value with no transition at all -
 *        used only at init, to seed a sane starting state.
 */
static void ramp_field_init_locked(ramp_field_t *field, float value)
{
    field->ramp_start = value;
    field->ramp_target = value;
    field->ramp_start_us = esp_timer_get_time();
    field->ramp_duration_us = (int64_t)PID_TICK_PERIOD_MS * 1000;
    field->base_current = value;
}

/**
 * @brief Generate a symmetric noise sample in [-amplitude, +amplitude]
 *        using the ESP32's hardware RNG.
 */
static float noise_sample(float amplitude)
{
    /* esp_random() returns a full-range uint32_t; map to [-1, 1] then
     * scale. Precision here is overkill for cosmetic sensor jitter but
     * costs nothing. */
    int32_t r = (int32_t)(esp_random() % 20001) - 10000; /* [-10000, 10000] */
    float unit = (float)r / 10000.0f;                    /* [-1.0, 1.0] */
    return unit * amplitude;
}

static float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* ---------------------------------------------------------------------------
 * Background Tick Task
 * ------------------------------------------------------------------------- */

static void pid_generator_tick_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "PID generator tick task started (period=%dms)", PID_TICK_PERIOD_MS);

    for (;;) {
        int64_t now_us = esp_timer_get_time();

        if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            float rpm_base = ramp_field_advance_locked(&s_rpm_field, now_us);
            float speed_base = ramp_field_advance_locked(&s_speed_field, now_us);
            float coolant_base = ramp_field_advance_locked(&s_coolant_field, now_us);
            float throttle_base = ramp_field_advance_locked(&s_throttle_field, now_us);

            float rpm_out = clampf(rpm_base + noise_sample(NOISE_RPM_AMPLITUDE), RPM_MIN, RPM_MAX);
            float speed_out = clampf(speed_base + noise_sample(NOISE_SPEED_AMPLITUDE),
                                      SPEED_MIN, SPEED_MAX);
            float coolant_out = clampf(coolant_base + noise_sample(NOISE_COOLANT_AMPLITUDE),
                                        COOLANT_MIN, COOLANT_MAX);
            float throttle_out = clampf(throttle_base + noise_sample(NOISE_THROTTLE_AMPLITUDE),
                                         THROTTLE_MIN, THROTTLE_MAX);

            s_output.engine_rpm = (uint16_t)(rpm_out + 0.5f);
            s_output.vehicle_speed_kmh = speed_out;
            s_output.coolant_temp_c = (int16_t)(coolant_out >= 0 ? coolant_out + 0.5f
                                                                  : coolant_out - 0.5f);
            s_output.throttle_position_pct = throttle_out;

            xSemaphoreGive(s_state_mutex);
        } else {
            ESP_LOGW(TAG, "Timed out acquiring state mutex in tick task");
        }

        vTaskDelay(pdMS_TO_TICKS(PID_TICK_PERIOD_MS));
    }
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

esp_err_t pid_generator_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_state_mutex = xSemaphoreCreateMutex();
    if (s_state_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create state mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Seed every field at a plausible warm-idle state so the very first
     * PID request (which can arrive within milliseconds of BLE connect,
     * before scenario_manager has set any real target) already returns
     * something sane rather than all-zero. */
    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        ramp_field_init_locked(&s_rpm_field, 800.0f);
        ramp_field_init_locked(&s_speed_field, 0.0f);
        ramp_field_init_locked(&s_coolant_field, 90.0f);
        ramp_field_init_locked(&s_throttle_field, 0.0f);

        s_output.engine_rpm = 800;
        s_output.vehicle_speed_kmh = 0.0f;
        s_output.coolant_temp_c = 90;
        s_output.throttle_position_pct = 0.0f;

        xSemaphoreGive(s_state_mutex);
    }

    BaseType_t task_created = xTaskCreate(
        pid_generator_tick_task, "pid_gen_tick_task",
        PID_TICK_TASK_STACK, NULL, PID_TICK_TASK_PRIORITY, NULL);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create tick task");
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "PID generator initialized (warm idle defaults)");
    return ESP_OK;
}

void pid_generator_get_current(pid_values_t *out)
{
    if (out == NULL || !s_initialized) {
        return;
    }

    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        memcpy(out, &s_output, sizeof(pid_values_t));
        xSemaphoreGive(s_state_mutex);
    } else {
        ESP_LOGW(TAG, "Timed out acquiring state mutex, returning zeroed sample");
        memset(out, 0, sizeof(pid_values_t));
    }
}

void pid_generator_set_target(const pid_values_t *target, float ramp_seconds)
{
    if (target == NULL || !s_initialized) {
        return;
    }

    int64_t now_us = esp_timer_get_time();

    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        ramp_field_set_target_locked(&s_rpm_field, (float)target->engine_rpm,
                                      ramp_seconds, now_us);
        ramp_field_set_target_locked(&s_speed_field, target->vehicle_speed_kmh,
                                      ramp_seconds, now_us);
        ramp_field_set_target_locked(&s_coolant_field, (float)target->coolant_temp_c,
                                      ramp_seconds, now_us);
        ramp_field_set_target_locked(&s_throttle_field, target->throttle_position_pct,
                                      ramp_seconds, now_us);
        xSemaphoreGive(s_state_mutex);
    } else {
        ESP_LOGW(TAG, "Timed out acquiring state mutex, target not applied");
    }
}
