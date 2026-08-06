/* ============================================================================
 * sensor_manager.c
 *
 * Implementation of the sensor fusion module declared in sensor_manager.h.
 *
 * Two internal background tasks run at different cadences because the
 * underlying drivers have very different timing characteristics:
 *
 *   - sensor_manager_fusion_task: runs at MPU6050_SAMPLE_PERIOD_MS
 *     (100ms/10Hz). Reads the IMU directly (fast, non-blocking I2C
 *     transaction), reads the latest cached GPS fix (gps.c already
 *     maintains this asynchronously via its own task), and reads the
 *     latest cached OBD snapshot (populated by the second task below),
 *     then publishes the merged result.
 *
 *   - sensor_manager_obd_task: runs at OBD_SAMPLE_PERIOD_MS (500ms).
 *     obd_read() is a blocking, sequential request/response protocol
 *     that can take up to ~400ms in the worst case (4 PIDs x
 *     OBD_REQUEST_TIMEOUT_MS). Running it inside the 100ms fusion loop
 *     would stall IMU sampling; a dedicated task avoids that entirely.
 *
 * A transient IMU read failure does not zero out the merged sample's
 * accel/gyro fields (which could look like "vehicle at rest" and mask a
 * real event) - the last successfully read IMU values are held instead,
 * with a log warning. GPS and OBD-II data, by contrast, already carry
 * their own validity flags from their respective drivers, so a failure
 * there is reported honestly rather than papered over.
 * ========================================================================= */

#include <string.h>
#include "sensor_manager.h"
#include "config.h"
#include "mpu6050.h"
#include "gps.h"
#include "obd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "sensor_manager";

#define GYRO_CALIBRATION_SAMPLE_COUNT   200   /* ~1s at 5ms/sample */

static SemaphoreHandle_t s_sample_mutex = NULL;
static vehicle_sample_t s_latest_sample = {0};

static SemaphoreHandle_t s_obd_cache_mutex = NULL;
static obd_data_t s_obd_cache = {0};

static bool s_initialized = false;

/* ---------------------------------------------------------------------------
 * OBD Polling Task
 * ------------------------------------------------------------------------- */

static void sensor_manager_obd_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "OBD polling task started");

    for (;;) {
        obd_data_t reading;
        esp_err_t err = obd_read(&reading);

        if (xSemaphoreTake(s_obd_cache_mutex, pdMS_TO_TICKS(MUTEX_MAX_WAIT_MS)) == pdTRUE) {
            if (err == ESP_OK) {
                memcpy(&s_obd_cache, &reading, sizeof(obd_data_t));
            } else {
                /* All four PIDs timed out this cycle - typically ignition
                 * off or adapter unplugged. Clear the cache entirely
                 * (all validity flags false) rather than leaving stale
                 * "valid" data (e.g. a frozen last-known RPM) that could
                 * mislead idling_detection into thinking the engine is
                 * still running long after the vehicle was shut off. */
                memset(&s_obd_cache, 0, sizeof(obd_data_t));
            }
            xSemaphoreGive(s_obd_cache_mutex);
        } else {
            ESP_LOGE(TAG, "Timed out acquiring OBD cache mutex");
        }

        vTaskDelay(pdMS_TO_TICKS(OBD_SAMPLE_PERIOD_MS));
    }
}

/* ---------------------------------------------------------------------------
 * Fusion Task
 * ------------------------------------------------------------------------- */

