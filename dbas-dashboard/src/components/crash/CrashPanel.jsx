import { motion, AnimatePresence } from 'framer-motion';
import { ShieldCheckIcon, ExclamationTriangleIcon } from '@heroicons/react/24/solid';
import { formatDateTime, roundTo } from '../../utils/formatters';

// How long after a crash timestamp the panel keeps flashing "CRASH
// DETECTED" before settling back to SAFE. The firmware's own crash
// detector applies a 10s cooldown against duplicate reports
// (CRASH_COOLDOWN_MS in crash_detection.c); this window is deliberately
// longer than that so a real crash stays visually urgent on the
// dashboard well past the moment it's first reported.
const CRASH_ACTIVE_WINDOW_MS = 60_000;

export default function CrashPanel({ crashes, loading }) {
  const latestCrash = crashes && crashes.length > 0 ? crashes[0] : null;
  const isActive = latestCrash && Date.now() - latestCrash.timestamp_ms < CRASH_ACTIVE_WINDOW_MS;

  return (
    <div
      className={`glass-card rounded-2xl h-48 flex flex-col items-center justify-center gap-4 relative overflow-hidden ${
        isActive ? 'crash-flash border-[var(--color-danger)]/60' : ''
      }`}
    >
      <AnimatePresence mode="wait">
        {isActive ? (
          <motion.div
            key="crash"
            initial={{ opacity: 0, scale: 0.9 }}
            animate={{ opacity: 1, scale: 1 }}
            exit={{ opacity: 0 }}
            className="flex flex-col items-center gap-3 text-center px-6"
          >
            <motion.div
              animate={{ scale: [1, 1.15, 1] }}
              transition={{ duration: 0.9, repeat: Infinity }}
            >
              <ExclamationTriangleIcon className="w-16 h-16 text-[var(--color-danger)]" />
            </motion.div>
            <div>
              <p className="font-display text-xl font-bold text-[var(--color-danger)] tracking-wide">
                CRASH DETECTED
              </p>
              <p className="text-xs text-[var(--color-text-secondary)] mt-2">
                {formatDateTime(latestCrash.timestamp_ms)}
              </p>
              <p className="text-xs text-[var(--color-text-muted)] mt-1 font-mono-num">
                {roundTo(latestCrash.total_accel_g, 2)}g &middot; {roundTo(latestCrash.total_gyro_dps, 0)}&deg;/s
              </p>
            </div>
          </motion.div>
        ) : (
          <motion.div
            key="safe"
            initial={{ opacity: 0, scale: 0.95 }}
            animate={{ opacity: 1, scale: 1 }}
            exit={{ opacity: 0 }}
            className="flex flex-col items-center gap-3 text-center px-6"
          >
            <ShieldCheckIcon className="w-16 h-16 text-[var(--color-success)]" />
            <div>
              <p className="font-display text-xl font-bold text-[var(--color-success)] tracking-wide">
                SAFE
              </p>
              <p className="text-xs text-[var(--color-text-muted)] mt-2">
                {loading
                  ? 'Checking crash history…'
                  : latestCrash
                  ? `Last event: ${formatDateTime(latestCrash.timestamp_ms)}`
                  : 'No crash events recorded'}
              </p>
            </div>
          </motion.div>
        )}
      </AnimatePresence>
    </div>
  );
}
