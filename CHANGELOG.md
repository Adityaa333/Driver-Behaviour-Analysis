# Changelog

All notable changes to the Driver Behaviour Analysis System (DBAS) project will be documented in this file.
---

## [3.1.1] - 2026-08-01

### Added
- Added indicator task for GPS satellite connection success based on number of satellites & DOP.

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
