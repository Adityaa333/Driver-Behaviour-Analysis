/* ============================================================================
 * led_indicator.h
 *
 * Drives the onboard status LED to reflect GPS fix quality:
 *   - Solid on:   gps_is_ready() is true (valid fix, quality>0, enough
 *                 satellites, HDOP within tolerance)
 *   - Blinking:   not ready (no fix, poor DOP, or too few satellites)
 *
 * This is a thin, self-contained indicator task - it only reads
 * gps_is_ready() and drives one GPIO, so it has no dependency on
 * sensor_manager, driver_score, or MQTT, and failure here is always
 * non-fatal to the rest of the system.
 * ========================================================================= */

#ifndef LED_INDICATOR_H
#define LED_INDICATOR_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configure the status LED GPIO and start the background task
 *        that polls gps_is_ready() every LED_CHECK_PERIOD_MS and drives
 *        the LED solid-on (ready) or blinking (not ready).
 *
 * Must be called after gps_init() has run (i.e. after sensor_manager_init(),
 * since sensor_manager owns the GPS driver's lifecycle). Calling this
 * before gps_init() is not an error - gps_is_ready() simply returns
 * false until the GPS driver is initialized and has a fix.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_NO_MEM if the background task could not be created
 */
esp_err_t led_indicator_init(void);

#ifdef __cplusplus
}
#endif

#endif /* LED_INDICATOR_H */