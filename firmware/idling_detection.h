/* ============================================================================
 * idling_detection.h
 *
 * Detects excessive engine idling: engine running (RPM at or above
 * IDLING_RPM_THRESHOLD) while the vehicle is stationary (speed at or
 * below IDLING_SPEED_THRESHOLD_KMH) for longer than
 * IDLING_DURATION_THRESHOLD_SEC (all in config.h).
 *
 * Like crash_detection and geofence, this module is self-contained: it
 * reads samples from sensor_manager, reports an MQTT alert once per
 * qualifying idle session, and feeds incremental idle time to
 * driver_score for as long as excessive idling continues. No other
 * module needs to interact with it directly, so its public API is just
 * the init function.
 * ========================================================================= */

#ifndef IDLING_DETECTION_H
#define IDLING_DETECTION_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the background idling monitoring task.
 *
 * Must be called after sensor_manager_init(), mqtt_client_init(), and
 * driver_score_init().
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_NO_MEM if the background task could not be created
 */
esp_err_t idling_detection_init(void);

#ifdef __cplusplus
}
#endif

#endif /* IDLING_DETECTION_H */
