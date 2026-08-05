/* ============================================================================
 * pid_generator.h
 *
 * Owns the live RPM / vehicle speed / coolant temperature / throttle
 * position model and the FreeRTOS task that advances it smoothly over
 * time. Internally this is a per-field "ramp": each call to
 * pid_generator_set_target() defines a new target value and a duration
 * over which to smoothly transition there (eased, not linear-jerky),
 * starting from wherever the field actually is *right now* - not from
 * the previous target - so retargeting mid-transition never produces a
 * discontinuous jump. A small bounded noise component is layered on top
 * of the eased value on every tick for realism (real sensor readings
 * are never perfectly smooth even at a fixed target).
 *
 * Ownership split: pid_generator is pure "given a target and elapsed
 * time, what should the instrument cluster read right now" logic. It
 * has no knowledge of BLE, ELM327 command syntax, or of *why* a target
 * was chosen (that's scenario_manager's job - deciding what a "City
 * Driving" vs "Aggressive Driving" waypoint looks like and how
 * aggressively to ramp toward it). This keeps telemetry generation
 * testable in isolation, per the project's modularity requirement.
 * ========================================================================= */

#ifndef PID_GENERATOR_H
#define PID_GENERATOR_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief One instant's worth of simulated OBD-II Mode 01 readings, in
 *        the same engineering units DBAS's obd.h uses (so no unit
 *        conversion is needed anywhere in elm327_sim's response
 *        encoding).
 */
typedef struct {
    uint16_t engine_rpm;             /*!< 0-16383 (Mode 01 PID 0x0C range) */
    float    vehicle_speed_kmh;      /*!< 0-255 */
    int16_t  coolant_temp_c;         /*!< -40 to 215 (Mode 01 PID 0x05 range) */
    float    throttle_position_pct;  /*!< 0-100 */
} pid_values_t;

/**
 * @brief Initialize the PID generator's internal state to a safe
 *        default (engine off / idle-like values).
 *
 * @return ESP_OK on success, ESP_ERR_NO_MEM if internal synchronization
 *         primitives could not be created.
 */
esp_err_t pid_generator_init(void);

/**
 * @brief Get a thread-safe snapshot of the current simulated values
 *        (the eased, noise-layered instantaneous reading - not the
 *        target).
 *
 * Safe to call from any task (elm327_sim's command-processing task
 * calls this on every PID request).
 *
 * @param[out] out Pointer to a caller-allocated structure to populate.
 */
void pid_generator_get_current(pid_values_t *out);

/**
 * @brief Set a new target for all four fields simultaneously, to be
 *        reached via a smooth, eased transition over ramp_seconds.
 *
 * Each field ramps independently starting from its own current value
 * at the moment of this call (not from the previous target), so
 * calling this again before a prior transition finishes re-targets
 * smoothly from wherever the field actually is rather than jumping.
 *
 * scenario_manager is expected to be the sole caller: it decides what
 * the target should be (a "waypoint" appropriate to the active driving
 * scenario) and how aggressively to reach it (a short ramp_seconds for
 * harsh/aggressive events, a longer one for gentle drift).
 *
 * @param[in] target       Desired end values for all four fields.
 * @param[in] ramp_seconds Time over which to smoothly transition every
 *                          field to its target. Values below one tick
 *                          period are floored (see PID_TICK_PERIOD_MS in
 *                          pid_generator.c) to avoid an effectively
 *                          instantaneous, unrealistic jump.
 */
void pid_generator_set_target(const pid_values_t *target, float ramp_seconds);

#ifdef __cplusplus
}
#endif

#endif /* PID_GENERATOR_H */
