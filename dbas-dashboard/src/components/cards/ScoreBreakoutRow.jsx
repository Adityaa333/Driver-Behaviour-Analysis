import StatCard from '../common/StatCard';

const countTone = (value) => {
  if (value === null || value === undefined) return 'muted';
  return value > 0 ? 'danger' : 'success';
};

const secondsTone = (value) => {
  if (value === null || value === undefined) return 'muted';
  return value > 0 ? 'warning' : 'success';
};

export default function ScoreBreakoutRow({ score, loading }) {
  return (
    <div className="grid grid-cols-1 sm:grid-cols-2 xl:grid-cols-3 gap-4">
      <StatCard
        label="Harsh Braking"
        value={score?.harsh_braking_count ?? null}
        icon={null}
        tone={countTone(score?.harsh_braking_count)}
        loading={loading}
      />
      <StatCard
        label="Harsh Acceleration"
        value={score?.harsh_accel_count ?? null}
        icon={null}
        tone={countTone(score?.harsh_accel_count)}
        loading={loading}
      />
      <StatCard
        label="Harsh Cornering"
        value={score?.harsh_cornering_count ?? null}
        icon={null}
        tone={countTone(score?.harsh_cornering_count)}
        loading={loading}
      />
      <StatCard
        label="Overspeed Events"
        value={score?.overspeed_count ?? null}
        icon={null}
        tone={countTone(score?.overspeed_count)}
        loading={loading}
      />
      <StatCard
        label="Idling Seconds"
        value={score?.idling_seconds_total ?? null}
        unit="s"
        icon={null}
        tone={secondsTone(score?.idling_seconds_total)}
        loading={loading}
      />
      <StatCard
        label="Crash Count"
        value={score?.crash_count ?? null}
        icon={null}
        tone={countTone(score?.crash_count)}
        loading={loading}
      />
      <StatCard
        label="Geofence Violations"
        value={score?.geofence_violation_count ?? null}
        icon={null}
        tone={countTone(score?.geofence_violation_count)}
        loading={loading}
      />
    </div>
  );
}
