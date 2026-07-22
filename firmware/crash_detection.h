/* ============================================================================
 * crash_detection.h
 *
 * Detects severe impact events from IMU data and immediately reports
 * them via MQTT (highest QoS) and to driver_score. This is the highest
 * -priority task in the system (TASK_PRIORITY_CRASH_DETECTION in
 * config.h) since a missed or delayed crash detection is the most
 * safety-critical failure mode a driver behaviour system can have.
 *
 * Detection uses the total (vector-magnitude) acceleration and angular
 * rate rather than a single axis, since a crash impact can occur from
 * any direction relative to the vehicle (unlike braking/acceleration/
 * cornering in driver_score.c, which are meaningfully axis-specific).
 * ========================================================================= */

#ifndef CRASH_DETECTION_H
#define CRASH_DETECTION_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the background crash monitoring task.
 *
 * Must be called after sensor_manager_init(), mqtt_client_init(), and
 * driver_score_init(), since a confirmed crash reports to both.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_NO_MEM if the background task could not be created
 */
esp_err_t crash_detection_init(void);

#ifdef __cplusplus
}
#endif

#endif /* CRASH_DETECTION_H */
