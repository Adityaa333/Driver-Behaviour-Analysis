/* ============================================================================
 * elm327_sim.h
 *
 * NimBLE GATT SERVER that impersonates a cheap "UART-bridge" ELM327 BLE
 * OBD-II clone - the same class of device DBAS's own obd.c (a NimBLE
 * GATT CLIENT) is written to talk to. Advertises service 0xFFF0 with a
 * notify characteristic 0xFFF1 (adapter -> DBAS) and a write
 * characteristic 0xFFF2 (DBAS -> adapter), accepts a single central
 * connection, and answers a realistic subset of the ELM327 AT command
 * set plus four Mode 01 PIDs using live values pulled from
 * pid_generator.
 *
 * This module owns BLE transport and ELM327 text-protocol framing only.
 * It has no opinion about *what* RPM/speed/coolant/throttle should read
 * right now - that's pid_generator's job - keeping BLE communication
 * cleanly separated from telemetry generation per the project's
 * modularity requirement.
 * ========================================================================= */

#ifndef ELM327_SIM_H
#define ELM327_SIM_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the NimBLE host stack, register the FFF0 GATT
 *        service, start the background command-processing task, and
 *        begin advertising as a BLE OBD-II adapter.
 *
 * Must be called after pid_generator_init(), since the command
 * processor reads live PID values as soon as the first request arrives
 * (which can happen immediately after connection).
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_NO_MEM if internal queues/tasks could not be created
 *      - ESP_ERR_INVALID_STATE if the NimBLE host could not be
 *        initialized
 */
esp_err_t elm327_sim_init(void);

#ifdef __cplusplus
}
#endif

#endif /* ELM327_SIM_H */
