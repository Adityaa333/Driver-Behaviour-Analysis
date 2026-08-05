import apiClient from './apiClient';

/**
 * GET /devices
 * Returns devices most-recently-seen first (per backend/api.py), which
 * is exactly the ordering useDeviceDiscovery relies on to auto-select
 * "the latest active device."
 */
export const getDevices = async (limit = 50, offset = 0) => {
  const { data } = await apiClient.get('/devices', { params: { limit, offset } });
  return data;
};

/** GET /devices/{deviceId} */
export const getDevice = async (deviceId) => {
  const { data } = await apiClient.get(`/devices/${deviceId}`);
  return data;
};

export default { getDevices, getDevice };
