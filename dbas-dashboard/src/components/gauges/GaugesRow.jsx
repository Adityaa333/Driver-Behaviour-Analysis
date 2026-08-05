import RadialGauge from './RadialGauge';
import { roundTo } from '../../utils/formatters';

const SCORE_ZONES = [
  { upTo: 50, tone: 'danger' },
  { upTo: 80, tone: 'warning' },
  { upTo: 100, tone: 'success' },
];

// Mirrors dashboard/flow.json's speed gauge segmentation (seg1=80, seg2=120, max=180).
const SPEED_ZONES = [
  { upTo: 80, tone: 'success' },
  { upTo: 120, tone: 'warning' },
  { upTo: 180, tone: 'danger' },
];

export default function GaugesRow({ score, telemetry, scoreLoading, telemetryLoading }) {
  const speedValue = telemetry
    ? telemetry.obd_speed_valid
      ? telemetry.obd_speed_kmh
      : telemetry.gps_fix_valid
      ? telemetry.gps_speed_kmh
      : null
    : null;

  return (
    <div className="grid grid-cols-1 md:grid-cols-2 gap-4">
      <div className="glass-card rounded-2xl py-6 flex flex-col items-center justify-center">
        <RadialGauge
          value={score ? roundTo(score.score, 0) : null}
          min={0}
          max={100}
          unit="SAFETY SCORE"
          label={score?.rating ? `Rating: ${score.rating}` : 'Awaiting score data'}
          zones={SCORE_ZONES}
          loading={scoreLoading}
        />
      </div>
      <div className="glass-card rounded-2xl py-6 flex flex-col items-center justify-center">
        <RadialGauge
          value={speedValue !== null ? roundTo(speedValue, 0) : null}
          min={0}
          max={180}
          unit="KM/H"
          label={
            telemetry
              ? telemetry.obd_speed_valid
                ? 'Source: OBD-II'
                : telemetry.gps_fix_valid
                ? 'Source: GPS'
                : 'No speed source available'
              : 'Awaiting telemetry'
          }
          zones={SPEED_ZONES}
          loading={telemetryLoading}
        />
      </div>
    </div>
  );
}
