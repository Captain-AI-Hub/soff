import type { SoffConfig } from "../App";

interface Props {
  config: SoffConfig;
}

export function StatsPanel({ config }: Props) {
  return (
    <div className="p-3 text-xs space-y-2">
      <h3 className="text-[var(--text-secondary)] uppercase tracking-wider text-[10px]">Info</h3>
      <div className="space-y-1 text-[var(--text-muted)]">
        <p>Primary: <span className="text-[var(--text-primary)]">{config.main_db}</span></p>
        <p>Secondary: <span className="text-[var(--text-primary)]">{config.diff_db}</span></p>
        <p>Version: {config.version}</p>
        <p>Date: {config.date}</p>
      </div>
    </div>
  );
}
