/* ============================================================================
 * geofence.h
 *
 * Monitors the vehicle's GPS position against a set of circular geofence
 * zones and reports violations. Two zone types are supported to cover
 * the two common fleet use cases:
 *
 *   - GEOFENCE_ZONE_ALLOWED:    violation fires when the vehicle EXITS
 *                                the zone (e.g. "must stay within
 *                                delivery region").
 *   - GEOFENCE_ZONE_RESTRICTED: violation fires when the vehicle ENTERS
 *                                the zone (e.g. "must not enter city
 *                                center low-emission zone").
 *
 * ASSUMPTION: zones are provisioned locally via geofence_add_zone(),
 * called by task_manager/app_main at startup. Remote, dynamic zone
 * provisioning from the cloud backend would require adding MQTT
 * subscribe support to mqtt_client.c (currently publish-only) - flagged
 * here as a natural extension point, not implemented in this module.
 * ========================================================================= */

#ifndef GEOFENCE_H
#define GEOFENCE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GEOFENCE_NAME_MAX_LEN   32

typedef enum {
    GEOFENCE_ZONE_ALLOWED = 0,   /*!< Violation on exit */
    GEOFENCE_ZONE_RESTRICTED,    /*!< Violation on entry */
} geofence_zone_type_t;

/**
 * @brief Start the background geofence monitoring task.
 *
 * Must be called after sensor_manager_init(), mqtt_client_init(), and
 * driver_score_init(). Zones may be added before or after this call.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_NO_MEM if internal synchronization primitives or the
 *        background task could not be created
 */
esp_err_t geofence_init(void);

/**
 * @brief Register a new circular geofence zone.
 *
 * The zone's initial in/out state is established on its first evaluation
 * cycle without firing a violation (there is no way to know whether
 * "starting inside an ALLOWED zone" or "starting outside a RESTRICTED
 * zone" represents a pre-existing violation, so the first observation is
 * treated as a baseline rather than a transition).
 *
 * @param[in]  name           Human-readable zone name (for alert
 *                             payloads), truncated to GEOFENCE_NAME_MAX_LEN-1
 *                             characters.
 * @param[in]  center_lat_deg Zone center latitude, decimal degrees.
 * @param[in]  center_lon_deg Zone center longitude, decimal degrees.
 * @param[in]  radius_m       Zone radius, meters (must be > 0).
 * @param[in]  type           Zone type (allowed or restricted).
 * @param[out] out_zone_index Optional; receives the assigned zone index
 *                             (usable with geofence_remove_zone()). Pass
 *                             NULL if not needed.
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if name is NULL, radius_m <= 0
 *      - ESP_ERR_NO_MEM if GEOFENCE_MAX_ZONES zones are already registered
 *      - ESP_ERR_INVALID_STATE if geofence_init() has not been called
 */
esp_err_t geofence_add_zone(const char *name, double center_lat_deg, double center_lon_deg,
                             float radius_m, geofence_zone_type_t type, uint8_t *out_zone_index);

/**
 * @brief Remove a previously registered geofence zone.
 *
 * @param[in] zone_index Index returned by geofence_add_zone().
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if zone_index is out of range or not in use
 *      - ESP_ERR_INVALID_STATE if geofence_init() has not been called
 */
esp_err_t geofence_remove_zone(uint8_t zone_index);

#ifdef __cplusplus
}
#endif

#endif /* GEOFENCE_H */
