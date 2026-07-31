/* ============================================================================
 * mpu6050.c
 *
 * Implementation of the MPU6050 I2C driver declared in mpu6050.h.
 * See mpu6050.h for the public API contract.
 * ========================================================================= */

#include <string.h>
#include "mpu6050.h"
#include "config.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "mpu6050";

/* ---------------------------------------------------------------------------
 * MPU6050 Register Map (internal - not exposed to consumers)
 * ------------------------------------------------------------------------- */
#define MPU6050_REG_SMPLRT_DIV         0x19
#define MPU6050_REG_CONFIG             0x1A
#define MPU6050_REG_GYRO_CONFIG        0x1B
#define MPU6050_REG_ACCEL_CONFIG       0x1C
#define MPU6050_REG_ACCEL_XOUT_H       0x3B
#define MPU6050_REG_TEMP_OUT_H         0x41
#define MPU6050_REG_GYRO_XOUT_H        0x43
#define MPU6050_REG_PWR_MGMT_1         0x6B
#define MPU6050_REG_WHO_AM_I           0x75

#define MPU6050_WHO_AM_I_VALUE         0x68
#define MPU6500_WHO_AM_I_VALUE         0x70

/* Configuration values selected to fix the full-scale ranges documented
 * in mpu6050.h: +/-8g accelerometer, +/-500 deg/s gyroscope. */
#define MPU6050_PWR1_WAKE_PLL_XGYRO    0x01    /* Wake, PLL clock w/ X gyro ref */
#define MPU6050_CONFIG_DLPF_44HZ       0x03    /* DLPF ~44 Hz bandwidth */
#define MPU6050_GYRO_CONFIG_FS500      0x08    /* FS_SEL=1 -> +/-500 deg/s */
#define MPU6050_ACCEL_CONFIG_FS8G      0x10    /* AFS_SEL=2 -> +/-8 g */
#define MPU6050_SMPLRT_DIV_VALUE       0x09    /* 1kHz / (1+9) = 100 Hz internal */

/* Scale factors corresponding to the fixed full-scale ranges above,
 * per the MPU6050 register map datasheet. */
#define MPU6050_ACCEL_LSB_PER_G        4096.0f
#define MPU6050_GYRO_LSB_PER_DPS       65.5f
#define MPU6050_TEMP_SENSITIVITY       340.0f
#define MPU6050_TEMP_OFFSET_C          36.53f

#define MPU6050_BURST_READ_LEN         14      /* accel(6) + temp(2) + gyro(6) */
#define MPU6050_CALIBRATION_DELAY_MS   5

/* ---------------------------------------------------------------------------
 * Module State
 * ------------------------------------------------------------------------- */
static SemaphoreHandle_t s_i2c_mutex = NULL;
static bool s_initialized = false;
static bool s_i2c_driver_owned = false; /* true if this module installed the driver */

static float s_gyro_offset_x_dps = 0.0f;
static float s_gyro_offset_y_dps = 0.0f;
static float s_gyro_offset_z_dps = 0.0f;

/* ---------------------------------------------------------------------------
 * Low-level I2C helpers
 * ------------------------------------------------------------------------- */

/**
 * @brief Write a single byte to a device register. Caller must hold
 *        s_i2c_mutex.
 */
static esp_err_t mpu6050_write_reg_locked(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = { reg, value };
    return i2c_master_write_to_device(I2C_MASTER_PORT, MPU6050_I2C_ADDRESS,
                                       buf, sizeof(buf),
                                       pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS));
}

/**
 * @brief Read a contiguous block of registers starting at reg. Caller must
 *        hold s_i2c_mutex.
 */
static esp_err_t mpu6050_read_regs_locked(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_write_read_device(I2C_MASTER_PORT, MPU6050_I2C_ADDRESS,
                                         &reg, 1, buf, len,
                                         pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS));
}

/**
 * @brief Perform one burst read of accel + temp + gyro raw registers and
 *        convert to signed 16-bit values. Thread-safe (acquires the bus
 *        mutex internally).
 */
