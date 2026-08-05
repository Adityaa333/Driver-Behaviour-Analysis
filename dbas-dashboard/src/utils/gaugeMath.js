/**
 * Geometry helpers for the hand-built SVG instrument gauges (score /
 * speed). The gauges sweep a 270-degree arc (like a real automotive
 * dial, not a plain half-circle) starting at 225deg and ending at
 * -45deg (i.e. 495deg), going clockwise through the bottom.
 */
export const GAUGE_START_ANGLE = -220; // degrees, measured from 3 o'clock, counter-clockwise start
export const GAUGE_END_ANGLE = 40; // sweep clockwise to here

const toRad = (deg) => (deg * Math.PI) / 180;

export function polarToCartesian(cx, cy, radius, angleDeg) {
  const rad = toRad(angleDeg);
  return {
    x: cx + radius * Math.cos(rad),
    y: cy + radius * Math.sin(rad),
  };
}

/** Builds an SVG arc path between two angles (degrees) on a given radius. */
export function describeArc(cx, cy, radius, startAngle, endAngle) {
  const start = polarToCartesian(cx, cy, radius, startAngle);
  const end = polarToCartesian(cx, cy, radius, endAngle);
  const largeArcFlag = endAngle - startAngle <= 180 ? 0 : 1;
  return `M ${start.x} ${start.y} A ${radius} ${radius} 0 ${largeArcFlag} 1 ${end.x} ${end.y}`;
}

/** Maps a value within [min, max] to an angle within the gauge sweep. */
export function valueToAngle(value, min, max) {
  const clamped = Math.min(Math.max(value, min), max);
  const fraction = (clamped - min) / (max - min);
  return GAUGE_START_ANGLE + fraction * (GAUGE_END_ANGLE - GAUGE_START_ANGLE);
}

/** Maps a [0,1] zone boundary fraction (relative to min/max) to a gauge angle. */
export function fractionToAngle(fraction) {
  return GAUGE_START_ANGLE + fraction * (GAUGE_END_ANGLE - GAUGE_START_ANGLE);
}
