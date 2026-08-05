import apiClient from './apiClient';

/**
 * GET /devices/{deviceId}/telemetry/latest
 * Returns the most recent `limit` telemetry rows, newest first - used
 * for the live gauges/cards.
 */
export const getLatestTelemetry = async (deviceId, limit = 1) => {
  const { data } = await apiClient.get(`/devices/${deviceId}/telemetry/latest`, {
    params: { limit },
  });
  return data;
};

/**
 * GET /devices/{deviceId}/telemetry
 *
 * NOTE: backend/api.py exposes historical telemetry at `/telemetry`
 * (paginated, most-recent-first), not `/telemetry/history` - there is
 * no separate `/history` suffix route in the deployed backend. This
 * wrapper is named getTelemetryHistory to match how the dashboard uses
 * it (feeding the speed/RPM/throttle trend charts), but it calls the
 * real route so it actually returns data against the current API.
 */
export const getTelemetryHistory = async (deviceId, limit = 50) => {
  const { data } = await apiClient.get(`/devices/${deviceId}/telemetry`, {
    params: { limit },
  });
  return data;
};

export default { getLatestTelemetry, getTelemetryHistory };
