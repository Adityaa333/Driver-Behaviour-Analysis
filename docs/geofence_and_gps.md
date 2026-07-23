# Geofence Module

## Overview

The `geofence` module continuously monitors the vehicle's GPS position against a set of user-defined circular geofence zones. It detects when the vehicle enters or exits a zone based on its type and reports violations through MQTT while also notifying the Driver Score module.

---

## Responsibilities

- Store registered geofence zones.
- Periodically read the latest GPS position.
- Calculate the distance between the vehicle and each zone.
- Detect entry/exit transitions.
- Generate violation alerts.
- Submit geofence violation events to the Driver Score module.

---

## Public API

### `geofence_init()`

Initializes the module by:

- Creating the zone mutex.
- Clearing all stored zones.
- Starting the background monitoring task.

---

### `geofence_add_zone()`

Registers a new circular geofence.

Stores:

- Zone name
- Center latitude
- Center longitude
- Radius
- Zone type (Allowed/Restricted)

Returns the assigned zone index if requested.

---

### `geofence_remove_zone()`

Removes a previously registered zone using its index.

---

## Internal Functions

### `geofence_distance_m()`

Calculates the great-circle distance between the current vehicle location and a zone center using the Haversine formula.

Returns the distance in meters.

---

### `geofence_report_violation()`

Handles violation reporting by:

- Creating an MQTT alert payload.
- Publishing the alert.
- Creating a `DRIVER_EVENT_GEOFENCE_VIOLATION`.
- Submitting the event to the Driver Score module.

---

### `geofence_task()`

Runs continuously and:

- Retrieves the latest vehicle sample.
- Ignores processing if GPS is unavailable.
- Computes distance to every active zone.
- Detects zone transitions.
- Reports violations when required.
- Updates the stored zone state.

---

## Zone Types

### Allowed Zone

Vehicle is expected to stay inside.

Violation occurs when:

- Vehicle exits the zone.

---

### Restricted Zone

Vehicle is expected to stay outside.

Violation occurs when:

- Vehicle enters the zone.

---

## Design Decisions

- Uses the Haversine formula for accurate Earth-surface distance calculations.
- Establishes the initial inside/outside state without triggering an alert.
- Detects only state transitions rather than continuously reporting while inside/outside.
- Supports multiple simultaneously active zones.
- Protects shared zone data using a mutex.
- Depends on valid GPS fixes before performing any geofence checks.

---