static void sensor_manager_fusion_task(void *arg)
{
    (void)arg;

    mpu6050_data_t last_good_imu = {0};
    bool have_good_imu = false;

    ESP_LOGI(TAG, "Sensor fusion task started");

    for (;;) {
        mpu6050_data_t imu;
        esp_err_t imu_err = mpu6050_read(&imu);
        if (imu_err == ESP_OK) {
            last_good_imu = imu;
            have_good_imu = true;
        } else {
            ESP_LOGW(TAG, "MPU6050 read failed (%s), holding last known IMU values",
                     esp_err_to_name(imu_err));
        }

        gps_data_t gps;
        if (gps_get_latest(&gps) != ESP_OK) {
            memset(&gps, 0, sizeof(gps)); /* fix_valid=false, safe default */
        }

        obd_data_t obd_snapshot;
        if (xSemaphoreTake(s_obd_cache_mutex, pdMS_TO_TICKS(MUTEX_MAX_WAIT_MS)) == pdTRUE) {
            memcpy(&obd_snapshot, &s_obd_cache, sizeof(obd_data_t));
            xSemaphoreGive(s_obd_cache_mutex);
        } else {
            ESP_LOGE(TAG, "Timed out acquiring OBD cache mutex in fusion task");
            memset(&obd_snapshot, 0, sizeof(obd_snapshot));
        }

        vehicle_sample_t merged = {0};

        if (have_good_imu) {
            merged.accel_x_g = last_good_imu.accel_x_g;
            merged.accel_y_g = last_good_imu.accel_y_g;
            merged.accel_z_g = last_good_imu.accel_z_g;
            merged.gyro_x_dps = last_good_imu.gyro_x_dps;
            merged.gyro_y_dps = last_good_imu.gyro_y_dps;
            merged.gyro_z_dps = last_good_imu.gyro_z_dps;
        }

        merged.latitude_deg = gps.latitude_deg;
        merged.longitude_deg = gps.longitude_deg;
        merged.gps_speed_kmh = gps.speed_kmh;
        merged.heading_deg = gps.heading_deg;
        merged.gps_fix_valid = gps.fix_valid;

        merged.engine_rpm = obd_snapshot.engine_rpm;
        merged.engine_rpm_valid = obd_snapshot.engine_rpm_valid;
        merged.obd_speed_kmh = obd_snapshot.vehicle_speed_kmh;
        merged.obd_speed_valid = obd_snapshot.vehicle_speed_valid;
        merged.throttle_position_pct = obd_snapshot.throttle_position_pct;
        merged.throttle_position_valid = obd_snapshot.throttle_position_valid;
        merged.coolant_temp_c = obd_snapshot.coolant_temp_c;
        merged.coolant_temp_valid = obd_snapshot.coolant_temp_valid;

        merged.timestamp_us = esp_timer_get_time();

        if (xSemaphoreTake(s_sample_mutex, pdMS_TO_TICKS(MUTEX_MAX_WAIT_MS)) == pdTRUE) {
            memcpy(&s_latest_sample, &merged, sizeof(vehicle_sample_t));
            xSemaphoreGive(s_sample_mutex);
        } else {
            ESP_LOGE(TAG, "Timed out acquiring sample mutex in fusion task");
        }

        vTaskDelay(pdMS_TO_TICKS(MPU6050_SAMPLE_PERIOD_MS));
    }
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

esp_err_t sensor_manager_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t err = mpu6050_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MPU6050 init failed (fatal - required for crash detection): %s",
                 esp_err_to_name(err));
        return err;
    }

    err = mpu6050_calibrate_gyro(GYRO_CALIBRATION_SAMPLE_COUNT);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Gyro calibration failed, proceeding with zero offsets: %s",
                 esp_err_to_name(err));
    }

    err = gps_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GPS init failed, continuing without GPS: %s", esp_err_to_name(err));
    }

    err = obd_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "OBD init failed, continuing without OBD-II: %s", esp_err_to_name(err));
    }

    s_sample_mutex = xSemaphoreCreateMutex();
    if (s_sample_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create sample mutex");
        return ESP_ERR_NO_MEM;
    }

    s_obd_cache_mutex = xSemaphoreCreateMutex();
    if (s_obd_cache_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create OBD cache mutex");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t fusion_task_created = xTaskCreatePinnedToCore(
        sensor_manager_fusion_task, "sensor_fusion_task", TASK_STACK_SIZE_MPU6050, NULL,
        TASK_PRIORITY_MPU6050, NULL, TASK_CORE_SENSOR_ACQUISITION);
    if (fusion_task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create sensor fusion task");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t obd_task_created = xTaskCreatePinnedToCore(
        sensor_manager_obd_task, "sensor_obd_task", TASK_STACK_SIZE_OBD, NULL,
        TASK_PRIORITY_OBD, NULL, TASK_CORE_SENSOR_ACQUISITION);
    if (obd_task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create OBD polling task");
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Sensor manager initialized");
    return ESP_OK;
}

esp_err_t sensor_manager_get_latest(vehicle_sample_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_sample_mutex, pdMS_TO_TICKS(MUTEX_MAX_WAIT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Timed out acquiring sample mutex in sensor_manager_get_latest");
        return ESP_ERR_TIMEOUT;
    }

    memcpy(out, &s_latest_sample, sizeof(vehicle_sample_t));
    xSemaphoreGive(s_sample_mutex);

    return ESP_OK;
}
