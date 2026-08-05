# DBAS Dashboard

Frontend for the Driver Behaviour Analysis System. Consumes the FastAPI
backend's REST API exclusively (no Node-RED).

## Status: Phase 2 — fully wired

Every section from the brief is now implemented with real data, not
placeholders:

- **Live Snapshot** — 6 stat cards (Safety Score, Vehicle Speed, Engine
  RPM, Throttle Position, Engine Temperature, GPS Speed), driven by two
  polled endpoints (`/score`, `/telemetry/latest`).
- **Instrument Cluster** — hand-built SVG automotive gauges (270°
  sweep, colored zones, animated needle) for Driver Score and Vehicle
  Speed.
- **Trends** — 4 area charts (score, speed, RPM, throttle) built on
  Recharts, dark-themed, fed by `/score/history` and `/telemetry`.
- **Vehicle Location** — React Leaflet dark map with a pulsing vehicle
  marker, auto-pan on position updates, popup with speed/heading. Code-
  split into its own lazy-loaded chunk since Leaflet is heavy and isn't
  needed for first paint.
- **Crash Detection** — SAFE / CRASH DETECTED panel driven by
  `/crashes`; flashes and pulses for 60s after a crash timestamp (see
  the comment in `CrashPanel.jsx` for why 60s was chosen relative to the
  firmware's own 10s cooldown).
- **Recent Alerts** — table using the backend's own `severity`/`message`
  fields directly (no re-deriving alert text on the frontend).
- **System Status** — WiFi/MQTT connectivity, uptime, heap, firmware
  version (pulled from the device list, since `/status` doesn't carry
  it), RSSI with a signal-quality label.

Every widget is backed by one of the resource hooks in `src/hooks/`,
each polling independently at 1s (configurable via `.env`) and each
tolerant of missing data (no fix yet, no score published yet, etc.)
without breaking the layout.

## Getting started

```bash
npm install
cp .env.example .env   # then edit VITE_API_BASE_URL if your backend isn't on localhost:8000
npm run dev
```

Build for production:

```bash
npm run build
```


## Folder structure

```
src/
  components/
    layout/     TopNav, DashboardLayout
    cards/      StatCardsRow (6 live-snapshot cards)
    gauges/     RadialGauge (generic SVG gauge), GaugesRow
    charts/     TrendChart (generic Recharts area chart), ChartsGrid
    map/        VehicleMap (React Leaflet, lazy-loaded)
    alerts/     AlertsTable
    crash/      CrashPanel
    status/     SystemStatusPanel
    common/     StatusDot, StatCard, LoadingSpinner, ErrorState, SectionHeader
  pages/        DashboardPage
  hooks/        usePolling (base primitive), useDeviceDiscovery, useBackendHealth,
                useLatestScore, useLatestTelemetry, useScoreHistory,
                useTelemetryHistory, useDeviceStatus, useDeviceAlerts, useDeviceCrashes
  services/     apiClient, deviceService, scoreService, telemetryService,
                alertService, statusService
  context/      DeviceContext (shared selected-device state)
  utils/        formatters.js, gaugeMath.js
  styles/       index.css (Tailwind v4 theme tokens, glassmorphism, dark palette)
```

## API notes / one discrepancy worth flagging

Your brief listed `GET /devices/{deviceId}/telemetry/history`, but
`backend/api.py` doesn't expose a `/history` suffix for telemetry — the
paginated historical route is just `GET /devices/{deviceId}/telemetry`
(newest first), alongside `GET /devices/{deviceId}/telemetry/latest`.
`telemetryService.getTelemetryHistory()` is named to match your brief's
intent but calls the real `/telemetry` route so the charts will
actually get data. If a `/telemetry/history` route gets added later,
that's a one-line change in `src/services/telemetryService.js`.

Also: `/health` lives outside the `/api/v1` prefix (see `backend/app.py`),
so `statusService.getBackendHealth()` strips `/api/v1` off your
`VITE_API_BASE_URL` before calling it. If you deploy the API behind a
different reverse-proxy path, double check that stripping logic still
lands on the right URL.

## Design tokens

Dark theme colors are fixed exactly as specified: background `#0F172A`,
cards `#1E293B`, accent `#3B82F6`, success `#22C55E`, warning `#F59E0B`,
danger `#EF4444`. Typography: **Space Grotesk** for headings (angular,
instrument-cluster character), **Inter** for body/UI text, **JetBrains
Mono** for live telemetry numerals (tabular figures so fast-updating
numbers don't jitter in width).

## Known data gaps (not frontend bugs)

- **Engine Temperature** — the brief asks for this card, but
  `task_manager_telemetry_task()` in the firmware never publishes
  coolant temperature in the telemetry MQTT payload (only accel/gyro/
  GPS/RPM/OBD-speed/throttle), even though `obd.c` reads it. The
  `TelemetryOut` schema in `backend/api.py` doesn't carry it either.
  The card renders honestly as "—" with a "Not published in telemetry"
  note rather than showing a fabricated number. Wiring this up for real
  needs a firmware + backend schema change first.
- **Vehicle Speed** prefers OBD-II (`obd_speed_kmh`) and falls back to
  GPS speed only when `obd_speed_valid` is false - matching the
  precedence `driver_score.c` itself uses for overspeed detection.

## Next steps / open to adjust

Everything in the brief is implemented. Natural follow-ups if useful:
- Wire up a device-vs-device comparison view (multi-device fleet page)
- Add a settings panel for poll interval / map tile provider
- Add unit tests around the formatters and the crash-active-window logic

