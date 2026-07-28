/* ============================================================================
 * wifi_manager.h
 *
 * Manages the ESP32 WiFi station (STA) connection lifecycle: initial
 * connection, automatic reconnection with bounded fast retries followed
 * by a backoff delay, and connection-status queries for other modules
 * (mqtt_client waits on this before attempting to connect to the broker).
 *
 * This module never gives up permanently on reconnection - a fleet device
 * that loses WiFi (e.g. driving out of yard coverage) must keep retrying
 * indefinitely and recover automatically when back in range, without
 * requiring a reboot.
 * ========================================================================= */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize NVS (required by the WiFi driver), the network
 *        interface, event loop, and WiFi driver, then begin connecting to
 *        the network configured by WIFI_SSID/WIFI_PASSWORD in config.h.
 *
 * This function returns once the connection attempt has been started; it
 * does not block until connected. Use wifi_manager_wait_connected() if
 * the caller needs to block until a connection is established.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if NVS, the event loop, or the WiFi driver
 *        could not be initialized
 *      - ESP_ERR_NO_MEM if internal synchronization primitives could not
 *        be created
 */
esp_err_t wifi_manager_init(void);

/**
 * @brief Check whether the station currently holds a valid IP address.
 *
 * @return true if connected, false otherwise.
 */
bool wifi_manager_is_connected(void);

/**
 * @brief Block the calling task until a connection is established or the
 *        timeout elapses.
 *
 * @param[in] timeout_ms Maximum time to wait, in milliseconds.
 * @return
 *      - ESP_OK if connected within the timeout
 *      - ESP_ERR_TIMEOUT if the timeout elapsed while still disconnected
 *      - ESP_ERR_INVALID_STATE if wifi_manager_init() has not been called
 */
esp_err_t wifi_manager_wait_connected(uint32_t timeout_ms);

/**
 * @brief Get the current access point's received signal strength, for
 *        inclusion in periodic device status telemetry.
 *
 * @param[out] rssi_dbm Pointer to receive the RSSI in dBm.
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if rssi_dbm is NULL
 *      - ESP_ERR_INVALID_STATE if not currently connected
 */
esp_err_t wifi_manager_get_rssi(int8_t *rssi_dbm);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_MANAGER_H */
