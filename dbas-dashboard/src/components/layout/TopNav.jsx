import { useEffect, useState } from 'react';
import { TruckIcon, ChevronDownIcon } from '@heroicons/react/24/outline';
import { useDeviceContext } from '../../context/DeviceContext';
import useBackendHealth from '../../hooks/useBackendHealth';
import StatusDot from '../common/StatusDot';
import { formatTime } from '../../utils/formatters';

function HealthBadge({ label, up, loading }) {
  const tone = loading ? 'muted' : up ? 'success' : 'danger';
  return (
    <div className="flex items-center gap-2 px-3 py-1.5 rounded-full bg-white/[0.03] border border-[var(--color-border)]">
      <StatusDot tone={tone} pulse={up && !loading} />
      <span className="text-xs text-[var(--color-text-secondary)] font-medium">{label}</span>
      <span
        className={`text-xs font-semibold ${
          loading
            ? 'text-[var(--color-text-muted)]'
            : up
            ? 'text-[var(--color-success)]'
            : 'text-[var(--color-danger)]'
        }`}
      >
        {loading ? '…' : up ? 'Online' : 'Offline'}
      </span>
    </div>
  );
}

function DevicePicker() {
  const { devices, selectedDeviceId, selectDevice } = useDeviceContext();
  const [open, setOpen] = useState(false);

  if (devices.length === 0) {
    return (
      <div className="px-3 py-1.5 rounded-lg bg-white/[0.03] border border-[var(--color-border)] text-xs text-[var(--color-text-muted)]">
        No devices detected
      </div>
    );
  }

  const current = devices.find((d) => d.device_id === selectedDeviceId);

  return (
    <div className="relative">
      <button
        onClick={() => setOpen((o) => !o)}
        className="flex items-center gap-2 px-3 py-1.5 rounded-lg bg-white/[0.03] border border-[var(--color-border)] hover:border-[var(--color-accent)]/50 transition-colors"
      >
        <TruckIcon className="w-4 h-4 text-[var(--color-accent)]" />
        <span className="font-mono-num text-xs text-[var(--color-text-primary)]">
          {current?.device_id || selectedDeviceId}
        </span>
        <ChevronDownIcon className="w-3.5 h-3.5 text-[var(--color-text-muted)]" />
      </button>
      {open && (
        <>
          <div className="fixed inset-0 z-10" onClick={() => setOpen(false)} />
          <div className="absolute right-0 mt-2 w-64 rounded-xl glass-card p-1.5 z-20 shadow-2xl">
            {devices.map((d) => (
              <button
                key={d.device_id}
                onClick={() => {
                  selectDevice(d.device_id);
                  setOpen(false);
                }}
                className={`w-full text-left px-3 py-2 rounded-lg text-xs flex items-center justify-between transition-colors ${
                  d.device_id === selectedDeviceId
                    ? 'bg-[var(--color-accent)]/15 text-[var(--color-accent)]'
                    : 'text-[var(--color-text-secondary)] hover:bg-white/5'
                }`}
              >
                <span className="font-mono-num">{d.device_id}</span>
                <span className="text-[var(--color-text-muted)]">{d.device_type || 'device'}</span>
              </button>
            ))}
          </div>
        </>
      )}
    </div>
  );
}

function LiveClock() {
  const [now, setNow] = useState(Date.now());
  useEffect(() => {
    const id = setInterval(() => setNow(Date.now()), 1000);
    return () => clearInterval(id);
  }, []);
  return (
    <span className="font-mono-num text-xs text-[var(--color-text-secondary)]">
      {formatTime(now)}
    </span>
  );
}

export default function TopNav() {
  const { backendUp, mqttUp, loading } = useBackendHealth();

  return (
    <header className="sticky top-0 z-30 border-b border-[var(--color-border)] bg-[var(--color-bg)]/85 backdrop-blur-xl">
      <div className="max-w-[1600px] mx-auto px-6 h-16 flex items-center justify-between gap-4">
        <div className="flex items-center gap-3">
          <div className="w-9 h-9 rounded-lg bg-gradient-to-br from-[var(--color-accent)] to-blue-700 flex items-center justify-center shadow-lg shadow-blue-500/20">
            <TruckIcon className="w-5 h-5 text-white" />
          </div>
          <div>
            <h1 className="font-display font-semibold text-sm tracking-wide leading-none">
              DBAS
            </h1>
            <p className="text-[10px] text-[var(--color-text-muted)] leading-none mt-1">
              Driver Behaviour Analysis System
            </p>
          </div>
        </div>

        <div className="hidden md:flex items-center gap-3">
          <HealthBadge label="MQTT" up={mqttUp} loading={loading} />
          <HealthBadge label="Backend" up={backendUp} loading={loading} />
        </div>

        <div className="flex items-center gap-3">
          <DevicePicker />
          <div className="hidden sm:flex flex-col items-end leading-none">
            <span className="text-[10px] text-[var(--color-text-muted)] mb-1">Last Updated</span>
            <LiveClock />
          </div>
        </div>
      </div>
    </header>
  );
}
