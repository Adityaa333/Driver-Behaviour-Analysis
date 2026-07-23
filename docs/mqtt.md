# MQTT Client Module

## Purpose
The `mqtt_client` module manages the MQTT connection and provides a simplified publishing interface for the rest of the firmware. Other modules publish messages through dedicated API functions without interacting directly with the underlying ESP-IDF MQTT client.

---

## Responsibilities
- Generate a unique device identifier from the ESP32 MAC address.
- Create and manage the internal publish queue.
- Start and own the background MQTT client task.
- Monitor broker connection status.
- Publish telemetry, alerts, scores, crash events, and status messages.

---

## Public API

### `esp_err_t mqtt_client_init(void)`
Initializes the module by generating the device ID, creating synchronization objects, creating the publish queue, and starting the background MQTT task. The background task waits for WiFi before connecting to the broker.

### `bool mqtt_client_is_connected(void)`
Returns the current MQTT connection status based on the internal event group.

### `const char *mqtt_client_get_device_id(void)`
Returns the internally generated device identifier derived from the ESP32 factory MAC address.

### `mqtt_client_publish_telemetry()`
Enqueues a telemetry JSON payload for publication using the telemetry topic and QoS configuration.

### `mqtt_client_publish_score()`
Enqueues a driver score payload using the score topic.

### `mqtt_client_publish_alert()`
Enqueues a non-crash safety event for publication.

### `mqtt_client_publish_crash()`
Enqueues a crash event using the highest-priority MQTT configuration.

### `mqtt_client_publish_status()`
Enqueues periodic device status information for publication.

---

## Important Internal Functions

### `mqtt_generate_device_id()`
Reads the ESP32 factory MAC address and converts it into a hexadecimal device identifier used throughout the module.

### `mqtt_event_handler()`
Processes MQTT connection events and updates the internal connection state maintained by the event group.

### `mqtt_publish_task()`
Owns the ESP-IDF MQTT client instance, waits for WiFi connectivity, establishes the broker connection, and continuously publishes queued messages.

### `mqtt_enqueue()`
Builds the MQTT topic, copies the JSON payload into the publish queue, and validates message size before enqueueing.

---

## Data Flow
Application modules call one of the public publish functions with a JSON payload. The payload is formatted into a publish message and placed in the internal FreeRTOS queue. The background publish task removes queued messages and forwards them to the ESP-IDF MQTT client for transmission to the broker.

---

## Dependencies
The module depends on the ESP-IDF MQTT library, FreeRTOS queues, tasks, and event groups for asynchronous communication. It also relies on `wifi_manager` to wait for network connectivity, `esp_mac` to obtain the device identifier, and configuration constants defined in `config.h`.

---

## Key Design Decisions
Only the background publish task owns the MQTT client handle, preventing concurrent access from multiple tasks. All public publish functions perform queue-based communication instead of direct network operations, preventing application tasks from blocking on MQTT activity. Messages are dropped when the publish queue is full rather than blocking producers indefinitely.