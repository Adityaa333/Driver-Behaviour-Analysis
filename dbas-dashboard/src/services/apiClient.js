import axios from 'axios';

const API_BASE_URL = import.meta.env.VITE_API_BASE_URL || 'http://localhost:8000/api/v1';

/**
 * Shared Axios instance for every DBAS backend call. Centralizing the
 * base URL and timeout here means every service module (devices,
 * telemetry, scores, alerts...) stays a thin wrapper around this one
 * configured client rather than repeating config everywhere.
 */
const apiClient = axios.create({
  baseURL: API_BASE_URL,
  timeout: 8000,
  headers: {
    Accept: 'application/json',
  },
});

// Normalizes Axios/network errors into a small, predictable shape so UI
// code can branch on `error.isNetworkError` / `error.status` without
// digging into Axios internals in every component.
apiClient.interceptors.response.use(
  (response) => response,
  (error) => {
    const normalized = {
      message: error.message || 'Unknown error',
      status: error.response?.status ?? null,
      isNetworkError: !error.response,
      original: error,
    };
    return Promise.reject(normalized);
  }
);

export default apiClient;
export { API_BASE_URL };
