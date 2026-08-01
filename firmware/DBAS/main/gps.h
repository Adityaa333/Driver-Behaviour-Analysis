/* ============================================================================
 * gps.h
 *
 * Driver for a UART-based NMEA-0183 GPS module (e.g. NEO-6M/NEO-M8N class).
 *
 * Unlike mpu6050.c, this driver owns its own background FreeRTOS task
 * (started internally by gps_init()) because NMEA parsing requires
 * continuous byte-level UART consumption independent of any fixed sampling
 * period. Consumers call gps_get_latest() to retrieve a thread-safe
 * snapshot of the most recently parsed fix; they never interact with the
 * UART or NMEA parsing directly.
 * ========================================================================= */

#ifndef GPS_H
#define GPS_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief A merged GPS fix combining the most recent RMC (position, speed,
 *        heading, validity) and GGA (altitude, satellite count) sentences.
 */
typedef struct {
    double latitude_deg;        /*!< Latitude in decimal degrees, +N/-S */
    double longitude_deg;       /*!< Longitude in decimal degrees, +E/-W */
    float speed_kmh;            /*!< Ground speed, km/h */
    float heading_deg;          /*!< Course over ground, degrees (0-359.9) */
    float altitude_m;           /*!< Altitude above mean sea level, meters */
    uint8_t satellites_in_use;  /*!< Number of satellites used in fix */
    bool fix_valid;             /*!< True if the last RMC sentence reported
                                      an active ('A') fix status */

    uint8_t fix_quality;        /*!< GGA fix quality: 0=invalid, 1=GPS,
                                      2=DGPS, etc. */
    uint8_t fix_type;           /*!< GSA fix type: 1=no fix, 2=2D, 3=3D */
    float pdop;                 /*!< Position dilution of precision (GSA) */
    float hdop;                 /*!< Horizontal dilution of precision (GSA) */
    float vdop;                 /*!< Vertical dilution of precision (GSA) */

    int64_t timestamp_us;       /*!< Time this snapshot was assembled
                                      (esp_timer), regardless of fix_valid */
} gps_data_t;

/**
 * @brief Configure the GPS UART and start the background NMEA parsing task.
 *
 * Safe to call once during system startup. The parsing task runs for the
 * lifetime of the application; there is no corresponding deinit function
 * since the GPS module is expected to run continuously during a trip.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if the UART driver could not be installed
 *      - ESP_ERR_NO_MEM if the internal mutex or task could not be created
 */
esp_err_t gps_init(void);

/**
 * @brief Retrieve a thread-safe copy of the most recently parsed GPS fix.
 *
 * If no NMEA sentence has been successfully parsed yet, the returned
 * structure will have fix_valid = false and timestamp_us = 0.
 *
 * @param[out] data Pointer to a caller-allocated structure to populate.
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if data is NULL
 *      - ESP_ERR_INVALID_STATE if gps_init() has not been called
 */
esp_err_t gps_get_latest(gps_data_t *data);

/**
 * @brief Get the time elapsed since the last sentence with an active
 *        ('A') fix status was parsed. Useful for consumers (e.g.
 *        geofence, crash_detection) that need to distinguish a live fix
 *        from a stale/lost one before trusting position data.
 *
 * @return Age of the last valid fix in milliseconds, or -1 if no valid
 *         fix has ever been received.
 */
int64_t gps_get_fix_age_ms(void);

/**
 * @brief Evaluate whether the current GPS fix is good enough to trust
 *        for safety-relevant use (e.g. gating a status LED, or a future
 *        "GPS ready" gate on geofence/crash location reporting).
 *
 * A fix is considered ready when all of the following hold on the
 * latest snapshot:
 *   - fix_valid is true (RMC reported an active 'A' fix)
 *   - fix_quality > 0 (GGA did not report "invalid")
 *   - satellites_in_use >= GPS_READY_MIN_SATELLITES
 *   - hdop <= GPS_READY_MAX_HDOP
 *
 * Thresholds are defined in config.h. Returns false (not ready) if
 * gps_init() has not been called or no sentence has ever been parsed,
 * since s_latest's defaults (fix_quality=0, hdop=99.9) fail the check
 * safely.
 *
 * @return true if the fix meets all readiness criteria.
 */
bool gps_is_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* GPS_H */
