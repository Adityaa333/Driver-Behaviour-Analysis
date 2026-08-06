import { useMemo, useState, Suspense, lazy } from 'react';
import {
  ChartBarIcon,
  BoltIcon,
  MapPinIcon,
  BellAlertIcon,
  ShieldExclamationIcon,
  ShieldCheckIcon,
  CpuChipIcon,
  ChevronDownIcon,
} from '@heroicons/react/24/outline';
import { useDeviceContext } from '../context/DeviceContext';
import SectionHeader from '../components/common/SectionHeader';
import { LoadingSpinner } from '../components/common/LoadingSpinner';

import StatCardsRow from '../components/cards/StatCardsRow';
import ScoreBreakoutRow from '../components/cards/ScoreBreakoutRow';
import GaugesRow from '../components/gauges/GaugesRow';
import ChartsGrid from '../components/charts/ChartsGrid';
import AlertsTable from '../components/alerts/AlertsTable';
import CrashPanel from '../components/crash/CrashPanel';
import GeofencePanel from '../components/geofence/GeofencePanel';
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
  const { data: alerts, loading: alertsPollingLoading } = useDeviceAlerts(selectedDeviceId, 20);
  const { data: crashes, loading: crashesLoading } = useDeviceCrashes(selectedDeviceId, 10);

  const firmwareVersion = devices.find((d) => d.device_id === selectedDeviceId)?.firmware_version;
  const [showScoreBreakdown, setShowScoreBreakdown] = useState(false);

  const scoreAlerts = useMemo(() => {
    if (!score) return [];
    const ts = score.timestamp_ms || Date.now();
    const alerts = [];

    const addCountAlert = (type, count, message) => {
      if (count > 0) {
        alerts.push({
          timestamp_ms: ts,
          alert_type: type,
          severity: type === 'crash' ? 'High' : 'Medium',
          message,
        });
      }
    };

    addCountAlert('harsh_braking', score.harsh_braking_count, `${score.harsh_braking_count} harsh braking events detected`);
    addCountAlert('harsh_acceleration', score.harsh_accel_count, `${score.harsh_accel_count} harsh acceleration events detected`);
    addCountAlert('harsh_cornering', score.harsh_cornering_count, `${score.harsh_cornering_count} harsh cornering events detected`);
    addCountAlert('overspeed', score.overspeed_count, `${score.overspeed_count} overspeed events detected`);
    if (score.idling_seconds_total > 0) {
      alerts.push({
        timestamp_ms: ts,
        alert_type: 'excessive_idling',
        severity: 'Medium',
        message: `${Math.round(score.idling_seconds_total)}s idling detected`,
      });
    }
    addCountAlert('geofence_violation', score.geofence_violation_count, `${score.geofence_violation_count} geofence violations detected`);
    addCountAlert('crash', score.crash_count, `${score.crash_count} crash events detected`);

    return alerts;
  }, [score]);

  const crashAlerts = useMemo(() => {
    if (!crashes || crashes.length === 0) return [];
    return crashes.map((crash) => ({
      timestamp_ms: crash.timestamp_ms,
      alert_type: 'crash',
      severity: 'High',
      message: `Crash detected with ${crash.total_accel_g.toFixed(1)}g accel`,
    }));
  }, [crashes]);

  const combinedAlerts = useMemo(() => {
    const allAlerts = [...scoreAlerts, ...crashAlerts, ...(alerts || [])];
    return allAlerts.sort((a, b) => b.timestamp_ms - a.timestamp_ms);
  }, [scoreAlerts, crashAlerts, alerts]);

  const combinedAlertsLoading = alertsPollingLoading || crashesLoading;

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

      {/* Map + Crash / Geofence panels */}
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
        <div className="grid grid-cols-1 gap-4">
          <div>
            <SectionHeader title="Crash Detection" icon={ShieldExclamationIcon} />
            <CrashPanel crashes={crashes} loading={crashesLoading} />
          </div>
          <div>
            <SectionHeader title="Geofence Status" icon={ShieldCheckIcon} />
            <GeofencePanel telemetry={telemetry} loading={telemetryLoading} />
          </div>
        </div>
      </section>

      {/* Alerts + System status */}
      <section className="grid grid-cols-1 lg:grid-cols-3 gap-4">
        <div className="lg:col-span-2">
          <SectionHeader title="Recent Alerts" icon={BellAlertIcon} />
          <AlertsTable alerts={combinedAlerts} loading={combinedAlertsLoading} />
        </div>
        <div>
          <SectionHeader title="System Status" icon={CpuChipIcon} />
          <SystemStatusPanel status={status} firmwareVersion={firmwareVersion} loading={statusLoading} />
        </div>
      </section>

      {/* Score details */}
      <section>
        <div className="flex flex-col gap-2">
          <div className="flex flex-col sm:flex-row sm:items-center sm:justify-between gap-3">
            <SectionHeader title="Score Breakdown" subtitle="Latest event counts from the device" icon={ShieldCheckIcon} />
            <button
              type="button"
              onClick={() => setShowScoreBreakdown((open) => !open)}
              className="inline-flex items-center gap-2 px-4 py-2 rounded-2xl border border-[var(--color-border)] bg-white/5 text-xs font-semibold text-[var(--color-text-secondary)] hover:bg-white/10 transition"
            >
              <span>{showScoreBreakdown ? 'Hide details' : 'Show details'}</span>
              <ChevronDownIcon
                className={`w-4 h-4 transition-transform ${showScoreBreakdown ? 'rotate-180' : 'rotate-0'}`}
              />
            </button>
          </div>
          {showScoreBreakdown && (
            <div className="glass-card rounded-2xl p-4">
              <ScoreBreakoutRow score={score} loading={scoreLoading} />
            </div>
          )}
          {!showScoreBreakdown && (
            <div className="text-xs text-[var(--color-text-muted)]">Toggle this panel to view the current score event counts when available.</div>
          )}
        </div>
      </section>
    </div>
  );
}