static esp_err_t mpu6050_read_raw(int16_t accel_raw[3], int16_t gyro_raw[3],
                                   int16_t *temp_raw)
{
    if (s_i2c_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_i2c_mutex, pdMS_TO_TICKS(MUTEX_MAX_WAIT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Timed out acquiring I2C mutex for read");
        return ESP_ERR_TIMEOUT;
    }

    uint8_t raw[MPU6050_BURST_READ_LEN];
    esp_err_t err = mpu6050_read_regs_locked(MPU6050_REG_ACCEL_XOUT_H, raw,
                                              MPU6050_BURST_READ_LEN);

    xSemaphoreGive(s_i2c_mutex);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Burst read failed: %s", esp_err_to_name(err));
        return err;
    }

    accel_raw[0] = (int16_t)((raw[0] << 8) | raw[1]);
    accel_raw[1] = (int16_t)((raw[2] << 8) | raw[3]);
    accel_raw[2] = (int16_t)((raw[4] << 8) | raw[5]);
    *temp_raw    = (int16_t)((raw[6] << 8) | raw[7]);
    gyro_raw[0]  = (int16_t)((raw[8] << 8) | raw[9]);
    gyro_raw[1]  = (int16_t)((raw[10] << 8) | raw[11]);
    gyro_raw[2]  = (int16_t)((raw[12] << 8) | raw[13]);

    return ESP_OK;
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

esp_err_t mpu6050_init(void)
{
    esp_err_t err;

    if (s_i2c_mutex == NULL) {
        s_i2c_mutex = xSemaphoreCreateMutex();
        if (s_i2c_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create I2C mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    /* Configure and install the I2C driver. If another module already
     * installed the driver on this port, i2c_param_config/driver_install
     * will report ESP_ERR_INVALID_STATE; we treat that as non-fatal since
     * the bus is still usable. */
    i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_GPIO,
        .scl_io_num = I2C_MASTER_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };

    err = i2c_param_config(I2C_MASTER_PORT, &i2c_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_param_config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = i2c_driver_install(I2C_MASTER_PORT, i2c_conf.mode, 0, 0, 0);
    if (err == ESP_OK) {
        s_i2c_driver_owned = true;
    } else if (err == ESP_ERR_INVALID_STATE) {
        /* Driver already installed by another module on this port; fine. */
        ESP_LOGW(TAG, "I2C driver already installed on port %d", I2C_MASTER_PORT);
    } else {
        ESP_LOGE(TAG, "i2c_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }

    if (xSemaphoreTake(s_i2c_mutex, pdMS_TO_TICKS(MUTEX_MAX_WAIT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Timed out acquiring I2C mutex during init");
        return ESP_ERR_TIMEOUT;
    }

/* Verify device identity before touching configuration registers.
     *
     * MPU6500_WHO_AM_I_VALUE is accepted alongside the genuine MPU6050
     * ID because many boards labeled "MPU-6050" actually
     * carry an MPU6500 die - the two are pin and register-compatible
     * for the init/read sequence this driver uses, so treating 0x70 as
     * valid lets development continue without hardware changes. This is
     * a stopgap: MPU6500 has different noise/bias characteristics than
     * a genuine MPU6050, and this system's crash thresholds
     * (THRESHOLD_CRASH_ACCEL_G/DPS in config.h) were chosen with an
     * MPU6050 in mind. Replace with a verified MPU6050 before relying
     * on this for real safety-critical deployment. 
*/
    uint8_t who_am_i = 0;
    err = mpu6050_read_regs_locked(MPU6050_REG_WHO_AM_I, &who_am_i, 1);
    if (err != ESP_OK) {
        xSemaphoreGive(s_i2c_mutex);
        ESP_LOGE(TAG, "WHO_AM_I read failed: %s", esp_err_to_name(err));
        return err;
    }
    if (who_am_i != MPU6050_WHO_AM_I_VALUE && who_am_i != MPU6500_WHO_AM_I_VALUE) {
        xSemaphoreGive(s_i2c_mutex);
        ESP_LOGE(TAG, "Unexpected WHO_AM_I: 0x%02X (expected 0x%02X or 0x%02X)",
                 who_am_i, MPU6050_WHO_AM_I_VALUE, MPU6500_WHO_AM_I_VALUE);
        return ESP_ERR_NOT_FOUND;
    }
    if (who_am_i == MPU6500_WHO_AM_I_VALUE) {
        ESP_LOGW(TAG, "Detected MPU6500 die (WHO_AM_I=0x70) on a board expected to be "
                 "MPU6050 - proceeding with MPU6050 register sequence, but replace with "
                 "a genuine MPU6050 before relying on this for safety-critical operation");
    }

    /* Wake the device from sleep and select the gyro X PLL as clock source
     * for improved timing stability, per the datasheet recommendation. */
    err = mpu6050_write_reg_locked(MPU6050_REG_PWR_MGMT_1, MPU6050_PWR1_WAKE_PLL_XGYRO);
    if (err != ESP_OK) {
        xSemaphoreGive(s_i2c_mutex);
        ESP_LOGE(TAG, "PWR_MGMT_1 write failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Datasheet recommends a brief settling delay after clock source
     * selection before further configuration. */
    xSemaphoreGive(s_i2c_mutex);
    vTaskDelay(pdMS_TO_TICKS(10));
    if (xSemaphoreTake(s_i2c_mutex, pdMS_TO_TICKS(MUTEX_MAX_WAIT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Timed out re-acquiring I2C mutex during init");
        return ESP_ERR_TIMEOUT;
    }

    err = mpu6050_write_reg_locked(MPU6050_REG_SMPLRT_DIV, MPU6050_SMPLRT_DIV_VALUE);
    if (err != ESP_OK) {
        xSemaphoreGive(s_i2c_mutex);
        ESP_LOGE(TAG, "SMPLRT_DIV write failed: %s", esp_err_to_name(err));
        return err;
    }

    err = mpu6050_write_reg_locked(MPU6050_REG_CONFIG, MPU6050_CONFIG_DLPF_44HZ);
    if (err != ESP_OK) {
        xSemaphoreGive(s_i2c_mutex);
        ESP_LOGE(TAG, "CONFIG write failed: %s", esp_err_to_name(err));
        return err;
    }

    err = mpu6050_write_reg_locked(MPU6050_REG_GYRO_CONFIG, MPU6050_GYRO_CONFIG_FS500);
    if (err != ESP_OK) {
        xSemaphoreGive(s_i2c_mutex);
        ESP_LOGE(TAG, "GYRO_CONFIG write failed: %s", esp_err_to_name(err));
        return err;
    }

    err = mpu6050_write_reg_locked(MPU6050_REG_ACCEL_CONFIG, MPU6050_ACCEL_CONFIG_FS8G);
    if (err != ESP_OK) {
        xSemaphoreGive(s_i2c_mutex);
        ESP_LOGE(TAG, "ACCEL_CONFIG write failed: %s", esp_err_to_name(err));
        return err;
    }

    xSemaphoreGive(s_i2c_mutex);

    s_initialized = true;
    ESP_LOGI(TAG, "MPU6050 initialized (accel +/-8g, gyro +/-500dps)");
    return ESP_OK;
}

esp_err_t mpu6050_read(mpu6050_data_t *data)
{
    if (data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        ESP_LOGE(TAG, "mpu6050_read called before successful mpu6050_init");
        return ESP_ERR_INVALID_STATE;
    }

    int16_t accel_raw[3];
    int16_t gyro_raw[3];
    int16_t temp_raw;

    esp_err_t err = mpu6050_read_raw(accel_raw, gyro_raw, &temp_raw);
    if (err != ESP_OK) {
        return err;
    }

    data->accel_x_g = (float)accel_raw[0] / MPU6050_ACCEL_LSB_PER_G;
    data->accel_y_g = (float)accel_raw[1] / MPU6050_ACCEL_LSB_PER_G;
    data->accel_z_g = (float)accel_raw[2] / MPU6050_ACCEL_LSB_PER_G;

    data->gyro_x_dps = ((float)gyro_raw[0] / MPU6050_GYRO_LSB_PER_DPS) - s_gyro_offset_x_dps;
    data->gyro_y_dps = ((float)gyro_raw[1] / MPU6050_GYRO_LSB_PER_DPS) - s_gyro_offset_y_dps;
    data->gyro_z_dps = ((float)gyro_raw[2] / MPU6050_GYRO_LSB_PER_DPS) - s_gyro_offset_z_dps;

    data->temp_c = ((float)temp_raw / MPU6050_TEMP_SENSITIVITY) + MPU6050_TEMP_OFFSET_C;
    data->timestamp_us = esp_timer_get_time();

    return ESP_OK;
}

esp_err_t mpu6050_calibrate_gyro(uint16_t sample_count)
{
    if (sample_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        ESP_LOGE(TAG, "mpu6050_calibrate_gyro called before successful mpu6050_init");
        return ESP_ERR_INVALID_STATE;
    }

    double sum_x = 0.0, sum_y = 0.0, sum_z = 0.0;
    uint16_t collected = 0;

    /* Reset any previous offsets so raw (uncorrected) values are averaged. */
    s_gyro_offset_x_dps = 0.0f;
    s_gyro_offset_y_dps = 0.0f;
    s_gyro_offset_z_dps = 0.0f;

    for (uint16_t i = 0; i < sample_count; i++) {
        int16_t accel_raw[3];
        int16_t gyro_raw[3];
        int16_t temp_raw;

        esp_err_t err = mpu6050_read_raw(accel_raw, gyro_raw, &temp_raw);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Calibration sample %u failed: %s", i, esp_err_to_name(err));
            return err;
        }

        sum_x += (double)gyro_raw[0] / MPU6050_GYRO_LSB_PER_DPS;
        sum_y += (double)gyro_raw[1] / MPU6050_GYRO_LSB_PER_DPS;
        sum_z += (double)gyro_raw[2] / MPU6050_GYRO_LSB_PER_DPS;
        collected++;

        vTaskDelay(pdMS_TO_TICKS(MPU6050_CALIBRATION_DELAY_MS));
    }

    s_gyro_offset_x_dps = (float)(sum_x / collected);
    s_gyro_offset_y_dps = (float)(sum_y / collected);
    s_gyro_offset_z_dps = (float)(sum_z / collected);

    ESP_LOGI(TAG, "Gyro calibration complete: offsets (dps) x=%.3f y=%.3f z=%.3f",
             s_gyro_offset_x_dps, s_gyro_offset_y_dps, s_gyro_offset_z_dps);

    return ESP_OK;
}
