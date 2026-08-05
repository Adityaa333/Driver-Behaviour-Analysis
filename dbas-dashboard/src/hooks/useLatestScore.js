import usePolling from './usePolling';
import { getLatestScore } from '../services/scoreService';

/** Polls the most recent safety score snapshot for a device. */
export default function useLatestScore(deviceId) {
  return usePolling(() => getLatestScore(deviceId), {
    enabled: !!deviceId,
    deps: [deviceId],
  });
}
