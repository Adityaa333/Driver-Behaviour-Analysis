# OBD Module — Plain-English Overview

## What it's for

This module is the thing in your firmware that talks to the car and asks
"how fast are you going, what's your engine RPM, how open is the throttle,
and how hot is the coolant?" It hides all the messy communication details
behind two simple functions so the rest of the firmware never has to
think about Bluetooth, ELM327 commands, or hex parsing.

## The two things anything else needs to know

```c
esp_err_t obd_init(void);
esp_err_t obd_read(obd_data_t *data);
```

- **`obd_init()`** — call once at startup. Gets the Bluetooth radio going
  and starts trying to connect to your OBD-II dongle in the background.
- **`obd_read(&data)`** — call whenever you want a fresh reading. It asks
  the car for 4 things and fills in `data` with whatever it got back.

Everything else in this file is internal plumbing to make those two
functions work reliably over a wireless link that can drop at any time.

## What data comes back

`obd_data_t` holds:

| Field | What it is |
|---|---|
| `engine_rpm` | Engine speed |
| `vehicle_speed_kmh` | Speed from the car's own wheel sensors |
| `throttle_position_pct` | How far the gas pedal is pressed (0–100%) |
| `coolant_temp_c` | Engine coolant temperature |

**Each field has its own `_valid` flag.** This is the most important
design idea in the whole module: if the RPM request times out but speed
comes back fine, you still get a usable speed reading instead of the
whole read failing. One flaky question to the car doesn't ruin the
answers to the other three.

## How it actually talks to the car (the "ELM327" part)

You're not wiring directly into the car's CAN bus. Instead there's a
**Bluetooth dongle** (an "ELM327") plugged into the car's OBD-II port that
does the CAN-bus talking for you, and your ESP32 just chats with that
dongle over Bluetooth like it's a walkie-talkie.

The "conversation" looks like plain text:

- You send something like `"010C\r"` (translation: *"Mode 01, give me
  PID 0x0C, which is engine RPM"*)
- The dongle sends back something like `"41 0C 1A F8"` — a string of hex
  numbers ending in a `>` character that means "I'm done talking, your
  turn."

So under the hood, `obd_read()` is really just:
1. Send a short text command for each of the 4 things you want to know
2. Wait for the dongle's `>`-terminated reply
3. Turn the hex numbers in that reply into the actual RPM/speed/%/°C values

## The moving parts inside the file

| Piece | Plain-English job |
|---|---|
| `obd_bt_connection_task()` | Runs forever in the background. Keeps trying to connect (and reconnect) to the dongle over Bluetooth, and never gives up — a dropped Bluetooth link recovers on its own, no reboot needed. |
| `obd_elm327_configure_locked()` | Right after connecting, "resets" the dongle and tells it a few housekeeping settings (turn off command echo, pick the right protocol automatically). Runs once per connection. |
| `obd_request_pid_locked()` | Sends one text command (e.g. "give me RPM") and gets back the raw hex bytes for it. |
| `obd_parse_pid_response()` | Takes the dongle's messy text reply and pulls out the actual numbers, ignoring junk like "SEARCHING..." messages some dongles print. |
| `obd_spp_callback()` | The low-level "a Bluetooth event happened" handler — connected, disconnected, or "here's some data" — that feeds everything else. |
| `s_bus_mutex` | A lock ensuring only one question is ever "in flight" to the dongle at a time, since it can only handle one conversation at once. |

## What happens when things go wrong

- **Dongle not connected / ignition off / no response at all:**
  `obd_read()` returns a timeout error and all four `_valid` flags stay
  false. Nothing crashes; the rest of the firmware (like idling
  detection) just treats OBD data as unavailable until it comes back.
- **Bluetooth link drops mid-trip:** the background connection task
  notices and keeps retrying with a short delay between attempts,
  indefinitely, without needing anyone to reboot the device.
- **One PID (say, coolant temp) times out but the others work:** you
  still get valid RPM/speed/throttle back — only `coolant_temp_valid`
  comes back false.

## What you'd need to set up for your specific dongle

In `config.h`:

- `OBD_BT_TARGET_MAC` — the Bluetooth address of your specific dongle (ELM327) 
- `OBD_BT_SPP_SCN` — which "channel" to talk to it on (often `1`, but
  not guaranteed — depends on the dongle)
- `OBD_REQUEST_TIMEOUT_MS` — how long to wait for a reply before giving
  up on one question (Bluetooth is slower than a wired connection, so
  this needs to be more generous than you'd use for real CAN wiring)

## One-sentence summary

**`obd.c`/`obd.h` is a translator: it turns "give me the car's vitals"
into a short text conversation with a Bluetooth dongle, and hands back a
simple struct of numbers — while quietly handling reconnects, timeouts,
and partial failures so nothing else in the firmware has to.**
