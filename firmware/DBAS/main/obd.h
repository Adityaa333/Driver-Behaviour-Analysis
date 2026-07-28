// Design notes:

// Per-PID validity flags (rather than one pass/fail for the whole read)
//  mean a single noisy CAN frame doesn't invalidate the entire telemetry cycle — 
//  driver_score.c can still use RPM even if throttle position timed out that cycle.
// obd_read() is documented as blocking up to ~4× the per-request timeout, since it's a 
// sequential request/response protocol, not something that can be parallelized on a single CAN bus
//  — important for whoever sizes the calling task's period in task_manager.c.

/* ============================================================================
 * obd.h
 *
 * Driver for reading standard OBD-II Mode 01 PIDs over the ESP32's built-in
 * TWAI (CAN) controller, via an external CAN transceiver wired to the
 * vehicle's OBD-II diagnostic bus.
 *
 * Unlike gps.c, this driver is request/response rather than a continuous
 * stream: obd_read() actively requests each PID and waits for the ECU's
 * reply, since the OBD-II bus only reports data when polled. Each field
 * carries its own validity flag because a single PID request can time out
 * (e.g. transient bus noise, ECU busy) without the whole read failing.
 * ========================================================================= */

#ifndef OBD_H
#define OBD_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief One polling cycle's worth of OBD-II PID readings.
 *
 * Each value has a corresponding *_valid flag. A field should only be
 * used by consumers if its valid flag is true; a timed-out PID request
 * leaves the previous numeric value in place but its valid flag is
 * always recomputed for the current read.
 */
typedef struct {
    uint16_t engine_rpm;            /*!< Engine speed, RPM */
    bool engine_rpm_valid;

    float vehicle_speed_kmh;        /*!< Vehicle speed, km/h */
    bool vehicle_speed_valid;

    float throttle_position_pct;    /*!< Throttle position, 0-100% */
    bool throttle_position_valid;

    int16_t coolant_temp_c;         /*!< Engine coolant temperature, Celsius */
    bool coolant_temp_valid;

    int64_t timestamp_us;           /*!< Time this read cycle completed */
} obd_data_t;

/**
 * @brief Configure and start the TWAI (CAN) driver at the bitrate defined
 *        by OBD_CAN_BITRATE_BPS in config.h (500 kbps, standard for
 *        OBD-II).
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if the TWAI driver could not be installed
 *        or started
 */
esp_err_t obd_init(void);

/**
 * @brief Poll the vehicle ECU for engine RPM, vehicle speed, throttle
 *        position, and coolant temperature, in that order.
 *
 * Each PID is requested individually (functional broadcast to 0x7DF) and
 * this function blocks until either a matching response arrives or
 * OBD_REQUEST_TIMEOUT_MS elapses, for each of the four PIDs in turn. Worst
 * case this call blocks for approximately 4 * OBD_REQUEST_TIMEOUT_MS.
 *
 * @param[out] data Pointer to a caller-allocated structure to populate.
 *                   Fields whose corresponding PID request timed out will
 *                   have their *_valid flag set to false.
 * @return
 *      - ESP_OK if at least one PID was read successfully
 *      - ESP_ERR_TIMEOUT if none of the four PIDs received a response
 *        (typical when the vehicle ignition is off or the CAN bus is
 *        disconnected)
 *      - ESP_ERR_INVALID_ARG if data is NULL
 *      - ESP_ERR_INVALID_STATE if obd_init() has not been called
 */
esp_err_t obd_read(obd_data_t *data);

#ifdef __cplusplus
}
#endif

#endif /* OBD_H */
