# Idling Detection Module

## Purpose
The `idling_detection` module monitors vehicle operating conditions to detect prolonged engine idling. It evaluates engine RPM and vehicle speed using fused sensor data, reports excessive idling through MQTT, and submits idling events to the driver scoring system.

---

## Responsibilities
- Monitor engine RPM and vehicle speed continuously.
- Detect idle sessions based on configurable thresholds.
- Generate a single MQTT alert when an idle session exceeds the configured duration.
- Submit incremental idling events to the driver scoring module.
- Execute idling detection in a dedicated background task.

---

## Public API

### `esp_err_t idling_detection_init(void)`
Creates and starts the background idling detection task. The module operates autonomously after initialization and requires no additional interaction from other components.

---

## Important Internal Functions

### `idling_detection_task()`
Continuously retrieves the latest fused vehicle sample, tracks idle session state, detects threshold crossings, reports excessive idling, and submits incremental scoring events while idling continues.

### `idling_report_threshold_exceeded()`
Builds an MQTT alert containing the idling duration, engine RPM, timestamp, and GPS location when available, then submits the alert through the MQTT client.

---

## Data Flow
The background task periodically retrieves the latest vehicle sample from the sensor manager. When engine RPM remains above the configured threshold while vehicle speed stays below the configured limit for the required duration, the module publishes an alert once and continues submitting periodic idling events to the driver scoring module until the idle session ends.

---

## Dependencies
The module depends on `sensor_manager` for fused vehicle data, `driver_score` for score updates, `mqtt_client` for alert publishing, cJSON for payload generation, FreeRTOS tasks, ESP timers, ESP logging utilities, and configuration values defined in `config.h`.

---

## Key Design Decisions
The module sends only one MQTT alert per idle session to prevent repeated notifications during extended idling. Driver score updates continue throughout the session using incremental events so the accumulated penalty reflects the total idling duration. Vehicle speed is taken from OBD data when available and falls back to GPS speed if necessary, improving reliability across different operating conditions.