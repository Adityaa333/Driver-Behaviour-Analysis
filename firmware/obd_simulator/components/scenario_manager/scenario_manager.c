/* ============================================================================
 * scenario_manager.c
 *
 * Implementation of the scenario switching/generation logic declared in
 * scenario_manager.h.
 *
 * Three background tasks:
 *
 *   - scenario_waypoint_task: the actual "driving scenario" logic. Every
 *     few seconds (interval depends on the active scenario), computes a
 *     new target pid_values_t "waypoint" appropriate to that scenario
 *     (e.g. City Driving alternates stopped/moving; Aggressive alternates
 *     hard-accel/hard-brake spikes; Engine Fault slowly creeps coolant
 *     temperature upward while RPM surges erratically) and hands it to
 *     pid_generator_set_target() along with a ramp duration that itself
 *     characterizes the scenario (fast harsh ramps for Aggressive, slow
 *     gentle ramps for Idle/Highway). Reacts to a scenario change within
 *     ~200ms rather than waiting out the previous scenario's stale
 *     interval.
 *   - scenario_console_task: reads scenario selections typed into the
 *     serial monitor.
 *   - scenario_button_task: polls one GPIO (mirroring the polling style
 *     DBAS's own led_indicator.c already uses for GPIO, rather than
 *     introducing an ISR-driven pattern this project doesn't otherwise
 *     use) and cycles forward through scenarios on each debounced press.
 * ========================================================================= */

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>
#include "scenario_manager.h"
#include "pid_generator.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_log.h"

static const char *TAG = "scenario_manager";

/* ---------------------------------------------------------------------------
 * Configuration
 *
 * This is a standalone test-tool project (not part of DBAS), so these
 * live here directly rather than in a shared config.h - there is
 * exactly one place in the codebase that would ever read them.
 * ------------------------------------------------------------------------- */

/* GPIO0 is the "BOOT" button already present on essentially every
 * ESP32-WROOM devkit, wired active-low with an external pull-up - using
 * it means "push-button scenario switching" works with zero extra
 * wiring on the most common dev boards. Move to a different GPIO here
 * if you'd rather free up BOOT for its usual role. */
#define SCENARIO_BUTTON_GPIO         GPIO_NUM_0
#define SCENARIO_BUTTON_POLL_MS      50
#define SCENARIO_BUTTON_DEBOUNCE_MS  250

#define SCENARIO_WAYPOINT_TASK_STACK  3072
#define SCENARIO_CONSOLE_TASK_STACK   4096
#define SCENARIO_BUTTON_TASK_STACK    2560
#define SCENARIO_TASK_PRIORITY        4

/* How often the waypoint-wait loop wakes up to check for a forced
 * re-evaluation (scenario changed via console/button). Small relative
 * to any scenario's waypoint interval, so switching feels immediate. */
#define SCENARIO_WAIT_POLL_MS         200

static const char *const s_scenario_names[SCENARIO_COUNT] = {
    [SCENARIO_IDLE]          = "Idle",
    [SCENARIO_CITY_DRIVING]  = "City Driving",
    [SCENARIO_HIGHWAY]       = "Highway",
    [SCENARIO_AGGRESSIVE]    = "Aggressive Driving",
    [SCENARIO_ENGINE_FAULT]  = "Engine Fault",
};

static volatile scenario_type_t s_current_scenario = SCENARIO_IDLE;
static volatile bool s_force_new_waypoint = true; /* Trigger a waypoint immediately at startup */
static bool s_initialized = false;

/* ---------------------------------------------------------------------------
 * Random Helper
 * ------------------------------------------------------------------------- */

static float randf(float min_val, float max_val)
{
    float unit = (float)esp_random() / (float)UINT32_MAX; /* [0.0, 1.0] */
    return min_val + unit * (max_val - min_val);
}

/* ---------------------------------------------------------------------------
 * Per-Scenario Waypoint Generators
 *
 * Each returns the number of seconds to wait before the NEXT waypoint
 * (itself randomized within a scenario-appropriate range, so the
 * simulated drive doesn't feel mechanically periodic), and writes the
 * new target values plus the ramp duration to use reaching them.
 *
 * A little per-scenario static state (phase toggles, a slow-creeping
 * fault temperature) lives next to its generator function rather than
 * in one shared struct, since each scenario's "memory" is unrelated to
 * the others' and reset independently whenever that scenario becomes
 * active again (see scenario_manager_set_scenario()).
 * ------------------------------------------------------------------------- */

static void waypoint_reset_idle(void) { /* No persistent state to reset */ }

