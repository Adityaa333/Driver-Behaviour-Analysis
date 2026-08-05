const COLOR_MAP = {
  success: 'bg-[var(--color-success)]',
  warning: 'bg-[var(--color-warning)]',
  danger: 'bg-[var(--color-danger)]',
  muted: 'bg-[var(--color-text-muted)]',
  accent: 'bg-[var(--color-accent)]',
};

/**
 * A small dot with an optional expanding ring - the vocabulary used
 * everywhere in the top nav / status panel to mean "this is live."
 */
export default function StatusDot({ tone = 'muted', pulse = false, size = 'sm' }) {
  const dimension = size === 'sm' ? 'w-2 h-2' : 'w-2.5 h-2.5';
  return (
    <span className="relative inline-flex items-center justify-center">
      {pulse && (
        <span
          className={`absolute inline-flex ${dimension} rounded-full ${COLOR_MAP[tone]} pulse-ring`}
        />
      )}
      <span className={`relative inline-flex ${dimension} rounded-full ${COLOR_MAP[tone]}`} />
    </span>
  );
}
