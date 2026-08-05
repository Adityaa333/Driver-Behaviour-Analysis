import { useCallback, useEffect, useRef, useState } from 'react';

const DEFAULT_INTERVAL_MS = Number(import.meta.env.VITE_POLL_INTERVAL_MS) || 1000;

/**
 * Repeatedly calls an async fetcher on a fixed interval and exposes its
 * latest result, loading, and error state. This is the single primitive
 * every data-bound card/chart/gauge in the dashboard is built on, so
 * polling cadence, in-flight overlap protection, and graceful-failure
 * behavior only need to be right in one place.
 *
 * - Overlap-safe: if a request is still pending when the next tick
 *   fires, that tick is skipped rather than piling up requests.
 * - Failure is graceful: a failed poll keeps the last good `data`
 *   in place and surfaces `error` alongside it, rather than clearing
 *   the UI to a blank/error state on a single dropped request.
 * - `enabled=false` pauses polling entirely (e.g. while no device is
 *   selected yet).
 *
 * @param {() => Promise<any>} fetcher
 * @param {{ intervalMs?: number, enabled?: boolean, deps?: any[] }} options
 */
export default function usePolling(fetcher, { intervalMs = DEFAULT_INTERVAL_MS, enabled = true, deps = [] } = {}) {
  const [data, setData] = useState(null);
  const [error, setError] = useState(null);
  const [loading, setLoading] = useState(true);
  const [lastUpdated, setLastUpdated] = useState(null);

  const fetcherRef = useRef(fetcher);
  fetcherRef.current = fetcher;

  const inFlightRef = useRef(false);

  const tick = useCallback(async () => {
    if (inFlightRef.current) return;
    inFlightRef.current = true;
    try {
      const result = await fetcherRef.current();
      setData(result);
      setError(null);
      setLastUpdated(Date.now());
    } catch (err) {
      setError(err);
    } finally {
      inFlightRef.current = false;
      setLoading(false);
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  useEffect(() => {
    if (!enabled) {
      setLoading(false);
      return undefined;
    }

    setLoading(true);
    tick();
    const id = setInterval(tick, intervalMs);
    return () => clearInterval(id);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [enabled, intervalMs, tick, ...deps]);

  return { data, error, loading, lastUpdated, refetch: tick };
}
