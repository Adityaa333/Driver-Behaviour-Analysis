import { useEffect, useState } from 'react';
import { getDevices } from '../services/deviceService';
import usePolling from './usePolling';

/**
 * Discovers every device the backend has ever seen and keeps a
 * `selectedDeviceId` in state. GET /devices is already ordered
 * most-recently-seen-first (backend/api.py's list_devices), so the
 * first row is "the latest active device."
 *
 * The user can override the selection (device picker in the top nav);
 * once they do, their choice is respected across polls rather than
 * being silently reset back to the newest device on every refresh -
 * but if their previously-selected device disappears from the list, we
 * fall back to auto-selecting again.
 */
export default function useDeviceDiscovery() {
  const [selectedDeviceId, setSelectedDeviceId] = useState(null);
  const [userSelected, setUserSelected] = useState(false);

  const { data: devices, error, loading } = usePolling(() => getDevices(50, 0), {
    intervalMs: 5000,
  });

  useEffect(() => {
    if (!devices || devices.length === 0) return;

    const stillExists = devices.some((d) => d.device_id === selectedDeviceId);
    if (!selectedDeviceId || (!userSelected && !stillExists)) {
      setSelectedDeviceId(devices[0].device_id);
    } else if (userSelected && !stillExists) {
      // Previously-selected device vanished from the fleet; fall back.
      setSelectedDeviceId(devices[0].device_id);
      setUserSelected(false);
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [devices]);

  const selectDevice = (deviceId) => {
    setSelectedDeviceId(deviceId);
    setUserSelected(true);
  };

  return {
    devices: devices || [],
    selectedDeviceId,
    selectDevice,
    loading,
    error,
  };
}
