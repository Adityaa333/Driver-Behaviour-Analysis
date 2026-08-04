import {
  ChartBarIcon,
  BoltIcon,
  MapPinIcon,
  BellAlertIcon,
  ShieldExclamationIcon,
  CpuChipIcon,
} from '@heroicons/react/24/outline';
import { useDeviceContext } from '../context/DeviceContext';
import SectionHeader from '../components/common/SectionHeader';
import ComingSoonPanel from '../components/common/ComingSoonPanel';
import { LoadingSpinner } from '../components/common/LoadingSpinner';

export default function DashboardPage() {
  const { selectedDeviceId, loading, devices } = useDeviceContext();

  if (loading && devices.length === 0) {
    return (
      <div className="flex flex-col items-center justify-center py-32 gap-3">
        <LoadingSpinner size={28} />
        <p className="text-sm text-[var(--color-text-muted)]">Discovering fleet devices…</p>
      </div>
    );
  }

  if (!selectedDeviceId) {
    return (
      <div className="flex flex-col items-center justify-center py-32 gap-2 text-center">
        <p className="text-sm text-[var(--color-text-secondary)]">No devices have reported yet.</p>
        <p className="text-xs text-[var(--color-text-muted)] max-w-sm">
          Once an ESP32 device publishes telemetry over MQTT and the backend records it, it will
          appear here automatically.
        </p>
      </div>
    );
  }

  return (
    <div className="flex flex-col gap-8">
      {/* Stat cards */}
      <section>
        <SectionHeader title="Live Snapshot" subtitle={`Device ${selectedDeviceId}`} icon={ChartBarIcon} />
        <div className="grid grid-cols-2 md:grid-cols-3 xl:grid-cols-6 gap-4">
          <ComingSoonPanel label="Safety Score" height="h-28" />
          <ComingSoonPanel label="Vehicle Speed" height="h-28" />
          <ComingSoonPanel label="Engine RPM" height="h-28" />
          <ComingSoonPanel label="Throttle" height="h-28" />
          <ComingSoonPanel label="Engine Temp" height="h-28" />
          <ComingSoonPanel label="GPS Speed" height="h-28" />
        </div>
      </section>

      {/* Gauges */}
      <section>
        <SectionHeader title="Instrument Cluster" icon={BoltIcon} />
        <div className="grid grid-cols-1 md:grid-cols-2 gap-4">
          <ComingSoonPanel label="Driver Score Gauge" height="h-72" />
          <ComingSoonPanel label="Vehicle Speed Gauge" height="h-72" />
        </div>
      </section>

      {/* Charts */}
      <section>
        <SectionHeader title="Trends" icon={ChartBarIcon} />
        <div className="grid grid-cols-1 lg:grid-cols-2 gap-4">
          <ComingSoonPanel label="Driver Score History" height="h-64" />
          <ComingSoonPanel label="Vehicle Speed History" height="h-64" />
          <ComingSoonPanel label="RPM History" height="h-64" />
          <ComingSoonPanel label="Throttle History" height="h-64" />
        </div>
      </section>

      {/* Map + Crash panel */}
      <section className="grid grid-cols-1 lg:grid-cols-3 gap-4">
        <div className="lg:col-span-2">
          <SectionHeader title="Vehicle Location" icon={MapPinIcon} />
          <ComingSoonPanel label="Live GPS Map" height="h-96" />
        </div>
        <div>
          <SectionHeader title="Crash Detection" icon={ShieldExclamationIcon} />
          <ComingSoonPanel label="Crash Status Panel" height="h-96" />
        </div>
      </section>

      {/* Alerts + System status */}
      <section className="grid grid-cols-1 lg:grid-cols-3 gap-4">
        <div className="lg:col-span-2">
          <SectionHeader title="Recent Alerts" icon={BellAlertIcon} />
          <ComingSoonPanel label="Alerts Table" height="h-72" />
        </div>
        <div>
          <SectionHeader title="System Status" icon={CpuChipIcon} />
          <ComingSoonPanel label="Device Health Panel" height="h-72" />
        </div>
      </section>
    </div>
  );
}
