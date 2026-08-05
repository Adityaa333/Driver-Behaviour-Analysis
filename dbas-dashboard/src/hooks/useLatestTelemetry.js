import usePolling from './usePolling';
import { getLatestTelemetry } from '../services/telemetryService';

/** Polls the single most recent fused telemetry sample for a device. */
export default function useLatestTelemetry(deviceId) {
  const { data, ...rest } = usePolling(() => getLatestTelemetry(deviceId, 1), {
    enabled: !!deviceId,
    deps: [deviceId],
  });
  // /telemetry/latest returns an array (newest first); the cards/gauges
  // only ever want the single latest row.
  return { data: Array.isArray(data) && data.length > 0 ? data[0] : null, ...rest };
}