static float waypoint_idle(pid_values_t *out, float *ramp_seconds)
{
    out->engine_rpm = (uint16_t)randf(750.0f, 850.0f);
    out->vehicle_speed_kmh = 0.0f;
    out->coolant_temp_c = (int16_t)randf(88.0f, 92.0f);
    out->throttle_position_pct = 0.0f;

    *ramp_seconds = randf(2.0f, 4.0f); /* Gentle drift - nothing sudden at idle */
    return randf(3.0f, 6.0f);
}

static bool s_city_moving = false;

static void waypoint_reset_city(void) { s_city_moving = false; }

static float waypoint_city_driving(pid_values_t *out, float *ramp_seconds)
{
    s_city_moving = !s_city_moving;

    if (s_city_moving) {
        out->vehicle_speed_kmh = randf(20.0f, 50.0f);
        out->engine_rpm = (uint16_t)randf(1200.0f, 2200.0f);
        out->throttle_position_pct = randf(15.0f, 35.0f);
        *ramp_seconds = randf(1.5f, 3.0f); /* Pulling away from a stop */
    } else {
        out->vehicle_speed_kmh = 0.0f;
        out->engine_rpm = (uint16_t)randf(750.0f, 850.0f);
        out->throttle_position_pct = 0.0f;
        *ramp_seconds = randf(1.0f, 2.0f); /* Braking to a stop is a bit quicker */
    }
    out->coolant_temp_c = (int16_t)randf(88.0f, 94.0f);

    return randf(4.0f, 8.0f);
}

static void waypoint_reset_highway(void) { /* No persistent state to reset */ }

static float waypoint_highway(pid_values_t *out, float *ramp_seconds)
{
    out->vehicle_speed_kmh = randf(95.0f, 120.0f);
    out->engine_rpm = (uint16_t)randf(2200.0f, 2900.0f);
    out->throttle_position_pct = randf(28.0f, 42.0f);
    out->coolant_temp_c = (int16_t)randf(90.0f, 94.0f);

    *ramp_seconds = randf(4.0f, 7.0f); /* Sustained cruising - minor drift only */
    return randf(8.0f, 15.0f);
}

static bool s_aggressive_accelerating = true;

static void waypoint_reset_aggressive(void) { s_aggressive_accelerating = true; }

static float waypoint_aggressive(pid_values_t *out, float *ramp_seconds)
{
    s_aggressive_accelerating = !s_aggressive_accelerating;

    if (s_aggressive_accelerating) {
        out->vehicle_speed_kmh = randf(90.0f, 140.0f);
        out->engine_rpm = (uint16_t)randf(4000.0f, 6000.0f);
        out->throttle_position_pct = randf(80.0f, 100.0f);
        *ramp_seconds = randf(0.5f, 1.2f); /* Hard acceleration */
    } else {
        out->vehicle_speed_kmh = randf(0.0f, 20.0f);
        out->engine_rpm = (uint16_t)randf(900.0f, 1500.0f);
        out->throttle_position_pct = 0.0f;
        *ramp_seconds = randf(0.4f, 1.0f); /* Hard braking */
    }
    out->coolant_temp_c = (int16_t)randf(90.0f, 97.0f);

    return randf(1.0f, 3.0f);
}

/* Slowly creeps upward across successive waypoints while this scenario
 * stays active, simulating a developing cooling-system fault rather
 * than an instantaneous one. Capped well above normal operating temp
 * but below a value that would itself be physically absurd. */
static float s_fault_coolant_creep_c = 90.0f;

static void waypoint_reset_engine_fault(void) { s_fault_coolant_creep_c = 90.0f; }

static float waypoint_engine_fault(pid_values_t *out, float *ramp_seconds)
{
    s_fault_coolant_creep_c += randf(0.5f, 2.0f);
    if (s_fault_coolant_creep_c > 130.0f) {
        s_fault_coolant_creep_c = 130.0f;
    }

    /* Erratic idle: RPM surges and stumbles rather than holding steady,
     * consistent with e.g. a vacuum leak or failing idle air control -
     * vehicle assumed stationary throughout (this fault signature is
     * about the engine, not about driving). */
    out->engine_rpm = (uint16_t)randf(500.0f, 3000.0f);
    out->vehicle_speed_kmh = 0.0f;
    out->throttle_position_pct = 0.0f;
    out->coolant_temp_c = (int16_t)s_fault_coolant_creep_c;

    *ramp_seconds = randf(0.3f, 1.0f); /* Rough/jittery, not smooth */
    return randf(1.0f, 3.0f);
}

typedef float (*waypoint_fn_t)(pid_values_t *out, float *ramp_seconds);
typedef void (*waypoint_reset_fn_t)(void);

