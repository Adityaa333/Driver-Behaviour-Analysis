/* ============================================================================
 * mqtt_client.h
 *
 * Manages the MQTT connection to the cloud broker and provides topic-
 * specific publish functions for the rest of the firmware.
 *
 * Design: callers never touch the underlying esp-mqtt client directly.
 * Each mqtt_client_publish_*() function formats the appropriate topic
 * (using this device's unique ID) and enqueues the message onto an
 * internal FreeRTOS queue; a single background task owns the actual
 * network client and drains that queue. This means sensor/scoring tasks
 * are never blocked by network I/O timing, and only one task ever touches
 * the mqtt client handle.
 *
 * This module also owns generation of the device's unique ID (derived
 * from the ESP32's factory-programmed MAC address), since the ID is
 * needed both for the MQTT client ID and for every topic string.
 * ========================================================================= */

#ifndef DBAS_MQTT_CLIENT_H
#define DBAS_MQTT_CLIENT_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Generate this device's unique ID, create the internal publish
 *        queue, and start the background MQTT connection task.
 *
 * The connection task waits for WiFi (via wifi_manager_wait_connected())
 * before starting the MQTT client, and the esp-mqtt component handles
 * broker-level reconnection automatically once started.
 *
 * Must be called after wifi_manager_init().
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_NO_MEM if the publish queue or background task could
 *        not be created
 *      - ESP_ERR_INVALID_STATE if the device MAC address could not be
 *        read
 */
esp_err_t mqtt_client_init(void);

/**
 * @brief Check whether the MQTT client currently holds an active broker
 *        connection (i.e. has received CONNACK).
 *
 * @return true if connected, false otherwise.
 */
bool mqtt_client_is_connected(void);

/**
 * @brief Get this device's unique ID (12 uppercase hex characters derived
 *        from the factory MAC address), used consistently across MQTT
 *        topics and telemetry payloads.
 *
 * @return Pointer to a NUL-terminated, internally-owned static string.
 *         Valid for the lifetime of the application. Returns an empty
 *         string if mqtt_client_init() has not been called successfully.
 */
const char *mqtt_client_get_device_id(void);

/**
 * @brief Publish a telemetry payload (raw sensor + OBD snapshot) at
 *        MQTT_QOS_TELEMETRY.
 * @param[in] json_payload NUL-terminated JSON string, must be shorter
 *                          than MQTT_MAX_PAYLOAD_LEN.
 * @return ESP_OK if enqueued, ESP_ERR_INVALID_ARG if json_payload is NULL
 *         or too long, ESP_ERR_NO_MEM if the publish queue is full.
 */
esp_err_t mqtt_client_publish_telemetry(const char *json_payload);

/**
 * @brief Publish a computed driver safety score at MQTT_QOS_TELEMETRY.
 * @see mqtt_client_publish_telemetry() for parameter/return semantics.
 */
esp_err_t mqtt_client_publish_score(const char *json_payload);

/**
 * @brief Publish a non-crash safety event (harsh braking, overspeed,
 *        idling, geofence violation, etc.) at MQTT_QOS_ALERT.
 * @see mqtt_client_publish_telemetry() for parameter/return semantics.
 */
esp_err_t mqtt_client_publish_alert(const char *json_payload);

/**
 * @brief Publish a crash detection event at MQTT_QOS_CRASH. This is the
 *        highest-priority, highest-QoS message type in the system.
 * @see mqtt_client_publish_telemetry() for parameter/return semantics.
 */
esp_err_t mqtt_client_publish_crash(const char *json_payload);

/**
 * @brief Publish periodic device health/status (uptime, RSSI, free heap,
 *        etc.) at MQTT_QOS_STATUS.
 * @see mqtt_client_publish_telemetry() for parameter/return semantics.
 */
esp_err_t mqtt_client_publish_status(const char *json_payload);

#ifdef __cplusplus
}
#endif

#endif /* MQTT_CLIENT_H */
