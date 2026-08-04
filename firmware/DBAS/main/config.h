/* ============================================================================
 * config.h
 *
 * Driver Behaviour Analysis System (DBAS) - Global Firmware Configuration
 *
 * Central location for all compile-time constants used across the firmware:
 * pin assignments, task scheduling parameters, communication settings, and
 * safety thresholds. Every other firmware module includes this header so
 * that hardware and tuning parameters are changed in exactly one place.
 *
 * Target: ESP32 (dual-core), ESP-IDF v5.x, FreeRTOS
 * ========================================================================= */

#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include "driver/i2c.h"
#include "driver/uart.h"
#include "host/ble_hs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Firmware Identification
 * ------------------------------------------------------------------------- */
#define FIRMWARE_VERSION_MAJOR         3
#define FIRMWARE_VERSION_MINOR         0
#define FIRMWARE_VERSION_PATCH         1
#define DEVICE_TYPE_STRING             "DBAS-ESP32"
#define DEVICE_ID_MAX_LEN              32   /* Derived from MAC at runtime */

/* ---------------------------------------------------------------------------
 * Wi-Fi Configuration
 *
 * NOTE: For a real commercial fleet deployment these credentials should be
 * provisioned per-device via NVS (e.g. through a provisioning app or
 * Bluetooth SmartConfig) rather than compiled into firmware. They are
 * defined here as macros to keep this reference implementation
 * self-contained and fully compilable out of the box.
 * ------------------------------------------------------------------------- */
#define WIFI_SSID                      "Amber"
#define WIFI_PASSWORD                  "123456789@"
#define WIFI_MAX_RETRY_COUNT           10
#define WIFI_RECONNECT_DELAY_MS        5000
#define WIFI_CONNECT_TIMEOUT_MS        15000

/* ---------------------------------------------------------------------------
 * MQTT Configuration
 * ------------------------------------------------------------------------- */
#define MQTT_BROKER_URI                "mqtt://192.168.1.20:1883"
#define MQTT_CLIENT_ID_PREFIX          "dbas_"
#define MQTT_KEEPALIVE_SEC             60
#define MQTT_QOS_TELEMETRY             1
#define MQTT_QOS_ALERT                 2
#define MQTT_QOS_CRASH                 2
#define MQTT_QOS_STATUS                1
#define MQTT_RECONNECT_TIMEOUT_MS      10000
#define MQTT_MAX_TOPIC_LEN             64
#define MQTT_MAX_PAYLOAD_LEN           512

/* MQTT topic format strings; each is formatted at runtime with the
 * device's unique ID (e.g. "fleet/AABBCCDDEEFF/telemetry"). */
#define MQTT_TOPIC_TELEMETRY_FMT       "fleet/%s/telemetry"
#define MQTT_TOPIC_SCORE_FMT           "fleet/%s/score"
#define MQTT_TOPIC_ALERT_FMT           "fleet/%s/alert"
#define MQTT_TOPIC_CRASH_FMT           "fleet/%s/crash"
#define MQTT_TOPIC_STATUS_FMT          "fleet/%s/status"

/* ---------------------------------------------------------------------------
 * I2C / MPU6050 Configuration
 * ------------------------------------------------------------------------- */
#define I2C_MASTER_PORT                I2C_NUM_0
#define I2C_MASTER_SDA_GPIO             21
#define I2C_MASTER_SCL_GPIO             22
#define I2C_MASTER_FREQ_HZ              400000
#define I2C_MASTER_TIMEOUT_MS           1000
#define MPU6050_I2C_ADDRESS             0x68
#define MPU6050_SAMPLE_PERIOD_MS        100     /* 10 Hz */

/* ---------------------------------------------------------------------------
 * UART / GPS Configuration
 * ------------------------------------------------------------------------- */
#define GPS_UART_PORT                   UART_NUM_1
#define GPS_UART_TX_GPIO                17
#define GPS_UART_RX_GPIO                16
#define GPS_UART_BAUD_RATE              9600
#define GPS_UART_RX_BUF_SIZE            2048
#define GPS_SAMPLE_PERIOD_MS            1000    /* 1 Hz, typical NMEA rate */
#define GPS_NMEA_MAX_SENTENCE_LEN       128
#define GPS_READY_MIN_SATELLITES        5
#define GPS_READY_MAX_HDOP              3.0f

/* ---------------------------------------------------------------------------
 * GPS Status LED
 * ------------------------------------------------------------------------- */
#define LED_STATUS_GPIO                 2       /* Onboard LED on most ESP32 devkits */
#define LED_CHECK_PERIOD_MS             500     /* Also doubles as blink half-period */
#define TASK_PRIORITY_LED_INDICATOR     2
#define TASK_STACK_SIZE_LED_INDICATOR   2048

/* ---------------------------------------------------------------------------
 * TWAI (CAN) / OBD-II Configuration
 * ------------------------------------------------------------------------- */
#define OBD_TWAI_TX_GPIO                4
#define OBD_TWAI_RX_GPIO                5
#define OBD_CAN_BITRATE_BPS             500000  /* Standard OBD-II CAN speed */
#define OBD_SAMPLE_PERIOD_MS            500     /* 2 Hz */
#define OBD_REQUEST_TIMEOUT_MS          400
#define OBD_RESPONSE_QUEUE_LEN          5

/* ---------------------------------------------------------------------------
 * ELM327-Original BT (CAN) / OBD-II Configuration
 * ------------------------------------------------------------------------- */

