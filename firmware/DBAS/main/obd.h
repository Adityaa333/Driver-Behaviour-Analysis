// Design notes:
//
// Per-PID validity flags (rather than one pass/fail for the whole read)
// mean a single dropped BLE notification doesn't invalidate the entire
// telemetry cycle - driver_score.c can still use RPM even if throttle
// position timed out that cycle.
//
// obd_read() is documented as blocking up to ~4x the per-request timeout,
// since it's a sequential request/response protocol over a single
// notify/write pair, not something that can be parallelized - important
// for whoever sizes the calling task's period in task_manager.c.

/* ============================================================================
 * obd.h
 *
 * Driver for reading standard OBD-II Mode 01 PIDs from a BLE
 * ("UART-over-BLE") ELM327-compatible dongle, using ESP-IDF's NimBLE GATT
 * client stack (NOT Bluedroid, NOT Classic Bluetooth SPP).
 *
 * TRANSPORT HISTORY: this module previously used a wired CAN transceiver
 * (TWAI), then a Bluetooth Classic SPP link to an ELM327 dongle. A BLE
 * scan of the actual hardware (see ble_scanner log, 2026-07-31) showed
 * the dongle exposes GATT service FFF0 with a notify characteristic
 * FFF1 and a write characteristic FFF2 - a common "UART bridge" pattern
 * used by cheap ELM327 BLE clones - so neither TWAI nor SPP applies.
 * NimBLE was chosen over Bluedroid specifically for flash/RAM footprint:
 * Bluedroid pulls in the full Classic+BLE dual-mode stack even when only
 * a BLE central/GATT-client role is needed, while NimBLE is ESP-IDF's
 * BLE-only host stack. See the accompanying config.h/sdkconfig notes for
 * the Kconfig options that make this savings real (NimBLE alone in the
 * component list doesn't shrink the binary - the sdkconfig role/feature
 * flags do the actual work).
 *
 * The public contract (obd_data_t, obd_init(), obd_read()) is UNCHANGED
 * from the SPP version, so sensor_manager.c and everything downstream of
 * it needs no modification.
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
 * @brief Initialize the NimBLE host stack, start the background
 *        connection-management task (which connects to
 *        OBD_BLE_TARGET_MAC, discovers the FFF0 service and its FFF1/
 *        FFF2 characteristics, subscribes to FFF1 notifications, and
 *        runs the ELM327 AT setup sequence), and return immediately.
 *
 * Connection/reconnection happens entirely in the background, mirroring
 * wifi_manager.c's philosophy: a dropped BLE link (dongle out of range,
 * bike powered off) recovers on its own without requiring a device
 * reboot, and obd_read() simply reports ESP_ERR_TIMEOUT while
 * disconnected.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_NO_MEM if internal synchronization primitives, queues,
 *        or the background task could not be created
 *      - ESP_ERR_INVALID_STATE if the NimBLE host could not be
 *        initialized
 */
esp_err_t obd_init(void);

/**
 * @brief Poll the connected dongle for engine RPM, vehicle speed,
 *        throttle position, and coolant temperature, in that order.
 *
 * Each PID is sent as a write-without-response to the FFF2
 * characteristic and this function blocks until either a complete
 * response (terminated by ELM327's '>' prompt, possibly reassembled
 * from multiple BLE notification packets) arrives on FFF1 or
 * OBD_REQUEST_TIMEOUT_MS elapses, for each of the four PIDs in turn.
 * Worst case this call blocks for approximately 4 * OBD_REQUEST_TIMEOUT_MS.
 *
 * @param[out] data Pointer to a caller-allocated structure to populate.
 *                   Fields whose corresponding PID request timed out will
 *                   have their *_valid flag set to false.
 * @return
 *      - ESP_OK if at least one PID was read successfully
 *      - ESP_ERR_TIMEOUT if the dongle is not currently connected/
 *        subscribed, or none of the four PIDs received a response
 *        (typical when ignition is off or the dongle is out of range)
 *      - ESP_ERR_INVALID_ARG if data is NULL
 *      - ESP_ERR_INVALID_STATE if obd_init() has not been called
 */
esp_err_t obd_read(obd_data_t *data);

#ifdef __cplusplus
}
#endif

#endif /* OBD_H */
