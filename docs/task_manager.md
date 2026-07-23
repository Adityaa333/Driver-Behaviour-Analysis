# Task Manager Module

## Purpose
The `task_manager` module orchestrates system startup and runtime initialization for the entire firmware. It initializes modules in dependency order, retries transient failures, starts periodic publishing tasks, and handles fatal startup failures by rebooting the device. This serves as the single entry point called by `app_main()`.

---

## Responsibilities
- Initialize firmware modules in the correct dependency order.
- Retry failed initializations before declaring a fatal error.
- Reboot the device if a critical module cannot be initialized.
- Create background tasks for telemetry and device status publishing.
- Coordinate system startup while delegating runtime work to individual modules.

---

## Public API

### `void task_manager_start_all(void)`
Initializes every firmware module, starts the telemetry and status publishing tasks, and enforces dependency ordering. Critical modules are retried before triggering a system reboot, while optional modules log failures and allow startup to continue.

---

## Important Internal Functions

### `init_with_retry()`
Attempts to initialize a module multiple times before reporting failure. It introduces a delay between attempts to handle transient initialization errors.

### `fatal_init_failure_reboot()`
Logs a fatal startup error, waits briefly to allow log output to flush, and restarts the device.

### `task_manager_telemetry_task()`
Periodically retrieves the latest fused vehicle sample, builds a telemetry JSON payload, and submits it to the MQTT client for publishing.

### `task_manager_status_task()`
Generates periodic device health information including firmware version, uptime, memory usage, WiFi status, MQTT status, RSSI, and driver score before publishing the status payload.

---

## Data Flow
During startup, the module initializes all dependent modules in a predefined sequence. After successful initialization, the telemetry and status tasks periodically gather information from other modules, serialize it into JSON using cJSON, and enqueue the payloads through the MQTT client.

---

## Dependencies
The module depends on `wifi_manager`, `mqtt_client`, `sensor_manager`, `driver_score`, `crash_detection`, `idling_detection`, and `geofence` for system functionality. It also uses FreeRTOS tasks, cJSON for payload generation, ESP logging utilities, system restart functions, and configuration values defined in `config.h`.

---

## Key Design Decisions
Startup follows a strict dependency order to ensure required services are available before dependent modules begin execution. Critical modules trigger bounded retries followed by an automatic reboot if initialization ultimately fails, while optional detector modules are treated as non-fatal. Telemetry and status publishing are centralized here because they are system-wide reporting tasks rather than responsibilities of any individual module.