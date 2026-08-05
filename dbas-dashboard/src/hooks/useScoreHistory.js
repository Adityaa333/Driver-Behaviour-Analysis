import usePolling from './usePolling';
import { getScoreHistory } from '../services/scoreService';

/** Polls score history (newest first) and returns it chronological (oldest first) for charting. */
export default function useScoreHistory(deviceId, limit = 30) {
  const { data, ...rest } = usePolling(() => getScoreHistory(deviceId, limit), {
    enabled: !!deviceId,
    deps: [deviceId, limit],
  });
  const chronological = Array.isArray(data) ? [...data].reverse() : [];
  return { data: chronological, ...rest };
}
