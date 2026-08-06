# Changelog

All notable changes to the Driver Behaviour Analysis System (DBAS) project will be documented in this file.
---

## [3.1.1] - 2026-08-06

### Added
- Dashboard: Geofence Status panel and map rendering of the configured geofence zone (center + radius). The map now draws the geofence circle to make violations visible in the Vehicle Location view.
- Dashboard: Score breakdown toggle (expand/collapse) and integration of score-derived alerts into the Recent Alerts list so safety events from score are visible in alerts.

### Changed
- Dashboard layout: experimented with matching heights for the Vehicle Location panel and the stacked Crash / Geofence panels to improve visual balance. Per user request the previous layout was restored and the geofence UI preserved.
- Telemetry: engine coolant temperature flow was reviewed and kept (firmware -> backend -> dashboard) so engine temperature is available in live stats when present.

### Fixed
- Dashboard: resolved a runtime error caused by a missing icon import (ShieldCheckIcon) that previously broke dashboard rendering.
- Backend: fixed a server-side error affecting /score/history that could return 500; score history now includes rating mapping reliably.
- Firmware: reverted temporary GPS raw NMEA sentence logging (was added for debugging) so normal log verbosity is restored.

### Removed
- Experimental LED indicator module (led_indicator) was added during debugging and subsequently removed; no lasting functional change.

---

## [3.0.1] - 2026-08-01

### Fixed
- Corrected telemetry output timestamp bug.
- Improved timestamp consistency across MQTT messages.

---

## [3.0.0] - 2026-07-30

### Added
- Bluetooth Low Energy (BLE) support for ELM327 communication.
- Updated geofencing zones for improved route monitoring.

### Changed
- Replaced Classic Bluetooth communication with BLE.
- Improved Bluetooth communication architecture for lower power consumption and better device compatibility.

### Removed
- Classic Bluetooth implementation.

---

## [2.1.2] - 2026-07-28

### Added
- MPU6500 compatibility.

### Fixed
- GPS NMEA checksum validation.
- Improved GPS parser reliability for corrupted or incomplete sentences.

### Improved
- Sensor initialization stability.
- IMU compatibility across supported devices.

---

## [2.1.1] - 2026-07-26

### Improved
- Backend integration.
- MQTT communication reliability.
- Firmware communication stability.

---

## [2.1.0] - 2026-07-24

### Added
- Bluetooth communication with ELM327 OBD-II adapters.
- Global `config.h` configuration system for project-wide settings.

### Changed
- Replaced wired CAN/TWAI OBD implementation with Bluetooth-based communication.
- Simplified hardware architecture by eliminating direct CAN controller dependency.

### Removed
- Wired CAN/TWAI OBD communication.

---

## [2.0.0] - 2026-07-22

### Added
- OBD-II integration.
- ELM327 support.
- Sensor Manager architecture for centralized sensor handling.

### Improved
- Modular firmware organization.
- Sensor abstraction and scalability.

---

## [1.4.0] - 2026-07-20

### Added
- Crash detection module.
- Driver alert generation.
- Priority-based MQTT telemetry transmission.

### Improved
- Critical event handling.
- Emergency telemetry prioritization.

---

## [1.3.0] - 2026-07-18

### Added
- Driver Behaviour Scoring algorithm.
- Harsh braking detection.
- Harsh cornering detection.
- Geofencing feature.
- MQTT telemetry publishing.

### Improved
- Driver behaviour analysis accuracy.
- Real-time telemetry generation.

---

## [1.2.0] - 2026-07-16

### Added
- Backend infrastructure for telemetry processing.

### Fixed
- FreeRTOS mutex synchronization issues.

### Improved
- Task communication reliability.
- Backend integration workflow.

---

## [1.0.0] - 2026-07-15

### Added
- Initial ESP-IDF project structure.
- Initial firmware build.
- MPU6050 sensor integration.
- GPS module integration.
- UART and I2C communication protocols.
- Basic sensor data acquisition.
- MQTT broker connectivity.
- Sensor data publishing over MQTT.
- FreeRTOS task framework.
- FreeRTOS logging and debugging.

### Improved
- Initial modular firmware organization.
- Sensor initialization sequence.
