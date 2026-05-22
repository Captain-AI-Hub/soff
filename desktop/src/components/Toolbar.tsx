import type { SoffConfig } from "../App";

interface Props {
  onOpen: () => void;
  config: SoffConfig | null;
  filter: string;
  onFilter: (f: string) => void;
}

const filters = [
  { key: "all", label: "All" },
  { key: "best", label: "Best" },
  { key: "partial", label: "Partial" },
  { key: "unreliable", label: "Unreliable" },
];

export function Toolbar({ onOpen, config, filter, onFilter }: Props) {
  return (
    <div className="shrink-0 bg-[var(--bg-secondary)] border-b border-[var(--border)] select-none">
      <div className="flex items-center h-12 px-4 gap-4">
        {/* Open button */}
        <button
          onClick={onOpen}
          className="px-4 py-2 text-sm font-medium bg-[var(--bg-surface)] border border-[var(--border)]
                     hover:bg-[var(--bg-hover)] hover:border-[var(--text-muted)] rounded-md transition-all duration-150"
        >
          Open
        </button>

        {config && (
          <>
            {/* Separator */}
            <div className="w-px h-6 bg-[var(--border)]" />

            {/* Filter tabs */}
            <div className="flex gap-1">
              {filters.map((f) => (
                <button
                  key={f.key}
                  onClick={() => onFilter(f.key)}
                  className={`px-3 py-1.5 text-xs font-medium rounded-md transition-all duration-100 ${
                    filter === f.key
                      ? "bg-[var(--accent)] text-white"
                      : "text-[var(--text-muted)] hover:text-[var(--text-secondary)] hover:bg-[var(--bg-surface)]"
                  }`}
                >
                  {f.label}
                </button>
              ))}
            </div>

            {/* Right side stats */}
            <div className="ml-auto flex items-center gap-5 text-xs">
              <Stat label="Matched" value={config.total_matches} color="var(--green)" />
              <Stat label="Unmatched" value={config.total_unmatched} color="var(--yellow)" />
              <span className="text-[var(--text-muted)] pl-4 border-l border-[var(--border)]">
                {config.date}
              </span>
            </div>
          </>
        )}
      </div>
    </div>
  );
}

function Stat({ label, value, color }: { label: string; value: number; color: string }) {
  return (
    <span className="text-[var(--text-muted)]">
      {label}{" "}
      <span className="font-mono font-semibold" style={{ color }}>
        {value.toLocaleString()}
      </span>
    </span>
  );
}
