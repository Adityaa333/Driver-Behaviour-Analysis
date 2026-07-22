/*
Design Notes : 
the accelerometer/gyro full-scale ranges (±8g / ±500 dps) rather than making them configurable, 
since they must stay consistent with the crash thresholds in config.h 
— exposing them as options would risk someone changing one without the other.
mpu6050_read() returns pre-scaled, engineering-unit values (g, deg/s) so no downstream module
touches raw registers or LSB/scale-factor math.
Gyro calibration is a separate explicit call (meant to run once at boot while stationary) 
rather than happening automatically inside init(), since it takes measurable time and the caller should control when it happens
*/

/* ============================================================================
 * mpu6050.h
 *
 * Driver for the InvenSense MPU6050 6-axis IMU (3-axis accelerometer +
 * 3-axis gyroscope) over I2C.
 *
 * This driver exposes calibrated, scaled readings (g and deg/s) rather than
 * raw register values, so consumers (sensor_manager, crash_detection, etc.)
 * never need to know about the underlying register map or full-scale
 * range configuration.
 *
 * Full-scale ranges are fixed at +/-8g (accelerometer) and +/-500 deg/s
 * (gyroscope). These ranges are chosen deliberately wider than the
 * THRESHOLD_CRASH_ACCEL_G (3.5g) and THRESHOLD_CRASH_GYRO_DPS (300 dps)
 * values in config.h, so that genuine crash-level events are captured
 * without clipping.
 * ========================================================================= */

#ifndef MPU6050_H
#define MPU6050_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief A single, fully-scaled IMU sample.
 *
 * Acceleration is expressed in g (9.81 m/s^2), angular rate in degrees
 * per second, and temperature in degrees Celsius (onboard die temperature,
 * useful for diagnostics rather than precision measurement).
 */
typedef struct {
    float accel_x_g;       /*!< Acceleration on X axis, in g */
    float accel_y_g;       /*!< Acceleration on Y axis, in g */
    float accel_z_g;       /*!< Acceleration on Z axis, in g */
    float gyro_x_dps;      /*!< Angular rate about X axis, in deg/s */
    float gyro_y_dps;      /*!< Angular rate about Y axis, in deg/s */
    float gyro_z_dps;      /*!< Angular rate about Z axis, in deg/s */
    float temp_c;          /*!< Onboard die temperature, in Celsius */
    int64_t timestamp_us;  /*!< Timestamp of sample acquisition (esp_timer) */
} mpu6050_data_t;

/**
 * @brief Initialize the I2C bus (if not already installed) and configure
 *        the MPU6050: wake from sleep, set sample rate divider, and select
 *        the fixed accelerometer/gyroscope full-scale ranges.
 *
 * Verifies device identity via the WHO_AM_I register before proceeding.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_NOT_FOUND if the WHO_AM_I register does not match the
 *        expected MPU6050 identity
 *      - ESP_ERR_INVALID_STATE if the I2C driver could not be installed
 *      - ESP_FAIL or an I2C driver error code on communication failure
 */
esp_err_t mpu6050_init(void);

/**
 * @brief Read one fully-scaled sample from the sensor.
 *
 * Applies the gyroscope zero-rate offsets computed by
 * mpu6050_calibrate_gyro() (if calibration has been performed); otherwise
 * raw gyroscope conversion is returned uncorrected.
 *
 * @param[out] data Pointer to a caller-allocated structure to populate.
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if data is NULL
 *      - ESP_FAIL or an I2C driver error code on communication failure
 */
esp_err_t mpu6050_read(mpu6050_data_t *data);

/**
 * @brief Estimate and store gyroscope zero-rate offsets by averaging
 *        a number of samples while the device is assumed stationary.
 *
 * This should be called once at startup, before the vehicle begins
 * moving (e.g. during the boot/self-test phase in app_main). Offsets are
 * held in driver-internal state and applied automatically by
 * subsequent mpu6050_read() calls.
 *
 * @param[in] sample_count Number of samples to average (must be > 0).
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if sample_count is 0
 *      - ESP_FAIL or an I2C driver error code on communication failure
 */
esp_err_t mpu6050_calibrate_gyro(uint16_t sample_count);

#ifdef __cplusplus
}
#endif

#endif /* MPU6050_H */
