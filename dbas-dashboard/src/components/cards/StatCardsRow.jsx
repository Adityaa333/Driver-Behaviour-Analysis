import {
  ShieldCheckIcon,
  BoltIcon,
  Cog6ToothIcon,
  AdjustmentsHorizontalIcon,
  FireIcon,
  MapIcon,
} from '@heroicons/react/24/outline';
import StatCard from '../common/StatCard';
import { roundTo, scoreColor, speedColor } from '../../utils/formatters';

/**
 * Derives the six brief-mandated stat cards from two already-polled
 * sources (latest score, latest telemetry) rather than each card
 * polling its own endpoint - keeps this section to exactly two HTTP
 * requests per tick no matter how many cards are shown.
 *
 * Vehicle Speed prefers the OBD-II reading (direct wheel-derived, see
 * driver_score.c's own preference) and falls back to GPS speed only
 * when obd_speed_valid is false - the same precedence the firmware
 * itself uses. GPS Speed is shown separately, always from the GPS
 * field, since the brief asks for both as distinct cards.
 */
export default function StatCardsRow({ score, telemetry, scoreLoading, telemetryLoading }) {
  const vehicleSpeedKmh = telemetry
    ? telemetry.obd_speed_valid
      ? telemetry.obd_speed_kmh
      : telemetry.gps_fix_valid
      ? telemetry.gps_speed_kmh
      : null
    : null;

  const rpm = telemetry?.engine_rpm_valid ? telemetry.engine_rpm : null;
  const throttle = telemetry?.throttle_position_valid ? telemetry.throttle_position_pct : null;
  const gpsSpeed = telemetry?.gps_fix_valid ? telemetry.gps_speed_kmh : null;

  return (
    <div className="grid grid-cols-2 md:grid-cols-3 xl:grid-cols-6 gap-4">
      <StatCard
        label="Safety Score"
        value={score ? roundTo(score.score, 0) : null}
        unit="/ 100"
        sublabel={score?.rating}
        icon={ShieldCheckIcon}
        tone={score ? scoreColor(score.score) : 'muted'}
        loading={scoreLoading}
      />
      <StatCard
        label="Vehicle Speed"
        value={vehicleSpeedKmh !== null ? roundTo(vehicleSpeedKmh, 0) : null}
        unit="km/h"
        sublabel={telemetry?.obd_speed_valid ? 'OBD-II' : telemetry?.gps_fix_valid ? 'GPS' : null}
        icon={BoltIcon}
        tone={vehicleSpeedKmh !== null ? speedColor(vehicleSpeedKmh) : 'muted'}
        loading={telemetryLoading}
      />
      <StatCard
        label="Engine RPM"
        value={rpm !== null ? rpm : null}
        unit="rpm"
        icon={Cog6ToothIcon}
        tone={rpm !== null ? (rpm > 4000 ? 'warning' : 'accent') : 'muted'}
        loading={telemetryLoading}
      />
      <StatCard
        label="Throttle Position"
        value={throttle !== null ? roundTo(throttle, 0) : null}
        unit="%"
        icon={AdjustmentsHorizontalIcon}
        tone="accent"
        loading={telemetryLoading}
      />
      <StatCard
        label="Engine Temperature"
        value={null}
        icon={FireIcon}
        tone="muted"
        loading={false}
        note="Not published in telemetry"
      />
      <StatCard
        label="GPS Speed"
        value={gpsSpeed !== null ? roundTo(gpsSpeed, 0) : null}
        unit="km/h"
        icon={MapIcon}
        tone={gpsSpeed !== null ? speedColor(gpsSpeed) : 'muted'}
        loading={telemetryLoading}
        note={!telemetry?.gps_fix_valid ? 'No GPS fix' : null}
      />
    </div>
  );
}
