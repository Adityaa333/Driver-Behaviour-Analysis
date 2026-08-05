/**
 * Temporary placeholder for a dashboard section that hasn't been built
 * yet in this incremental pass (cards / gauges / charts / map / alerts
 * / crash panel / status grid land in the next steps). Kept visually
 * consistent with glass-card so the shell reads as "in progress," not
 * broken.
 */
export default function ComingSoonPanel({ label, height = 'h-40' }) {
  return (
    <div
      className={`glass-card rounded-2xl ${height} flex items-center justify-center border-dashed`}
    >
      <span className="text-xs text-[var(--color-text-muted)] font-mono-num tracking-wide">
        {label} — coming next
      </span>
    </div>
  );
}
