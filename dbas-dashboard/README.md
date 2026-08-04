# DBAS Dashboard — Phase 1 (Foundation)

Frontend for the Driver Behaviour Analysis System. Consumes the FastAPI
backend's REST API exclusively (no Node-RED).

## What's in this pass

Per your request to build incrementally, this first delivery covers:

1. **Project setup** — Vite + React 19, Tailwind CSS v4, Axios, Recharts,
   React Leaflet, Framer Motion, Heroicons, React Router.
2. **Folder structure** — see below.
3. **API service layer** — one Axios client + one module per resource.
4. **Routing & layout** — top nav (title, MQTT/Backend status, device
   picker, live clock) + a dashboard page with the full section grid
   laid out (stat cards, gauges, charts, map, alerts, crash panel,
   system status), currently populated with placeholder panels ready to
   be swapped for the real widgets in the next pass.

The app runs, builds, and polls your backend end-to-end right now:
device discovery, health checks, and the top nav are fully live. The
content panels are labeled placeholders until the next incremental
step.

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
    cards/      (next pass) stat cards
    gauges/     (next pass) score/speed gauges
    charts/     (next pass) trend charts
    map/        (next pass) live GPS map
    alerts/     (next pass) alerts table
    crash/      (next pass) crash status panel
    status/     (next pass) system status panel
    common/     StatusDot, LoadingSpinner, ErrorState, SectionHeader, ComingSoonPanel
  pages/        DashboardPage
  hooks/        usePolling, useDeviceDiscovery, useBackendHealth
  services/     apiClient, deviceService, scoreService, telemetryService,
                alertService, statusService
  context/      DeviceContext (shared selected-device state)
  utils/        formatters.js
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

## Next steps (subsequent passes)

- Stat cards (Safety Score, Speed, RPM, Throttle, Engine Temp, GPS Speed)
- Driver Score gauge + Vehicle Speed gauge (Recharts radial)
- Trend charts: score history, speed history, RPM history, throttle history
- Live GPS map (React Leaflet) with animated vehicle marker
- Alerts table (severity, timestamp, description)
- Crash detection panel (SAFE / CRASH DETECTED, animated on crash)
- System status panel (WiFi, MQTT, uptime, heap, firmware version, RSSI)