static const waypoint_fn_t s_waypoint_fns[SCENARIO_COUNT] = {
    [SCENARIO_IDLE]          = waypoint_idle,
    [SCENARIO_CITY_DRIVING]  = waypoint_city_driving,
    [SCENARIO_HIGHWAY]       = waypoint_highway,
    [SCENARIO_AGGRESSIVE]    = waypoint_aggressive,
    [SCENARIO_ENGINE_FAULT]  = waypoint_engine_fault,
};

static const waypoint_reset_fn_t s_waypoint_reset_fns[SCENARIO_COUNT] = {
    [SCENARIO_IDLE]          = waypoint_reset_idle,
    [SCENARIO_CITY_DRIVING]  = waypoint_reset_city,
    [SCENARIO_HIGHWAY]       = waypoint_reset_highway,
    [SCENARIO_AGGRESSIVE]    = waypoint_reset_aggressive,
    [SCENARIO_ENGINE_FAULT]  = waypoint_reset_engine_fault,
};

/* ---------------------------------------------------------------------------
 * Waypoint Task
 * ------------------------------------------------------------------------- */

static void scenario_waypoint_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "Waypoint task started (initial scenario: %s)",
             s_scenario_names[s_current_scenario]);

    for (;;) {
        scenario_type_t scenario = s_current_scenario;

        pid_values_t target;
        float ramp_seconds = 2.0f;
        float interval_sec = s_waypoint_fns[scenario](&target, &ramp_seconds);

        pid_generator_set_target(&target, ramp_seconds);

        ESP_LOGI(TAG, "[%s] waypoint: rpm=%u speed=%.1fkm/h coolant=%dC throttle=%.1f%% "
                      "(ramp=%.1fs, next in %.1fs)",
                 s_scenario_names[scenario], target.engine_rpm, target.vehicle_speed_kmh,
                 target.coolant_temp_c, target.throttle_position_pct, ramp_seconds, interval_sec);

        s_force_new_waypoint = false;

        int64_t wait_ms = (int64_t)(interval_sec * 1000.0f);
        int64_t waited_ms = 0;
        while (waited_ms < wait_ms && !s_force_new_waypoint) {
            vTaskDelay(pdMS_TO_TICKS(SCENARIO_WAIT_POLL_MS));
            waited_ms += SCENARIO_WAIT_POLL_MS;
        }
    }
}

/* ---------------------------------------------------------------------------
 * Serial Console Input
 * ------------------------------------------------------------------------- */

static void scenario_console_print_menu(void)
{
    printf("\n--- OBD-II Simulator: Driving Scenario ---\n");
    printf("  0 / idle       - %s\n", s_scenario_names[SCENARIO_IDLE]);
    printf("  1 / city       - %s\n", s_scenario_names[SCENARIO_CITY_DRIVING]);
    printf("  2 / highway    - %s\n", s_scenario_names[SCENARIO_HIGHWAY]);
    printf("  3 / aggressive - %s\n", s_scenario_names[SCENARIO_AGGRESSIVE]);
    printf("  4 / fault      - %s\n", s_scenario_names[SCENARIO_ENGINE_FAULT]);
    printf("Type a number or name and press Enter to switch. Current: %s\n> ",
           s_scenario_names[s_current_scenario]);
    fflush(stdout);
}

/**
 * @brief Parse one line of console input into a scenario_type_t.
 *        Accepts either the numeric index or a short case-insensitive
 *        name.
 */
static bool scenario_parse_input(const char *line, scenario_type_t *out)
{
    if (strlen(line) == 1 && isdigit((unsigned char)line[0])) {
        int idx = line[0] - '0';
        if (idx >= 0 && idx < SCENARIO_COUNT) {
            *out = (scenario_type_t)idx;
            return true;
        }
        return false;
    }

    if (strcasecmp(line, "idle") == 0) { *out = SCENARIO_IDLE; return true; }
    if (strcasecmp(line, "city") == 0) { *out = SCENARIO_CITY_DRIVING; return true; }
    if (strcasecmp(line, "highway") == 0) { *out = SCENARIO_HIGHWAY; return true; }
    if (strcasecmp(line, "aggressive") == 0) { *out = SCENARIO_AGGRESSIVE; return true; }
    if (strcasecmp(line, "fault") == 0) { *out = SCENARIO_ENGINE_FAULT; return true; }
    if (strcasecmp(line, "engine_fault") == 0) { *out = SCENARIO_ENGINE_FAULT; return true; }

    return false;
}

/**
 * @brief Reconfigure the console UART for blocking, line-buffered
 *        stdin/stdout so fgets() behaves the way a normal serial
 *        terminal expects. Standard ESP-IDF console boilerplate (the
 *        same sequence used by IDF's own console examples), not
 *        scenario-specific logic - included here rather than in
 *        app-level main.c since this module is the only consumer of
 *        blocking stdin in this project.
 */
