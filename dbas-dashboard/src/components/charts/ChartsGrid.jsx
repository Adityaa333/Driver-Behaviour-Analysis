import { useMemo } from 'react';
import TrendChart from './TrendChart';

function Panel({ title, subtitle, children }) {
  return (
    <div className="glass-card rounded-2xl p-4">
      <div className="mb-2">
        <h3 className="text-xs font-semibold text-[var(--color-text-primary)]">{title}</h3>
        {subtitle && <p className="text-[10px] text-[var(--color-text-muted)]">{subtitle}</p>}
      </div>
      {children}
    </div>
  );
}

export default function ChartsGrid({ scoreHistory, telemetryHistory, scoreLoading, telemetryLoading }) {
  const speedSeries = useMemo(
    () =>
      (telemetryHistory || []).map((t) => ({
        timestamp_ms: t.timestamp_ms,
        speed_kmh: t.obd_speed_valid ? t.obd_speed_kmh : t.gps_fix_valid ? t.gps_speed_kmh : null,
      })),
    [telemetryHistory]
  );

  const rpmSeries = useMemo(
    () =>
      (telemetryHistory || []).map((t) => ({
        timestamp_ms: t.timestamp_ms,
        rpm: t.engine_rpm_valid ? t.engine_rpm : null,
      })),
    [telemetryHistory]
  );

  const throttleSeries = useMemo(
    () =>
      (telemetryHistory || []).map((t) => ({
        timestamp_ms: t.timestamp_ms,
        throttle_pct: t.throttle_position_valid ? t.throttle_position_pct : null,
      })),
    [telemetryHistory]
  );

  return (
    <div className="grid grid-cols-1 lg:grid-cols-2 gap-4">
      <Panel title="Driver Score History" subtitle="Cumulative-trip safety score">
        <TrendChart
          data={scoreHistory}
          xKey="timestamp_ms"
          yKey="score"
          unit="pts"
          tone="accent"
          loading={scoreLoading}
          yDomain={[0, 100]}
        />
      </Panel>
      <Panel title="Vehicle Speed History" subtitle="OBD-II preferred, GPS fallback">
        <TrendChart
          data={speedSeries}
          xKey="timestamp_ms"
          yKey="speed_kmh"
          unit="km/h"
          tone="success"
          loading={telemetryLoading}
        />
      </Panel>
      <Panel title="RPM History" subtitle="Engine speed">
        <TrendChart
          data={rpmSeries}
          xKey="timestamp_ms"
          yKey="rpm"
          unit="rpm"
          tone="warning"
          loading={telemetryLoading}
        />
      </Panel>
      <Panel title="Throttle History" subtitle="Throttle position">
        <TrendChart
          data={throttleSeries}
          xKey="timestamp_ms"
          yKey="throttle_pct"
          unit="%"
          tone="danger"
          loading={telemetryLoading}
          yDomain={[0, 100]}
        />
      </Panel>
    </div>
  );
}
