import { motion } from 'framer-motion';
import { Skeleton } from './LoadingSpinner';

const TONE_STYLES = {
  success: { text: 'text-[var(--color-success)]', glow: 'shadow-[0_0_0_1px_rgba(34,197,94,0.25)]', bar: 'bg-[var(--color-success)]' },
  warning: { text: 'text-[var(--color-warning)]', glow: 'shadow-[0_0_0_1px_rgba(245,158,11,0.25)]', bar: 'bg-[var(--color-warning)]' },
  danger: { text: 'text-[var(--color-danger)]', glow: 'shadow-[0_0_0_1px_rgba(239,68,68,0.25)]', bar: 'bg-[var(--color-danger)]' },
  accent: { text: 'text-[var(--color-accent)]', glow: 'shadow-[0_0_0_1px_rgba(59,130,246,0.25)]', bar: 'bg-[var(--color-accent)]' },
  muted: { text: 'text-[var(--color-text-muted)]', glow: '', bar: 'bg-[var(--color-text-muted)]' },
};

/**
 * A single live-metric card: icon + label up top, a large mono-numeral
 * reading, a unit, and an optional sublabel (e.g. score rating). The
 * left accent bar + icon color both key off `tone` so the whole card
 * reads as one semantic color at a glance.
 */
export default function StatCard({
  label,
  value,
  unit,
  sublabel,
  icon: Icon,
  tone = 'accent',
  loading = false,
  note,
}) {
  const styles = TONE_STYLES[tone] || TONE_STYLES.accent;
  const hasValue = value !== null && value !== undefined && value !== '';

  return (
    <motion.div
      initial={{ opacity: 0, y: 8 }}
      animate={{ opacity: 1, y: 0 }}
      transition={{ duration: 0.25 }}
      className={`relative glass-card rounded-2xl p-4 overflow-hidden ${styles.glow}`}
    >
      <span className={`absolute left-0 top-0 bottom-0 w-1 ${styles.bar} opacity-70`} />
      <div className="flex items-center justify-between mb-3">
        <span className="text-[11px] uppercase tracking-wider text-[var(--color-text-muted)] font-medium">
          {label}
        </span>
        {Icon && <Icon className={`w-4 h-4 ${styles.text}`} />}
      </div>

      {loading && !hasValue ? (
        <Skeleton className="h-8 w-20" />
      ) : hasValue ? (
        <div className="flex items-baseline gap-1.5">
          <span className={`font-mono-num text-2xl font-semibold ${styles.text}`}>{value}</span>
          {unit && <span className="text-xs text-[var(--color-text-muted)]">{unit}</span>}
        </div>
      ) : (
        <div className="flex items-baseline gap-1.5">
          <span className="font-mono-num text-2xl font-semibold text-[var(--color-text-muted)]">
            —
          </span>
        </div>
      )}

      {sublabel && (
        <p className={`text-[11px] mt-1 font-medium ${styles.text}`}>{sublabel}</p>
      )}
      {note && !sublabel && (
        <p className="text-[11px] mt-1 text-[var(--color-text-muted)]">{note}</p>
      )}
    </motion.div>
  );
}
