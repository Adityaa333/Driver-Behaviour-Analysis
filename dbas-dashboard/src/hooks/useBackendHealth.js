import { getBackendHealth } from '../services/statusService';
import usePolling from './usePolling';

/**
 * Polls GET /health for backend + MQTT broker liveness, feeding the
 * top-nav "Backend Status" / "MQTT Status" indicators. Kept as its own
 * hook (rather than folded into device polling) since /health is
 * fleet-wide, not scoped to a single device.
 */
export default function useBackendHealth() {
  const { data, error, loading } = usePolling(getBackendHealth, { intervalMs: 5000 });

  return {
    // /health returns { database: "up"|"down", mqtt: "up"|"down", ... }
    backendUp: !error && data?.database === 'up',
    mqttUp: !error && data?.mqtt === 'up',
    raw: data,
    loading,
    error,
  };
}
