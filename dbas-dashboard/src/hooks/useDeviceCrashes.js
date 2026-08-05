import usePolling from './usePolling';
import { getDeviceCrashes } from '../services/alertService';

/** Polls the device's confirmed crash events, newest first. */
export default function useDeviceCrashes(deviceId, limit = 10) {
  const { data, ...rest } = usePolling(() => getDeviceCrashes(deviceId, limit), {
    enabled: !!deviceId,
    deps: [deviceId, limit],
  });
  return { data: data || [], ...rest };
}
