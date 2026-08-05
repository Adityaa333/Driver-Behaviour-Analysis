import { Suspense, lazy } from 'react';
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
import { LoadingSpinner } from '../components/common/LoadingSpinner';

import StatCardsRow from '../components/cards/StatCardsRow';
import GaugesRow from '../components/gauges/GaugesRow';
import ChartsGrid from '../components/charts/ChartsGrid';
import AlertsTable from '../components/alerts/AlertsTable';
import CrashPanel from '../components/crash/CrashPanel';
import SystemStatusPanel from '../components/status/SystemStatusPanel';

// Leaflet pulls in a meaningful chunk of JS/CSS on its own; deferring it
// to a lazy chunk keeps the initial dashboard paint (cards/gauges/charts)
// fast even on a slow fleet-operator connection.
const VehicleMap = lazy(() => import('../components/map/VehicleMap'));

import useLatestScore from '../hooks/useLatestScore';
import useLatestTelemetry from '../hooks/useLatestTelemetry';
import useScoreHistory from '../hooks/useScoreHistory';
import useTelemetryHistory from '../hooks/useTelemetryHistory';
import useDeviceStatus from '../hooks/useDeviceStatus';
import useDeviceAlerts from '../hooks/useDeviceAlerts';
import useDeviceCrashes from '../hooks/useDeviceCrashes';

export default function DashboardPage() {
  const { selectedDeviceId, loading, devices } = useDeviceContext();

  const { data: score, loading: scoreLoading } = useLatestScore(selectedDeviceId);
  const { data: telemetry, loading: telemetryLoading } = useLatestTelemetry(selectedDeviceId);
  const { data: scoreHistory, loading: scoreHistoryLoading } = useScoreHistory(selectedDeviceId, 30);
  const { data: telemetryHistory, loading: telemetryHistoryLoading } = useTelemetryHistory(selectedDeviceId, 50);
  const { data: status, loading: statusLoading } = useDeviceStatus(selectedDeviceId);
  const { data: alerts, loading: alertsLoading } = useDeviceAlerts(selectedDeviceId, 20);
  const { data: crashes, loading: crashesLoading } = useDeviceCrashes(selectedDeviceId, 10);

  const firmwareVersion = devices.find((d) => d.device_id === selectedDeviceId)?.firmware_version;

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
        <StatCardsRow
          score={score}
          telemetry={telemetry}
          scoreLoading={scoreLoading}
          telemetryLoading={telemetryLoading}
        />
      </section>

      {/* Gauges */}
      <section>
        <SectionHeader title="Instrument Cluster" icon={BoltIcon} />
        <GaugesRow
          score={score}
          telemetry={telemetry}
          scoreLoading={scoreLoading}
          telemetryLoading={telemetryLoading}
        />
      </section>

      {/* Charts */}
      <section>
        <SectionHeader title="Trends" icon={ChartBarIcon} />
        <ChartsGrid
          scoreHistory={scoreHistory}
          telemetryHistory={telemetryHistory}
          scoreLoading={scoreHistoryLoading}
          telemetryLoading={telemetryHistoryLoading}
        />
      </section>

      {/* Map + Crash panel */}
      <section className="grid grid-cols-1 lg:grid-cols-3 gap-4">
        <div className="lg:col-span-2">
          <SectionHeader title="Vehicle Location" icon={MapPinIcon} />
          <Suspense
            fallback={
              <div className="glass-card rounded-2xl h-96 flex items-center justify-center">
                <LoadingSpinner />
              </div>
            }
          >
            <VehicleMap telemetry={telemetry} loading={telemetryLoading} />
          </Suspense>
        </div>
        <div>
          <SectionHeader title="Crash Detection" icon={ShieldExclamationIcon} />
          <CrashPanel crashes={crashes} loading={crashesLoading} />
        </div>
      </section>

      {/* Alerts + System status */}
      <section className="grid grid-cols-1 lg:grid-cols-3 gap-4">
        <div className="lg:col-span-2">
          <SectionHeader title="Recent Alerts" icon={BellAlertIcon} />
          <AlertsTable alerts={alerts} loading={alertsLoading} />
        </div>
        <div>
          <SectionHeader title="System Status" icon={CpuChipIcon} />
          <SystemStatusPanel status={status} firmwareVersion={firmwareVersion} loading={statusLoading} />
        </div>
      </section>
    </div>
  );
}
