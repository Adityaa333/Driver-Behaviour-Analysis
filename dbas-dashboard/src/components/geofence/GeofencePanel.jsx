import { ShieldCheckIcon, ExclamationTriangleIcon } from '@heroicons/react/24/solid';
import { formatDateTime } from '../../utils/formatters';

const GEOFENCE_ZONE = {
  name: 'CDAC ACTS',
  radius_m: 1000,
};

export default function GeofencePanel({ telemetry }) {
  const hasFix = telemetry?.gps_fix_valid;
  const insideFence = hasFix
    ? getDistanceToZone(telemetry.latitude_deg, telemetry.longitude_deg) <= GEOFENCE_ZONE.radius_m
    : null;
  const isViolation = insideFence === false;

  return (
    <div className="glass-card rounded-2xl h-48 flex flex-col items-center justify-center gap-4 px-6 relative overflow-hidden">
      {hasFix ? (
        isViolation ? (
          <div className="flex flex-col items-center gap-3 text-center">
            <ExclamationTriangleIcon className="w-12 h-12 text-[var(--color-danger)]" />
            <div>
              <p className="font-display text-lg font-bold text-[var(--color-danger)] tracking-wide">
                FENCE CROSSED
              </p>
              <p className="text-xs text-[var(--color-text-muted)] mt-2">
                {formatDateTime(telemetry.timestamp_ms)}
              </p>
              <p className="text-xs text-[var(--color-text-secondary)] mt-1">
                Leaving {GEOFENCE_ZONE.name} ({GEOFENCE_ZONE.radius_m}m radius)
              </p>
            </div>
          </div>
        ) : (
          <div className="flex flex-col items-center gap-3 text-center">
            <ShieldCheckIcon className="w-12 h-12 text-[var(--color-success)]" />
            <div>
              <p className="font-display text-lg font-bold text-[var(--color-success)] tracking-wide">
                SAFE
              </p>
              <p className="text-xs text-[var(--color-text-muted)] mt-2">
                {formatDateTime(telemetry.timestamp_ms)}
              </p>
              <p className="text-xs text-[var(--color-text-secondary)] mt-1">
                Inside {GEOFENCE_ZONE.name}
              </p>
            </div>
          </div>
        )
      ) : (
        <div className="flex flex-col items-center gap-3 text-center">
          <ShieldCheckIcon className="w-12 h-12 text-[var(--color-warning)]" />
          <div>
            <p className="font-display text-lg font-bold text-[var(--color-warning)] tracking-wide">
              UNKNOWN
            </p>
            <p className="text-xs text-[var(--color-text-muted)] mt-2">
              GPS fix required to determine geofence state
            </p>
            <p className="text-xs text-[var(--color-text-secondary)] mt-1">
              {GEOFENCE_ZONE.name} | {GEOFENCE_ZONE.radius_m}m radius
            </p>
          </div>
        </div>
      )}
    </div>
  );
}

function getDistanceToZone(lat, lon) {
  const centerLat = 18.535229;
  const centerLon = 73.811121;

  const toRad = (deg) => (deg * Math.PI) / 180;
  const dLat = toRad(lat - centerLat);
  const dLon = toRad(lon - centerLon);
  const lat1 = toRad(centerLat);
  const lat2 = toRad(lat);

  const a =
    Math.sin(dLat / 2) * Math.sin(dLat / 2) +
    Math.cos(lat1) * Math.cos(lat2) * Math.sin(dLon / 2) * Math.sin(dLon / 2);
  const c = 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a));
  const earthRadius = 6371000;
  return earthRadius * c;
}
