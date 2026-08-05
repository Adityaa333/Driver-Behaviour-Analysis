import usePolling from './usePolling';
import { getDeviceStatus } from '../services/statusService';

/** Polls the latest device health/connectivity snapshot. */
export default function useDeviceStatus(deviceId) {
  return usePolling(() => getDeviceStatus(deviceId), {
    enabled: !!deviceId,
    deps: [deviceId],
  });
}
