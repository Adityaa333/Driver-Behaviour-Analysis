import { useEffect, useRef, useState } from 'react';

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

  // We keep a ref to track the active request's device/dependency context
  const activeEffectRef = useRef(0);

  useEffect(() => {
    if (!enabled) {
      setLoading(false);
      return undefined;
    }

    activeEffectRef.current += 1;
    const currentEffectId = activeEffectRef.current;

    // Reset data and set loading on dependency change to prevent showing stale device data
    setData(null);
    setLoading(true);

    let inFlight = false;

    const tick = async () => {
      if (inFlight) return;
      inFlight = true;
      try {
        const result = await fetcherRef.current();
        if (activeEffectRef.current === currentEffectId) {
          setData(result);
          setError(null);
          setLastUpdated(Date.now());
        }
      } catch (err) {
        if (activeEffectRef.current === currentEffectId) {
          setError(err);
        }
      } finally {
        inFlight = false;
        if (activeEffectRef.current === currentEffectId) {
          setLoading(false);
        }
      }
    };

    // Run immediately
    tick();

    // Setup interval
    const intervalId = setInterval(tick, intervalMs);

    return () => {
      clearInterval(intervalId);
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [enabled, intervalMs, ...deps]);

  // We expose a manual refetch function
  const refetch = async () => {
    try {
      setLoading(true);
      const result = await fetcherRef.current();
      setData(result);
      setError(null);
      setLastUpdated(Date.now());
    } catch (err) {
      setError(err);
    } finally {
      setLoading(false);
    }
  };

  return { data, error, loading, lastUpdated, refetch };
}
