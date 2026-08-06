import { useEffect, useRef } from 'react';
import { MapContainer, TileLayer, Marker, Popup, useMap, Circle } from 'react-leaflet';
import L from 'leaflet';
import { MapPinIcon } from '@heroicons/react/24/outline';
import ErrorState from '../common/ErrorState';
import { formatTime, roundTo } from '../../utils/formatters';

const DEFAULT_CENTER = [18.535229, 73.811121]; // Falls back to the firmware's default geofence center (app_main.c)
const GEOFENCE_RADIUS_METERS = 1000;

const vehicleIcon = L.divIcon({
  className: '',
  html: `
    <div style="position:relative;width:22px;height:22px;">
      <span style="position:absolute;inset:0;border-radius:9999px;background:#3B82F6;opacity:0.35;" class="pulse-ring"></span>
      <span style="position:absolute;inset:5px;border-radius:9999px;background:#3B82F6;border:2px solid #E2E8F0;box-shadow:0 0 8px rgba(59,130,246,0.8);"></span>
    </div>
  `,
  iconSize: [22, 22],
  iconAnchor: [11, 11],
});

/** Recenters/pans the map whenever the vehicle's position updates. */
function Recenter({ position }) {
  const map = useMap();
  const hasCentered = useRef(false);

  useEffect(() => {
    if (!position) return;
    if (!hasCentered.current) {
      map.setView(position, 15);
      hasCentered.current = true;
    } else {
      map.panTo(position, { animate: true, duration: 0.8 });
    }
  }, [position, map]);

  return null;
}

export default function VehicleMap({ telemetry, loading }) {
  const hasFix = telemetry?.gps_fix_valid;
  const position = hasFix ? [telemetry.latitude_deg, telemetry.longitude_deg] : null;

  return (
    <div className="glass-card rounded-2xl overflow-hidden relative" style={{ height: 384 }}>
      <MapContainer
        center={position || DEFAULT_CENTER}
        zoom={position ? 15 : 12}
        style={{ height: '100%', width: '100%' }}
        zoomControl={true}
        attributionControl={true}
      >
        <TileLayer
          url="https://{s}.basemaps.cartocdn.com/dark_all/{z}/{x}/{y}{r}.png"
          attribution='&copy; <a href="https://carto.com/attributions">CARTO</a> &copy; OpenStreetMap contributors'
        />
        <Circle
          center={DEFAULT_CENTER}
          radius={GEOFENCE_RADIUS_METERS}
          pathOptions={{
            color: '#FBBF24',
            fillColor: '#FDE68A',
            fillOpacity: 0.15,
            dashArray: '4',
          }}
        />
        {position && (
          <>
            <Marker position={position} icon={vehicleIcon}>
              <Popup>
                <div className="text-xs">
                  <p className="font-semibold mb-1">Device {telemetry.device_id}</p>
                  <p>Speed: {roundTo(telemetry.gps_speed_kmh, 1)} km/h</p>
                  <p>Heading: {roundTo(telemetry.heading_deg, 0)}&deg;</p>
                  <p className="text-[var(--color-text-muted)] mt-1">
                    {formatTime(telemetry.timestamp_ms)}
                  </p>
                </div>
              </Popup>
            </Marker>
            <Recenter position={position} />
          </>
        )}
      </MapContainer>

      {!position && (
        <div className="absolute inset-0 flex items-center justify-center bg-[var(--color-bg)]/70 backdrop-blur-sm pointer-events-none">
          <div className="flex flex-col items-center gap-2">
            <MapPinIcon className="w-6 h-6 text-[var(--color-text-muted)]" />
            {loading ? (
              <ErrorState message="Waiting for telemetry…" compact />
            ) : (
              <ErrorState message="No active GPS fix for this device" compact />
            )}
          </div>
        </div>
      )}
    </div>
  );
}