static void scenario_console_uart_init(void)
{
    uart_vfs_dev_port_set_rx_line_endings(CONFIG_ESP_CONSOLE_UART_NUM, ESP_LINE_ENDINGS_CR);
    uart_vfs_dev_port_set_tx_line_endings(CONFIG_ESP_CONSOLE_UART_NUM, ESP_LINE_ENDINGS_CRLF);

    uart_config_t uart_config = {
        .baud_rate = CONFIG_ESP_CONSOLE_UART_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config(CONFIG_ESP_CONSOLE_UART_NUM, &uart_config);
    uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM, 256, 0, 0, NULL, 0);
    uart_vfs_dev_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);
}

static void scenario_console_task(void *arg)
{
    (void)arg;

    scenario_console_uart_init();
    scenario_console_print_menu();

    char line[32];
    for (;;) {
        if (fgets(line, sizeof(line), stdin) == NULL) {
            vTaskDelay(pdMS_TO_TICKS(200)); /* Avoid a busy loop if stdin has nothing yet */
            continue;
        }

        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) {
            continue; /* Blank line (bare Enter) - ignore */
        }

        scenario_type_t parsed;
        if (scenario_parse_input(line, &parsed)) {
            scenario_manager_set_scenario(parsed);
        } else {
            printf("Unrecognized scenario \"%s\".\n", line);
        }
        scenario_console_print_menu();
    }
}

/* ---------------------------------------------------------------------------
 * Push Button Input
 * ------------------------------------------------------------------------- */

static void scenario_button_task(void *arg)
{
    (void)arg;

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << SCENARIO_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed for button GPIO: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Button task started (GPIO%d, active-low, cycles scenarios forward)",
             SCENARIO_BUTTON_GPIO);

    bool was_pressed = false;
    int64_t last_press_us = 0;

    for (;;) {
        bool is_pressed = (gpio_get_level(SCENARIO_BUTTON_GPIO) == 0); /* active-low */
        int64_t now_us = esp_timer_get_time();

        if (is_pressed && !was_pressed &&
            (now_us - last_press_us) >= ((int64_t)SCENARIO_BUTTON_DEBOUNCE_MS * 1000)) {
            scenario_type_t next = (scenario_type_t)((s_current_scenario + 1) % SCENARIO_COUNT);
            ESP_LOGI(TAG, "Button pressed - cycling to next scenario");
            scenario_manager_set_scenario(next);
            last_press_us = now_us;
        }
        was_pressed = is_pressed;

        vTaskDelay(pdMS_TO_TICKS(SCENARIO_BUTTON_POLL_MS));
    }
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

esp_err_t scenario_manager_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_current_scenario = SCENARIO_IDLE;
    s_force_new_waypoint = true;

    BaseType_t waypoint_created = xTaskCreate(
        scenario_waypoint_task, "scenario_waypoint_task",
        SCENARIO_WAYPOINT_TASK_STACK, NULL, SCENARIO_TASK_PRIORITY, NULL);
    if (waypoint_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create waypoint task");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t console_created = xTaskCreate(
        scenario_console_task, "scenario_console_task",
        SCENARIO_CONSOLE_TASK_STACK, NULL, SCENARIO_TASK_PRIORITY, NULL);
    if (console_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create console task");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t button_created = xTaskCreate(
        scenario_button_task, "scenario_button_task",
        SCENARIO_BUTTON_TASK_STACK, NULL, SCENARIO_TASK_PRIORITY, NULL);
    if (button_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create button task");
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Scenario manager initialized (default scenario: Idle)");
    return ESP_OK;
}

esp_err_t scenario_manager_set_scenario(scenario_type_t scenario)
{
    if (scenario >= SCENARIO_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    if (scenario != s_current_scenario) {
        ESP_LOGI(TAG, "Scenario changed: %s -> %s",
                 s_scenario_names[s_current_scenario], s_scenario_names[scenario]);
    }

    /* Reset the new scenario's per-scenario state so re-entering it
     * later (or switching to it fresh) always starts from the same
     * defined baseline rather than wherever a previous activation left
     * off - important for SCENARIO_ENGINE_FAULT in particular, whose
     * coolant creep should restart from a normal temperature each time
     * the fault is (re-)selected. */
    s_waypoint_reset_fns[scenario]();

    s_current_scenario = scenario;
    s_force_new_waypoint = true;

    return ESP_OK;
}

scenario_type_t scenario_manager_get_scenario(void)
{
    return s_current_scenario;
}
