# Driver Score Module

## Overview

The `driver_score` module maintains a cumulative trip safety score (0–100). It combines internally detected driving behaviour (harsh braking, acceleration, cornering, overspeeding) with externally reported events (crash, idling, geofence violations) to calculate and periodically publish the driver's safety score.

---

## Responsibilities

- Maintain the current driver score.
- Detect harsh driving events from sensor data.
- Receive events from other modules.
- Update cumulative driving statistics.
- Recalculate score periodically.
- Publish score over MQTT.

---

## Public API

### `driver_score_init()`

Initializes the scoring engine.

Creates:

- State mutex
- Event queue
- Driver score task

---

### `driver_score_submit_event()`

Allows other modules to submit scoring events.

Used by:

- Crash Detection
- Idling Detection
- Geofence

---

### `driver_score_get_current()`

Returns the latest calculated driver score.

---

## Internal Functions

### `driver_score_apply_event_locked()`

Updates cumulative counters for:

- Harsh braking
- Harsh acceleration
- Harsh cornering
- Overspeed
- Idling
- Crash
- Geofence violations

---

### `driver_score_recompute_locked()`

Calculates the driver score using weighted penalties.

The score:

- Starts at 100.
- Decreases as events accumulate.
- Is clamped between 0 and 100.

---

### `driver_score_detect_harsh_events()`

Detects driving events directly from sensor data.

Internally detects:

- Harsh braking
- Harsh acceleration
- Harsh cornering
- Overspeed

Uses edge-trigger detection so a continuous event is counted only once.

---

### `driver_score_publish()`

- Recomputes score.
- Creates MQTT JSON payload.
- Publishes current score and statistics.

---

### `driver_score_task()`

Runs continuously and:

- Processes queued external events.
- Reads latest sensor sample.
- Detects harsh events.
- Periodically publishes the updated score.

---

## Data Used

Inputs:

- `vehicle_sample_t`
- External `driver_event_t`

Outputs:

- Updated score
- MQTT score payload

---

## Design Decisions

- Uses a cumulative trip score rather than a rolling window.
- Detects harsh events internally.
- Uses a queue for asynchronous events from other modules.
- Protects shared scoring state using a mutex.
- Prefers OBD speed over GPS speed for overspeed detection.

---

