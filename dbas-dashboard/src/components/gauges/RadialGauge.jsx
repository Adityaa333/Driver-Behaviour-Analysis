import { motion } from 'framer-motion';
import { describeArc, valueToAngle, fractionToAngle, polarToCartesian } from '../../utils/gaugeMath';

const SIZE = 240;
const CENTER = SIZE / 2;
const RADIUS = 92;
const TONE_HEX = {
  success: '#22C55E',
  warning: '#F59E0B',
  danger: '#EF4444',
};

/**
 * @param {object} props
 * @param {number} props.value
 * @param {number} props.min
 * @param {number} props.max
 * @param {string} props.unit
 * @param {string} props.label
 * @param {{ upTo: number, tone: 'success'|'warning'|'danger' }[]} props.zones
 *   Ordered ascending zone boundaries, e.g. [{upTo:50,tone:'danger'},
 *   {upTo:80,tone:'warning'},{upTo:100,tone:'success'}] mirrors the
 *   dashboard's score gauge segments.
 */
export default function RadialGauge({ value, min = 0, max = 100, unit = '', label, zones, loading }) {
  const hasValue = value !== null && value !== undefined && !Number.isNaN(value);
  const displayValue = hasValue ? Math.round(value) : null;
  const needleAngle = hasValue ? valueToAngle(value, min, max) : valueToAngle(min, min, max);

  // Build colored zone arcs from ascending boundaries.
  let prevFraction = 0;
  const zoneArcs = (zones || []).map((zone, i) => {
    const fraction = (zone.upTo - min) / (max - min);
    const startAngle = fractionToAngle(prevFraction);
    const endAngle = fractionToAngle(fraction);
    prevFraction = fraction;
    return (
      <path
        key={i}
        d={describeArc(CENTER, CENTER, RADIUS, startAngle, endAngle)}
        stroke={TONE_HEX[zone.tone]}
        strokeWidth={10}
        strokeLinecap="round"
        fill="none"
        opacity={0.85}
      />
    );
  });

  const needleTip = polarToCartesian(CENTER, CENTER, RADIUS - 22, needleAngle);
  const activeTone = hasValue
    ? zones?.find((z) => value <= z.upTo)?.tone || zones?.[zones.length - 1]?.tone
    : null;

  // Tick marks around the arc for an instrument-panel feel.
  const ticks = Array.from({ length: 11 }, (_, i) => {
    const frac = i / 10;
    const angle = fractionToAngle(frac);
    const outer = polarToCartesian(CENTER, CENTER, RADIUS + 8, angle);
    const inner = polarToCartesian(CENTER, CENTER, RADIUS + (i % 5 === 0 ? 0 : 3), angle);
    return <line key={i} x1={inner.x} y1={inner.y} x2={outer.x} y2={outer.y} stroke="#2C3B57" strokeWidth={i % 5 === 0 ? 2 : 1} />;
  });

  return (
    <div className="flex flex-col items-center">
      <svg width={SIZE} height={SIZE * 0.72} viewBox={`0 0 ${SIZE} ${SIZE * 0.78}`}>
        {/* Base track */}
        <path
          d={describeArc(CENTER, CENTER, RADIUS, fractionToAngle(0), fractionToAngle(1))}
          stroke="#243248"
          strokeWidth={10}
          strokeLinecap="round"
          fill="none"
        />
        {zoneArcs}
        {ticks}

        {/* Needle */}
        {hasValue && (
          <motion.line
            x1={CENTER}
            y1={CENTER}
            initial={false}
            animate={{ x2: needleTip.x, y2: needleTip.y }}
            transition={{ type: 'spring', stiffness: 60, damping: 12 }}
            stroke={activeTone ? TONE_HEX[activeTone] : '#E2E8F0'}
            strokeWidth={3}
            strokeLinecap="round"
          />
        )}
        <circle cx={CENTER} cy={CENTER} r={6} fill="#0F172A" stroke="#2C3B57" strokeWidth={2} />

        <text
          x={CENTER}
          y={CENTER + 34}
          textAnchor="middle"
          className="font-mono-num"
          fontSize="30"
          fontWeight="600"
          fill={activeTone ? TONE_HEX[activeTone] : '#94A3B8'}
        >
          {loading && !hasValue ? '…' : displayValue !== null ? displayValue : '—'}
        </text>
        <text
          x={CENTER}
          y={CENTER + 54}
          textAnchor="middle"
          fontSize="11"
          fill="#64748B"
        >
          {unit}
        </text>
      </svg>
      <span className="text-xs font-medium text-[var(--color-text-secondary)] mt-1">{label}</span>
    </div>
  );
}