#define OBD_BT_TARGET_MAC              { 0x96, 0x02, 0x00, 0x11, 0x1E, 0x66 }  /* your dongle's MAC */
#define OBD_BT_SPP_SCN                 1
#define OBD_BT_CONNECT_TIMEOUT_MS      8000
#define OBD_BT_RECONNECT_DELAY_MS      3000

/* Standard OBD-II Mode 01 PIDs used by this system */
#define OBD_PID_ENGINE_RPM              0x0C
#define OBD_PID_VEHICLE_SPEED           0x0D
#define OBD_PID_THROTTLE_POSITION       0x11
#define OBD_PID_ENGINE_COOLANT_TEMP     0x05

/* ELM327BLE - nimBLE additions */
#define OBD_BLE_TARGET_ADDR_TYPE BLE_ADDR_PUBLIC


/* ---------------------------------------------------------------------------
 * Task Priorities
 * (0 = lowest ... configMAX_PRIORITIES-1 = highest; default max is 25)
 * Crash detection is the highest-priority task in the system since a missed
 * or delayed crash event is the most safety-critical failure mode.
 * ------------------------------------------------------------------------- */
#define TASK_PRIORITY_CRASH_DETECTION   9
#define TASK_PRIORITY_MPU6050           7
#define TASK_PRIORITY_DRIVER_SCORE      6
#define TASK_PRIORITY_OBD               6
#define TASK_PRIORITY_GPS               5
#define TASK_PRIORITY_IDLING_DETECTION  5
#define TASK_PRIORITY_GEOFENCE          5
#define TASK_PRIORITY_MQTT_PUBLISH      4
#define TASK_PRIORITY_WIFI_MANAGER      3

/* ---------------------------------------------------------------------------
 * Task Stack Sizes (bytes)
 * ------------------------------------------------------------------------- */
#define TASK_STACK_SIZE_CRASH_DETECTION 4096
#define TASK_STACK_SIZE_MPU6050         4096
#define TASK_STACK_SIZE_GPS             4096
#define TASK_STACK_SIZE_OBD             4096
#define TASK_STACK_SIZE_DRIVER_SCORE    4096
#define TASK_STACK_SIZE_IDLING          3072
#define TASK_STACK_SIZE_GEOFENCE        3072
#define TASK_STACK_SIZE_MQTT_PUBLISH    4608
#define TASK_STACK_SIZE_WIFI_MANAGER    4096

/* ---------------------------------------------------------------------------
 * Task Core Pinning (ESP32 is dual-core: PRO_CPU=0, APP_CPU=1)
 * Sensor acquisition is pinned to core 0; scoring/processing and networking
 * share core 1. This keeps time-sensitive sensor sampling isolated from
 * WiFi/MQTT stack jitter.
 * ------------------------------------------------------------------------- */
#define TASK_CORE_SENSOR_ACQUISITION    0
#define TASK_CORE_PROCESSING            1
#define TASK_CORE_NETWORKING            1

/* ---------------------------------------------------------------------------
 * Queue Depths
 * ------------------------------------------------------------------------- */
#define QUEUE_LEN_MPU6050_SAMPLES       20
#define QUEUE_LEN_GPS_SAMPLES           10
#define QUEUE_LEN_OBD_SAMPLES           10
#define QUEUE_LEN_MQTT_PUBLISH          20
#define QUEUE_LEN_ALERT_EVENTS          10
#define QUEUE_LEN_CRASH_EVENTS          5

/* ---------------------------------------------------------------------------
 * Driver Score / Telemetry Timing
 * ------------------------------------------------------------------------- */
#define DRIVER_SCORE_CALC_PERIOD_MS     1000
#define TELEMETRY_PUBLISH_PERIOD_MS     5000
#define STATUS_PUBLISH_PERIOD_MS        30000

/* ---------------------------------------------------------------------------
 * Safety Event Thresholds
 * Values are expressed in g (9.81 m/s^2) for accelerometer-derived events
 * and degrees/second for gyroscope-derived events. These are reasonable
 * defaults based on common commercial fleet-telematics thresholds and
 * should be validated against real vehicle data during calibration.
 * ------------------------------------------------------------------------- */
#define THRESHOLD_HARSH_BRAKING_G       0.40f
#define THRESHOLD_HARSH_ACCEL_G         0.35f
#define THRESHOLD_HARSH_CORNERING_G     0.30f
#define THRESHOLD_CRASH_ACCEL_G         3.50f
#define THRESHOLD_CRASH_GYRO_DPS        300.0f
#define THRESHOLD_OVERSPEED_KMH         120.0f

/* ---------------------------------------------------------------------------
 * Idling Detection
 * ------------------------------------------------------------------------- */
#define IDLING_SPEED_THRESHOLD_KMH      2.0f
#define IDLING_RPM_THRESHOLD            400
#define IDLING_DURATION_THRESHOLD_SEC   180

/* ---------------------------------------------------------------------------
 * Geofencing
 * ------------------------------------------------------------------------- */
#define GEOFENCE_MAX_ZONES              8
#define GEOFENCE_DEFAULT_RADIUS_M       500.0f
#define GEOFENCE_CHECK_PERIOD_MS        2000

/* ---------------------------------------------------------------------------
 * Synchronization
 * ------------------------------------------------------------------------- */
#define MUTEX_MAX_WAIT_MS               1000

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_H */
