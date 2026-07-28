/* ============================================================================
 * sensor_manager.h
 *
 * Owns the three physical sensor drivers (mpu6050, gps, obd) and fuses
 * their readings into a single vehicle_sample_t snapshot, refreshed
 * continuously by internal background tasks. driver_score,
 * crash_detection, geofence, and idling_detection all read the current
 * snapshot independently via sensor_manager_get_latest() at whatever
 * cadence each needs - this module is a multi-reader data source, not a
 * queue, since a FreeRTOS queue would only deliver each sample to a
 * single consumer.
 *
 * vehicle_sample_t itself is defined in driver_score.h (see that file's
 * header comment for the rationale); this header includes it to expose
 * the type sensor_manager_get_latest() produces.
 * ========================================================================= */

#ifndef SENSOR_MANAGER_H
#define SENSOR_MANAGER_H

#include "esp_err.h"
#include "driver_score.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize all three underlying sensor drivers and start the
 *        internal fusion and OBD-polling background tasks.
 *
 * The MPU6050 is treated as a hard requirement: this system's core
 * safety function (crash detection, harsh-event detection) depends on
 * it, so a failure to initialize or identify the IMU is a fatal error
 * for this module. GPS and OBD-II are treated as soft requirements: if
 * either fails to initialize (e.g. no GPS antenna connected, no OBD-II
 * adapter plugged in), initialization continues and the corresponding
 * fields in vehicle_sample_t simply report as invalid until the
 * hardware becomes available.
 *
 * Gyroscope calibration (mpu6050_calibrate_gyro()) is performed once
 * here at startup; the vehicle is assumed stationary during this brief
 * window, consistent with a device powering on before a trip begins.
 *
 * @return
 *      - ESP_OK on success
 *      - Propagates the underlying error from mpu6050_init() if the IMU
 *        could not be initialized
 *      - ESP_ERR_NO_MEM if internal synchronization primitives or
 *        background tasks could not be created
 */
esp_err_t sensor_manager_init(void);

/**
 * @brief Get a thread-safe copy of the most recent fused sensor sample.
 *
 * Safe to call from any number of concurrent tasks. If the IMU has
 * never produced a successful reading, accel/gyro fields will be zero;
 * GPS/OBD fields always carry their own validity flags regardless.
 *
 * @param[out] out Pointer to a caller-allocated structure to populate.
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if out is NULL
 *      - ESP_ERR_INVALID_STATE if sensor_manager_init() has not been
 *        called
 */
esp_err_t sensor_manager_get_latest(vehicle_sample_t *out);

#ifdef __cplusplus
}
#endif

#endif /* SENSOR_MANAGER_H */
