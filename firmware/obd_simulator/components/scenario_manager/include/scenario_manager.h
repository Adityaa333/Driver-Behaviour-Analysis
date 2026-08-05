/* ============================================================================
 * scenario_manager.h
 *
 * *** MILESTONE 1 PLACEHOLDER ***
 * Defines the permanent scenario_type_t enum and switching API. The
 * next step implements: the background tick task that pushes new
 * targets into pid_generator every cycle, smooth (ramped, not
 * instantaneous) transitions when the scenario changes, serial-console
 * input (type a number + Enter), and push-button GPIO input (one button
 * cycling forward through scenarios, debounced). This step only wires
 * up scenario_manager_init()/set/get so elm327_sim and main.c have a
 * stable API to build against.
 * ========================================================================= */

#ifndef SCENARIO_MANAGER_H
#define SCENARIO_MANAGER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Driving scenarios the simulator can emulate. Values are stable
 *        ABI (used as the serial-console selection index), so append
 *        new scenarios at the end rather than reordering.
 */
typedef enum {
    SCENARIO_IDLE = 0,          /*!< Engine idling, stationary */
    SCENARIO_CITY_DRIVING,      /*!< Frequent accel/brake, low-moderate speed */
    SCENARIO_HIGHWAY,           /*!< Sustained high speed, low variance */
    SCENARIO_AGGRESSIVE,        /*!< Hard accel/brake, high RPM, speeding */
    SCENARIO_ENGINE_FAULT,      /*!< Erratic RPM/coolant, simulates a fault */
    SCENARIO_COUNT              /*!< Sentinel - not a selectable scenario */
} scenario_type_t;

/**
 * @brief Initialize scenario state (defaulting to SCENARIO_IDLE) and
 *        start the background tasks that will later own smoothing and
 *        input handling.
 *
 * Must be called after pid_generator_init().
 *
 * @return ESP_OK on success, ESP_ERR_NO_MEM on task/primitive creation
 *         failure.
 */
esp_err_t scenario_manager_init(void);

/**
 * @brief Switch to a new driving scenario.
 *
 * @param[in] scenario One of the scenario_type_t values (excluding
 *                      SCENARIO_COUNT).
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if scenario is out of
 *         range.
 */
esp_err_t scenario_manager_set_scenario(scenario_type_t scenario);

/**
 * @brief Get the currently active scenario.
 */
scenario_type_t scenario_manager_get_scenario(void);

#ifdef __cplusplus
}
#endif

#endif /* SCENARIO_MANAGER_H */
