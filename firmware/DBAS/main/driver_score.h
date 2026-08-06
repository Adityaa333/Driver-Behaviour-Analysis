/* ============================================================================
 * driver_score.h
 *
 * Computes a cumulative-trip Driver Safety Score (0-100, starting at 100
 * and decremented by weighted penalties) from two sources:
 *
 *   1. Raw vehicle_sample_t snapshots (pulled periodically from
 *      sensor_manager) - used internally to detect harsh braking, harsh
 *      acceleration, harsh cornering, and overspeeding via edge-triggered
 *      threshold comparisons against config.h.
 *   2. Discrete driver_event_t reports submitted by other detector
 *      modules (crash_detection, idling_detection, geofence) via
 *      driver_score_submit_event().
 *
 * vehicle_sample_t is defined here (rather than in sensor_manager.h)
 * because this module defines the contract of what a "scorable sample"
 * looks like; sensor_manager.h includes this header to produce values
 * matching it.
 * ========================================================================= */

#ifndef DRIVER_SCORE_H
#define DRIVER_SCORE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Merged IMU + GPS + OBD-II snapshot representing the vehicle's
 *        state at one point in time. Produced by sensor_manager,
 *        consumed by driver_score (and available to crash_detection,
 *        idling_detection, and geofence, which read the same type from
 *        sensor_manager independently).
 *
 * Per-source validity flags mirror the underlying drivers (gps_data_t,
 * obd_data_t) since any individual source can be temporarily unavailable
 * (no GPS fix, OBD request timeout) without invalidating the whole
 * sample.
 */
typedef struct {
    /* IMU (MPU6050), assumed vehicle-frame mounted: X = longitudinal
     * (+X forward), Y = lateral (+Y right), Z = vertical. */
    float accel_x_g;
    float accel_y_g;
    float accel_z_g;
    float gyro_x_dps;
    float gyro_y_dps;
    float gyro_z_dps;

    /* GPS */
    double latitude_deg;
    double longitude_deg;
    float gps_speed_kmh;
    float heading_deg;
    bool gps_fix_valid;

    /* OBD-II */
    uint16_t engine_rpm;
    bool engine_rpm_valid;
    float obd_speed_kmh;
    bool obd_speed_valid;
    float throttle_position_pct;
    bool throttle_position_valid;
    int16_t coolant_temp_c;         /*!< Engine coolant temperature, Celsius */
    bool coolant_temp_valid;

    int64_t timestamp_us;
} vehicle_sample_t;

/**
 * @brief Categories of scoring-relevant driving events. HARSH_BRAKING
 *        through OVERSPEED are detected internally by driver_score from
 *        vehicle_sample_t data; IDLING, CRASH, and GEOFENCE_VIOLATION are
 *        reported externally by their respective dedicated modules.
 */
typedef enum {
    DRIVER_EVENT_HARSH_BRAKING = 0,
    DRIVER_EVENT_HARSH_ACCELERATION,
    DRIVER_EVENT_HARSH_CORNERING,
    DRIVER_EVENT_OVERSPEED,
    DRIVER_EVENT_IDLING,
    DRIVER_EVENT_CRASH,
    DRIVER_EVENT_GEOFENCE_VIOLATION,
} driver_event_type_t;

/**
 * @brief A single scoring-relevant event report.
 */
typedef struct {
    driver_event_type_t type;
    float magnitude;       /*!< Event-specific: peak g for harsh events,
                                 km/h over limit for overspeed, duration
                                 in seconds for idling. Unused (0) for
                                 crash and geofence events. */
    int64_t timestamp_us;
} driver_event_t;

/**
 * @brief Create internal state (mutex, event queue) and start the
 *        background scoring task. The task pulls samples from
 *        sensor_manager, detects harsh-driving events internally, drains
 *        externally-submitted events, and periodically recomputes and
 *        publishes the score via mqtt_client_publish_score().
 *
 * Must be called after sensor_manager_init() and mqtt_client_init().
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_NO_MEM if internal synchronization primitives or the
 *        background task could not be created
 */
esp_err_t driver_score_init(void);

/**
 * @brief Report a discrete driving event for inclusion in the score.
 *        Called by crash_detection, idling_detection, and geofence.
 *        Thread-safe; non-blocking beyond a short bounded wait.
 *
 * @param[in] event Event to report. Copied internally; caller retains
 *                   ownership of the pointer after this call returns.
 * @return
 *      - ESP_OK if the event was enqueued
 *      - ESP_ERR_INVALID_ARG if event is NULL
 *      - ESP_ERR_NO_MEM if the internal event queue is full (event
 *        dropped; logged internally)
 *      - ESP_ERR_INVALID_STATE if driver_score_init() has not been
 *        called
 */
esp_err_t driver_score_submit_event(const driver_event_t *event);

/**
 * @brief Get the current cumulative-trip safety score.
 *
 * @param[out] score_out Pointer to receive the score, range [0.0, 100.0].
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if score_out is NULL
 *      - ESP_ERR_INVALID_STATE if driver_score_init() has not been
 *        called
 */
esp_err_t driver_score_get_current(float *score_out);

#ifdef __cplusplus
}
#endif

#endif /* DRIVER_SCORE_H */
