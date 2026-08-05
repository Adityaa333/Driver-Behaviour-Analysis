import usePolling from './usePolling';
import { getDeviceAlerts } from '../services/alertService';

/** Polls the device's recent alerts (idling / geofence), newest first. */
export default function useDeviceAlerts(deviceId, limit = 20) {
  const { data, ...rest } = usePolling(() => getDeviceAlerts(deviceId, limit), {
    enabled: !!deviceId,
    deps: [deviceId, limit],
  });
  return { data: data || [], ...rest };
}
