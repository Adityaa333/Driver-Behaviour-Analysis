/** Formats an epoch-ms timestamp as a local time-of-day string, e.g. "14:32:07". */
export const formatTime = (timestampMs) => {
  if (timestampMs === null || timestampMs === undefined) return '--:--:--';
  return new Date(timestampMs).toLocaleTimeString([], { hour12: false });
};

/** Formats an epoch-ms timestamp as a compact date + time string. */
export const formatDateTime = (timestampMs) => {
  if (!timestampMs) return '—';
  return new Date(timestampMs).toLocaleString([], {
    month: 'short',
    day: 'numeric',
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit',
    hour12: false,
  });
};

/** Formats a duration in seconds as "Hh Mm Ss" (drops leading zero units). */
export const formatUptime = (totalSeconds) => {
  if (totalSeconds === null || totalSeconds === undefined) return '—';
  const h = Math.floor(totalSeconds / 3600);
  const m = Math.floor((totalSeconds % 3600) / 60);
  const s = Math.floor(totalSeconds % 60);
  if (h > 0) return `${h}h ${m}m ${s}s`;
  if (m > 0) return `${m}m ${s}s`;
  return `${s}s`;
};

/** Formats a byte count as a human-readable KB/MB string. */
export const formatBytes = (bytes) => {
  if (bytes === null || bytes === undefined) return '—';
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(2)} MB`;
};

/** Clamps and rounds a number for gauge-friendly display. */
export const roundTo = (value, decimals = 0) => {
  if (value === null || value === undefined || Number.isNaN(value)) return null;
  const factor = 10 ** decimals;
  return Math.round(value * factor) / factor;
};

/** Rating -> color-token mapping shared by score gauge/card/badges. */
export const ratingColor = (rating) => {
  switch (rating) {
    case 'Excellent':
      return 'success';
    case 'Fair':
      return 'warning';
    case 'Poor':
      return 'danger';
    default:
      return 'muted';
  }
};

/** Chooses a semantic color token for a numeric score (0-100). */
export const scoreColor = (score) => {
  if (score === null || score === undefined) return 'muted';
  if (score >= 80) return 'success';
  if (score >= 50) return 'warning';
  return 'danger';
};

/** Chooses a semantic color token for speed relative to a soft limit. */
export const speedColor = (kmh, limit = 120) => {
  if (kmh === null || kmh === undefined) return 'muted';
  if (kmh >= limit) return 'danger';
  if (kmh >= limit * 0.75) return 'warning';
  return 'success';
};

/** Title-cases a snake_case alert type, e.g. "excessive_idling" -> "Excessive Idling". */
export const titleCase = (str) => {
  if (!str) return '';
  return str
    .split('_')
    .map((word) => word.charAt(0).toUpperCase() + word.slice(1))
    .join(' ');
};
