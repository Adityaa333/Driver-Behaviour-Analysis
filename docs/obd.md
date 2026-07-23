# OBD Module

## Purpose
The `obd` module provides an interface for reading standard OBD-II Mode 01 parameters over the ESP32's TWAI (CAN) controller. It polls the vehicle ECU for diagnostic information and converts the responses into application-friendly values. The module abstracts the CAN request/response protocol from higher-level components.

---

## Responsibilities
- Initialize and start the TWAI (CAN) driver.
- Send Mode 01 PID requests to the vehicle ECU.
- Receive and validate matching OBD-II responses.
- Convert raw PID values into engineering units.
- Monitor CAN bus health and recover automatically from bus-off conditions.

---

## Public API

### `esp_err_t obd_init(void)`
Initializes the TWAI driver using the configured CAN bitrate and starts CAN communication. The function prepares the module for subsequent OBD-II polling.

### `esp_err_t obd_read(obd_data_t *data)`
Polls the ECU for engine RPM, vehicle speed, throttle position, and coolant temperature. Each parameter includes its own validity flag, allowing partial results even if some PID requests time out.

---

## Important Internal Functions

### `obd_check_bus_health_locked()`
Checks the current TWAI controller state and attempts automatic recovery if the CAN controller has entered the BUS_OFF state.

### `obd_request_pid_locked()`
Transmits a single Mode 01 PID request and waits for the corresponding ECU response. Unrelated CAN frames are ignored until a matching response is received or the timeout expires.

---

## Data Flow
The application initializes the module using `obd_init()`. Each call to `obd_read()` first verifies CAN bus health, sequentially requests four predefined PIDs, converts successful responses into engineering units, updates the corresponding validity flags, timestamps the completed read, and returns the populated structure.

---

## Dependencies
The module depends on the ESP-IDF TWAI driver for CAN communication, FreeRTOS mutexes for synchronized bus access, `esp_timer` for timestamps, and ESP logging utilities for diagnostics. Hardware configuration values such as GPIO assignments, bitrate, and timeout settings are provided through `config.h`.

---

## Key Design Decisions
The module performs automatic bus-off recovery before every polling cycle instead of only during initialization. Each PID maintains an independent validity flag so partial sensor data remains usable when individual requests fail. The response-matching logic filters unrelated CAN traffic rather than assuming the next received frame belongs to the requested PID.