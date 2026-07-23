# Crash Detection Module

## Overview

The `crash_detection` module continuously monitors the latest fused vehicle sensor data and detects severe impact events. It runs as a dedicated high-priority FreeRTOS task, computes the total acceleration and angular velocity, and declares a crash if either exceeds predefined thresholds. Once a crash is confirmed, it immediately publishes a crash alert via MQTT and reports the event to the Driver Score module.

---

## Responsibilities

- Periodically monitor the latest vehicle sensor sample.
- Compute total acceleration and angular velocity magnitudes.
- Detect crashes using configurable thresholds.
- Publish crash information through MQTT.
- Submit crash events to the Driver Score module.
- Suppress duplicate crash detections using a cooldown timer.

---

## Public API

### `crash_detection_init()`

Initializes the crash detection module by creating the background monitoring task.

**Returns**

- `ESP_OK` on success.
- `ESP_ERR_NO_MEM` if task creation fails.

---

## Internal Functions

### `crash_is_confirmed()`

Calculates:

- Total acceleration magnitude.
- Total gyroscope magnitude.

Returns `true` if either exceeds the configured crash threshold.

---

### `crash_detection_report()`

Handles crash reporting by:

- Creating a JSON payload containing crash details.
- Publishing the payload over MQTT.
- Creating a `DRIVER_EVENT_CRASH`.
- Submitting the event to `driver_score`.

---

### `crash_detection_task()`

Runs continuously and performs the following:

- Reads the latest vehicle sample.
- Checks whether cooldown has expired.
- Evaluates crash conditions.
- Reports confirmed crashes.
- Starts cooldown to prevent duplicate reports.

---

## Data Used

Input:

- `vehicle_sample_t` from `sensor_manager`

Output:

- MQTT crash alert
- `DRIVER_EVENT_CRASH` sent to `driver_score`

---

## Design Decisions

- Uses vector magnitude instead of individual axes so crashes from any direction can be detected.
- Uses a cooldown period to avoid multiple reports from the same collision.
- Runs as one of the highest-priority tasks due to its safety-critical nature.

---

