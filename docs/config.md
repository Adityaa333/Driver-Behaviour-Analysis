# Configuration Module

## Purpose
The `config` module provides a centralized collection of compile-time constants used throughout the firmware. It defines hardware configuration, communication parameters, task scheduling values, safety thresholds, and system tuning parameters so that all modules share a consistent configuration source.

---

## Responsibilities
- Define firmware identification information.
- Store hardware interface and GPIO assignments.
- Configure communication settings for WiFi, MQTT, I2C, UART, and CAN.
- Provide task priorities, stack sizes, and CPU core assignments.
- Define timing intervals, queue sizes, and safety thresholds.

---

## Public API

### Configuration Macros
The module exposes compile-time macros grouped by functional area, including:
- Firmware identification and device information.
- WiFi and MQTT communication settings.
- I2C, GPS, and OBD hardware configuration.
- Task priorities, stack sizes, and CPU core assignments.
- Queue capacities and periodic task intervals.
- Detection thresholds for crash, harsh driving, idling, and geofencing.
- Synchronization and timeout values used across the firmware.

---

## Important Internal Components

### Hardware Configuration
Defines GPIO assignments, communication ports, baud rates, bus speeds, and device addresses required by the supported peripherals.

### System Configuration
Provides task scheduling parameters, memory allocation sizes, queue depths, and timing intervals used by FreeRTOS tasks.

### Detection Thresholds
Defines configurable limits used by detection modules, including crash acceleration, harsh driving thresholds, overspeed limits, idling conditions, and geofence settings.

---

## Data Flow
The configuration module contains only compile-time constants and does not maintain runtime state. Other modules include `config.h` to obtain hardware settings, timing parameters, thresholds, and system configuration values during compilation.

---

## Dependencies
The module includes standard integer types along with ESP-IDF I2C and UART driver definitions required by configuration macros. It serves as a common dependency for nearly every firmware module.

---

## Key Design Decisions
All configurable parameters are centralized to simplify maintenance and ensure consistency across the firmware. Related settings are organized into functional sections, making hardware migration and parameter tuning possible without modifying implementation files. Runtime configuration is intentionally avoided, allowing values to be optimized at compile time.