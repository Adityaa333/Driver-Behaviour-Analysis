import apiClient, { API_BASE_URL } from './apiClient';
import axios from 'axios';

/** GET /devices/{deviceId}/status - latest health/connectivity snapshot */
export const getDeviceStatus = async (deviceId) => {
  const { data } = await apiClient.get(`/devices/${deviceId}/status`);
  return data;
};

/**
 * GET /health - backend + broker liveness probe. This lives outside
 * /api/v1 (see backend/app.py), so it's called against the API's
 * origin rather than through the versioned apiClient baseURL.
 */
export const getBackendHealth = async () => {
  const origin = API_BASE_URL.replace(/\/api\/v1\/?$/, '');
  const { data } = await axios.get(`${origin}/health`, { timeout: 5000 });
  return data;
};

export default { getDeviceStatus, getBackendHealth };
