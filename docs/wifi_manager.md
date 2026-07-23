# WiFi Manager Module

## Purpose
The `wifi_manager` module manages the ESP32 WiFi station connection throughout the device lifecycle. It initializes the networking stack, establishes the initial connection, automatically reconnects after disconnections, and exposes connection status to other modules.

---

## Responsibilities
- Initialize the WiFi subsystem and networking components.
- Connect the device to the configured wireless network.
- Monitor WiFi connection events.
- Perform automatic reconnection using retries and timed backoff.
- Provide connection status and signal strength information.

---

## Public API

### `esp_err_t wifi_manager_init(void)`
Initializes the networking stack, event loop, WiFi driver, and connection parameters before starting the initial connection attempt. The function returns once the connection process has been started.

### `bool wifi_manager_is_connected(void)`
Returns whether the station currently has a valid network connection.

### `esp_err_t wifi_manager_wait_connected(uint32_t timeout_ms)`
Blocks until a WiFi connection is established or the specified timeout expires. Other modules use this function to synchronize network-dependent operations.

### `esp_err_t wifi_manager_get_rssi(int8_t *rssi_dbm)`
Returns the received signal strength of the currently connected access point.

---

## Important Internal Functions

### `wifi_event_handler()`
Processes WiFi and IP events, updates the connection state, manages retry counters, and schedules delayed reconnection attempts when necessary.

### `wifi_reconnect_timer_cb()`
Invoked after the configured backoff period to initiate another WiFi connection attempt.

---

## Data Flow
The application calls `wifi_manager_init()` to configure the WiFi subsystem and begin connecting to the configured access point. Connection events update the internal event group, allowing other modules to wait for connectivity, query the current state, or retrieve signal strength information after a successful connection.

---

## Dependencies
The module depends on the ESP-IDF WiFi, network interface, event loop, timer, and NVS components for network management. It also uses FreeRTOS event groups for connection synchronization, ESP logging utilities, and configuration values defined in `config.h`.

---

## Key Design Decisions
Connection state is maintained through an event group so dependent modules can block efficiently instead of polling. Automatic reconnection uses a combination of immediate retries followed by timer-based backoff, allowing recovery from temporary network outages without requiring a device reboot. The module begins the connection process asynchronously, leaving callers free to decide whether to wait for connectivity.