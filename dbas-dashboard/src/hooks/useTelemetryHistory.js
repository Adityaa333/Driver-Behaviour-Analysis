import usePolling from './usePolling';
import { getTelemetryHistory } from '../services/telemetryService';

/** Polls telemetry history (newest first) and returns it chronological (oldest first) for charting. */
export default function useTelemetryHistory(deviceId, limit = 50) {
  const { data, ...rest } = usePolling(() => getTelemetryHistory(deviceId, limit), {
    enabled: !!deviceId,
    deps: [deviceId, limit],
  });
  const chronological = Array.isArray(data) ? [...data].reverse() : [];
  return { data: chronological, ...rest };
}
