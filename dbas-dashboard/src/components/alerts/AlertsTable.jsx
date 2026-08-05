import { motion, AnimatePresence } from 'framer-motion';
import { BellAlertIcon } from '@heroicons/react/24/outline';
import ErrorState from '../common/ErrorState';
import { LoadingSpinner } from '../common/LoadingSpinner';
import { formatDateTime, titleCase } from '../../utils/formatters';

const SEVERITY_STYLES = {
  High: 'bg-[var(--color-danger-soft)] text-[var(--color-danger)] border-[var(--color-danger)]/30',
  Medium: 'bg-[var(--color-warning-soft)] text-[var(--color-warning)] border-[var(--color-warning)]/30',
  Low: 'bg-white/5 text-[var(--color-text-secondary)] border-[var(--color-border)]',
};

function SeverityBadge({ severity }) {
  const style = SEVERITY_STYLES[severity] || SEVERITY_STYLES.Low;
  return (
    <span className={`inline-flex items-center px-2 py-0.5 rounded-full text-[10px] font-semibold border ${style}`}>
      {severity}
    </span>
  );
}

export default function AlertsTable({ alerts, loading }) {
  if (loading && (!alerts || alerts.length === 0)) {
    return (
      <div className="glass-card rounded-2xl h-72 flex items-center justify-center">
        <LoadingSpinner />
      </div>
    );
  }

  if (!alerts || alerts.length === 0) {
    return (
      <div className="glass-card rounded-2xl h-72 flex flex-col items-center justify-center gap-2">
        <BellAlertIcon className="w-6 h-6 text-[var(--color-text-muted)]" />
        <ErrorState message="No alerts recorded for this device" compact />
      </div>
    );
  }

  return (
    <div className="glass-card rounded-2xl overflow-hidden">
      <div className="overflow-x-auto max-h-72 overflow-y-auto">
        <table className="w-full text-left text-xs">
          <thead className="sticky top-0 bg-[var(--color-bg-elevated)]/95 backdrop-blur">
            <tr className="text-[10px] uppercase tracking-wider text-[var(--color-text-muted)]">
              <th className="px-4 py-2.5 font-medium">Severity</th>
              <th className="px-4 py-2.5 font-medium">Timestamp</th>
              <th className="px-4 py-2.5 font-medium">Description</th>
            </tr>
          </thead>
          <tbody>
            <AnimatePresence initial={false}>
              {alerts.map((alert, idx) => (
                <motion.tr
                  key={`${alert.timestamp_ms}-${idx}`}
                  initial={{ opacity: 0 }}
                  animate={{ opacity: 1 }}
                  className="border-t border-[var(--color-border)]/60 hover:bg-white/[0.02] transition-colors"
                >
                  <td className="px-4 py-2.5">
                    <SeverityBadge severity={alert.severity} />
                  </td>
                  <td className="px-4 py-2.5 font-mono-num text-[var(--color-text-secondary)] whitespace-nowrap">
                    {formatDateTime(alert.timestamp_ms)}
                  </td>
                  <td className="px-4 py-2.5 text-[var(--color-text-primary)]">
                    <span className="font-medium">{titleCase(alert.alert_type)}</span>
                    <span className="text-[var(--color-text-muted)]"> — {alert.message}</span>
                  </td>
                </motion.tr>
              ))}
            </AnimatePresence>
          </tbody>
        </table>
      </div>
    </div>
  );
}
