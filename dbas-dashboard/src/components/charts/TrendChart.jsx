import {
  ResponsiveContainer,
  AreaChart,
  Area,
  XAxis,
  YAxis,
  CartesianGrid,
  Tooltip,
} from 'recharts';
import { formatTime } from '../../utils/formatters';
import ErrorState from '../common/ErrorState';
import { LoadingSpinner } from '../common/LoadingSpinner';

const TONE_HEX = {
  accent: '#3B82F6',
  success: '#22C55E',
  warning: '#F59E0B',
  danger: '#EF4444',
};

function CustomTooltip({ active, payload, label, unit }) {
  if (!active || !payload || payload.length === 0) return null;
  return (
    <div className="glass-card rounded-lg px-3 py-2 text-xs">
      <p className="text-[var(--color-text-muted)] mb-0.5">{formatTime(label)}</p>
      <p className="font-mono-num text-[var(--color-text-primary)] font-semibold">
        {payload[0].value} {unit}
      </p>
    </div>
  );
}

/**
 * @param {object} props
 * @param {Array<object>} props.data chronological (oldest first) records
 * @param {string} props.xKey timestamp field name (epoch ms)
 * @param {string} props.yKey value field name
 * @param {string} props.unit
 * @param {'accent'|'success'|'warning'|'danger'} props.tone
 */
export default function TrendChart({ data, xKey, yKey, unit = '', tone = 'accent', loading, height = 220, yDomain }) {
  const color = TONE_HEX[tone] || TONE_HEX.accent;
  const gradientId = `grad-${xKey}-${yKey}`;

  if (loading && (!data || data.length === 0)) {
    return (
      <div className="flex items-center justify-center" style={{ height }}>
        <LoadingSpinner />
      </div>
    );
  }

  if (!data || data.length === 0) {
    return (
      <div style={{ height }} className="flex items-center justify-center">
        <ErrorState message="Awaiting telemetry stream…" compact />
      </div>
    );
  }

  return (
    <ResponsiveContainer width="100%" height={height}>
      <AreaChart data={data} margin={{ top: 8, right: 12, bottom: 0, left: -12 }}>
        <defs>
          <linearGradient id={gradientId} x1="0" y1="0" x2="0" y2="1">
            <stop offset="5%" stopColor={color} stopOpacity={0.35} />
            <stop offset="95%" stopColor={color} stopOpacity={0} />
          </linearGradient>
        </defs>
        <CartesianGrid stroke="#1E293B" strokeDasharray="3 3" vertical={false} />
        <XAxis
          dataKey={xKey}
          tickFormatter={formatTime}
          stroke="#334155"
          tick={{ fill: '#64748B', fontSize: 10 }}
          minTickGap={40}
        />
        <YAxis
          stroke="#334155"
          tick={{ fill: '#64748B', fontSize: 10 }}
          width={36}
          domain={yDomain || ['auto', 'auto']}
        />
        <Tooltip content={<CustomTooltip unit={unit} />} />
        <Area
          type="monotone"
          dataKey={yKey}
          stroke={color}
          strokeWidth={2}
          fill={`url(#${gradientId})`}
          isAnimationActive={false}
          connectNulls
        />
      </AreaChart>
    </ResponsiveContainer>
  );
}
