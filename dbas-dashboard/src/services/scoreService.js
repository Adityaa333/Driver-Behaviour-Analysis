import apiClient from './apiClient';

/** GET /devices/{deviceId}/score - most recent safety score snapshot */
export const getLatestScore = async (deviceId) => {
  const { data } = await apiClient.get(`/devices/${deviceId}/score`);
  return data;
};

/** GET /devices/{deviceId}/score/history - most recent first */
export const getScoreHistory = async (deviceId, limit = 30) => {
  const { data } = await apiClient.get(`/devices/${deviceId}/score/history`, {
    params: { limit },
  });
  return data;
};

export default { getLatestScore, getScoreHistory };
