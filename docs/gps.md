# GPS Module

## Overview

The `gps` module is a UART-based NMEA-0183 GPS driver responsible for continuously receiving, validating, parsing, and storing GPS information. It runs its own FreeRTOS task that reads incoming NMEA sentences, extracts useful navigation data, and maintains the latest GPS snapshot for other modules.

---

## Responsibilities

- Configure the GPS UART.
- Continuously receive NMEA data.
- Validate NMEA checksums.
- Parse RMC and GGA sentences.
- Merge parsed information into a single GPS snapshot.
- Provide thread-safe access to the latest GPS data.

---

## Public API

### `gps_init()`

Initializes the GPS driver by:

- Creating the data mutex.
- Configuring the UART peripheral.
- Installing the UART driver.
- Starting the GPS UART task.

---

### `gps_get_latest()`

Returns a thread-safe copy of the latest GPS snapshot.

Returns:

- Position
- Speed
- Heading
- Altitude
- Satellite count
- Fix status
- Timestamp

---

### `gps_get_fix_age_ms()`

Returns the time elapsed since the last valid GPS fix.

Returns `-1` if no valid fix has ever been received.

---

## Internal Functions

### `nmea_coord_to_decimal()`

Converts NMEA latitude/longitude coordinates into decimal degrees.

---

### `nmea_checksum_valid()`

Validates the checksum of every received NMEA sentence before parsing.

Invalid or corrupted sentences are discarded.

---

### `gps_parse_rmc()`

Parses RMC sentences and updates:

- Latitude
- Longitude
- Speed
- Heading
- Fix validity
- Timestamp

Also records the time of the most recent valid GPS fix.

---

### `gps_parse_gga()`

Parses GGA sentences and updates:

- Altitude
- Number of satellites

These values are stored temporarily until merged with the next RMC update.

---

### `gps_parse_sentence()`

Determines the sentence type after checksum validation and dispatches it to the appropriate parser.

Supported:

- RMC
- GGA

Other sentence types are ignored.

---

### `gps_uart_task()`

Runs continuously and:

- Reads bytes from UART.
- Reconstructs complete NMEA sentences.
- Detects corrupted or oversized packets.
- Passes valid sentences to the parser.

---

## Data Stored

The module maintains a thread-safe `gps_data_t` containing:

- Latitude
- Longitude
- Speed
- Heading
- Altitude
- Satellite count
- GPS fix validity
- Timestamp

---

## Design Decisions

- Runs its own dedicated UART task instead of being periodically polled.
- Parses only RMC and GGA sentences since they provide all required information.
- Validates every sentence using the NMEA checksum before parsing.
- Merges information from multiple sentence types into a single GPS snapshot.
- Protects shared GPS data with a mutex for safe concurrent access.
- Tracks the age of the last valid GPS fix so other modules can determine whether location data is stale.