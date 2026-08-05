import {
  WifiIcon,
  SignalIcon,
  ClockIcon,
  CircleStackIcon,
  CpuChipIcon,
  TagIcon,
} from '@heroicons/react/24/outline';
import StatusDot from '../common/StatusDot';
import ErrorState from '../common/ErrorState';
import { LoadingSpinner } from '../common/LoadingSpinner';
import { formatUptime, formatBytes } from '../../utils/formatters';

function Row({ icon: Icon, label, children }) {
  return (
    <div className="flex items-center justify-between py-2.5 border-b border-[var(--color-border)]/50 last:border-0">
      <div className="flex items-center gap-2 text-[var(--color-text-secondary)]">
        <Icon className="w-4 h-4 text-[var(--color-text-muted)]" />
        <span className="text-xs">{label}</span>
      </div>
      <div className="text-xs font-mono-num text-[var(--color-text-primary)]">{children}</div>
    </div>
  );
}

function rssiQuality(rssi) {
  if (rssi === null || rssi === undefined) return { label: '—', tone: 'muted' };
  if (rssi >= -60) return { label: 'Excellent', tone: 'success' };
  if (rssi >= -75) return { label: 'Fair', tone: 'warning' };
  return { label: 'Weak', tone: 'danger' };
}

export default function SystemStatusPanel({ status, firmwareVersion, loading }) {
  if (loading && !status) {
    return (
      <div className="glass-card rounded-2xl h-72 flex items-center justify-center">
        <LoadingSpinner />
      </div>
    );
  }

  if (!status) {
    return (
      <div className="glass-card rounded-2xl h-72 flex items-center justify-center">
        <ErrorState message="No status reported yet for this device" compact />
      </div>
    );
  }

  const rssi = rssiQuality(status.wifi_rssi_dbm);

  return (
    <div className="glass-card rounded-2xl p-4 h-72 overflow-y-auto">
      <Row icon={WifiIcon} label="WiFi Connected">
        <span className="flex items-center gap-1.5">
          <StatusDot tone={status.wifi_connected ? 'success' : 'danger'} />
          <span className={status.wifi_connected ? 'text-[var(--color-success)]' : 'text-[var(--color-danger)]'}>
            {status.wifi_connected ? 'Connected' : 'Disconnected'}
          </span>
        </span>
      </Row>
      <Row icon={SignalIcon} label="MQTT Connected">
        <span className="flex items-center gap-1.5">
          <StatusDot tone={status.mqtt_connected ? 'success' : 'danger'} />
          <span className={status.mqtt_connected ? 'text-[var(--color-success)]' : 'text-[var(--color-danger)]'}>
            {status.mqtt_connected ? 'Connected' : 'Disconnected'}
          </span>
        </span>
      </Row>
      <Row icon={ClockIcon} label="Uptime">
        {formatUptime(status.uptime_sec)}
      </Row>
      <Row icon={CircleStackIcon} label="Heap Memory">
        {formatBytes(status.free_heap_bytes)}
        {status.min_free_heap_bytes != null && (
          <span className="text-[var(--color-text-muted)]"> (min {formatBytes(status.min_free_heap_bytes)})</span>
        )}
      </Row>
      <Row icon={TagIcon} label="Firmware Version">
        {firmwareVersion || '—'}
      </Row>
      <Row icon={CpuChipIcon} label="RSSI">
        <span
          className={
            rssi.tone === 'success'
              ? 'text-[var(--color-success)]'
              : rssi.tone === 'warning'
              ? 'text-[var(--color-warning)]'
              : rssi.tone === 'danger'
              ? 'text-[var(--color-danger)]'
              : 'text-[var(--color-text-muted)]'
          }
        >
          {status.wifi_rssi_dbm != null ? `${status.wifi_rssi_dbm} dBm` : '—'} &middot; {rssi.label}
        </span>
      </Row>
    </div>
  );
}
