# Driver Behaviour Analysis System

![C](https://img.shields.io/badge/C-Language-blue)
![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.x-red)
![Node-RED](https://img.shields.io/badge/Node--RED-Dashboard-red)
![ESP32](https://img.shields.io/badge/ESP32-Firmware-orange)

## Project Overview

The Driver Behaviour Analysis System is an embedded IoT project designed to monitor and evaluate driver safety using an ESP32. It collects real-time data from onboard sensors such as the IMU, GPS, and optional OBD-II interface to detect unsafe driving events, calculate a driver safety score, and publish telemetry over MQTT for real-time visualization on a dashboard.

---

## Features

- **Real-Time Telemetry**: Collects data from IMU, GPS, and optional OBD-II sensors.
- **Driver Scoring**: Calculates a safety score based on driving behaviour such as harsh braking, acceleration, and cornering.
- **Crash Detection**: Detects severe impact events using acceleration thresholds.
- **Geo-fencing**: Monitors entry/exit from predefined zones.
- **MQTT Communication**: Publishes telemetry and driver statistics in real time.
- **Dashboard**: Visualizes telemetry and driver scores through a Node-RED dashboard.
- **Alerts**: Publishes safety alerts for geofence violations, crashes, and idling.
- **Modular Firmware**: Organized into independent drivers and FreeRTOS tasks for easy maintenance and scalability.

---

## Tech Stack

| Component | Technology |
|-------------------|--------------------|
| Firmware | ESP32, ESP-IDF, FreeRTOS |
| Language | C |
| Communication | MQTT |
| Sensors | MPU6050, GPS, Optional OBD-II |
| Dashboard | Node-RED |

---

## System Architecture

The system consists of an ESP32-based firmware that interfaces with vehicle sensors, processes driver behaviour locally, and publishes telemetry over MQTT. The published data can be visualized using a Node-RED dashboard for real-time monitoring.

---

## Folder Structure

- **firmware/**: ESP32 firmware, drivers, and application logic.
- **backend/**: Supporting backend services for telemetry processing and integration.
- **ML/**: Experimental machine learning models and datasets for driver behaviour analysis.
- **dashboard/**: Node-RED flow configuration for real-time visualization.
- **docs/**: Project documentation.

---

## How It Works

1. **Data Collection**: The ESP32 continuously reads data from the IMU, GPS, and optional OBD-II interface.
2. **Data Processing**: FreeRTOS tasks process sensor data to detect unsafe driving events and calculate a driver safety score.
3. **Data Transmission**: Processed telemetry is published to an MQTT broker over Wi-Fi.
4. **Visualization**: The Node-RED dashboard subscribes to MQTT topics and displays real-time telemetry and driver statistics.

---

## Hardware Requirements

- **ESP32 Development Board**: Main processing unit.
- **MPU6050**: Accelerometer and gyroscope sensor.
- **GPS Module**: Vehicle location and speed tracking.
- **OBD-II Adapter (Optional)**: Vehicle diagnostic information.
- **Wi-Fi Connectivity**: MQTT communication.

---

## Getting Started

### Prerequisites

- ESP-IDF v5.x
- Node.js and Node-RED
- MQTT Broker (e.g., Mosquitto)

### Installation

1. Clone the repository:
   ```bash
   git clone https://github.com/your-username/Driver-Behaviour-Analysis.git
   cd Driver-Behaviour-Analysis
   ```

2. Configure the firmware:
   - Update the Wi-Fi credentials and MQTT broker settings in `config.h`.

3. Build and flash the firmware:
   ```bash
   idf.py build
   idf.py -p <PORT> flash monitor
   ```

4. Configure Node-RED:
   - Import the `dashboard/flow.json` into Node-RED.

5. Start the MQTT broker:
   ```bash
   mosquitto
   ```

---

## License

This project is licensed under the MIT License.