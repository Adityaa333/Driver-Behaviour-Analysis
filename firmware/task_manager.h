/* ============================================================================
 * task_manager.h
 *
 * Orchestrates system startup: initializes every module in dependency
 * order, retries transient failures, and reboots on unrecoverable
 * failure of a safety-critical module. Also owns two lightweight
 * periodic tasks that don't belong to any single detector module -
 * raw telemetry publishing and device health/status publishing - since
 * both are simple "read sensor_manager / mqtt_client's own status and
 * publish" loops rather than something with detection logic of its own.
 *
 * This is the only module app_main.c needs to call into.
 * ========================================================================= */

#ifndef TASK_MANAGER_H
#define TASK_MANAGER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize and start every firmware module in the correct
 *        dependency order:
 *
 *          wifi_manager -> mqtt_client -> sensor_manager -> driver_score
 *          -> crash_detection -> idling_detection -> geofence
 *          -> telemetry/status publishing tasks
 *
 * wifi_manager, mqtt_client, and sensor_manager are treated as
 * safety/connectivity-critical: each is retried a bounded number of
 * times, and if still failing, this function triggers a full device
 * reboot via esp_restart() rather than continuing in a state where the
 * system cannot perform its core function. crash_detection and
 * driver_score are equally critical since they are the safety scoring
 * pipeline itself. idling_detection and geofence are treated as
 * non-fatal on failure (logged, boot continues) since the vehicle
 * remains safe to operate and score without them.
 *
 * This function returns once every module has been started; all actual
 * runtime work happens in the background tasks each module's init()
 * function creates (plus the two started directly by this function). It
 * does not return at all in the reboot case, since esp_restart() does
 * not return.
 */
void task_manager_start_all(void);

#ifdef __cplusplus
}
#endif

#endif /* TASK_MANAGER_H */
