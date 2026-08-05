import apiClient from './apiClient';

/** GET /devices/{deviceId}/alerts - idling/geofence alerts, newest first */
export const getDeviceAlerts = async (deviceId, limit = 20) => {
  const { data } = await apiClient.get(`/devices/${deviceId}/alerts`, {
    params: { limit },
  });
  return data;
};

/** GET /devices/{deviceId}/crashes - confirmed crash events, newest first */
export const getDeviceCrashes = async (deviceId, limit = 10) => {
  const { data } = await apiClient.get(`/devices/${deviceId}/crashes`, {
    params: { limit },
  });
  return data;
};

export default { getDeviceAlerts, getDeviceCrashes };
