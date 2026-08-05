export default function SectionHeader({ title, subtitle, icon: Icon }) {
  return (
    <div className="flex items-center gap-2.5 mb-3">
      {Icon && <Icon className="w-4 h-4 text-[var(--color-accent)]" />}
      <div>
        <h2 className="font-display text-sm font-semibold text-[var(--color-text-primary)] tracking-wide">
          {title}
        </h2>
        {subtitle && (
          <p className="text-[11px] text-[var(--color-text-muted)]">{subtitle}</p>
        )}
      </div>
    </div>
  );
}
