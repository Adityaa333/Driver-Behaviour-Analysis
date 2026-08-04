import { ExclamationTriangleIcon } from '@heroicons/react/24/outline';

/**
 * Compact inline failure state for a single card/panel. Deliberately
 * small and un-alarming - one dropped poll shouldn't look like a system
 * failure - since usePolling keeps the last good data visible elsewhere
 * and this only renders when there truly is nothing to show yet.
 */
export default function ErrorState({ message = 'Unable to load data', compact = false }) {
  return (
    <div
      className={`flex items-center gap-2 text-[var(--color-text-muted)] ${
        compact ? 'text-xs py-2' : 'text-sm py-6'
      } justify-center`}
    >
      <ExclamationTriangleIcon className={compact ? 'w-4 h-4' : 'w-5 h-5'} />
      <span>{message}</span>
    </div>
  );
}
