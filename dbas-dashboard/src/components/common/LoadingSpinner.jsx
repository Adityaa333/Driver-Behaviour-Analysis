/** Small inline spinner for cards/panels still awaiting first data. */
export function LoadingSpinner({ size = 20 }) {
  return (
    <div
      className="animate-spin rounded-full border-2 border-[var(--color-border)] border-t-[var(--color-accent)]"
      style={{ width: size, height: size }}
      role="status"
      aria-label="Loading"
    />
  );
}

/** Shimmering placeholder block, used while a card awaits its first poll. */
export function Skeleton({ className = '' }) {
  return (
    <div
      className={`animate-pulse rounded-lg bg-[var(--color-border)]/40 ${className}`}
    />
  );
}

export default LoadingSpinner;
