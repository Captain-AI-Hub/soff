import type { SoffConfig } from "../App";

interface Props {
  onOpen: () => void;
  config: SoffConfig | null;
  filter: string;
  onFilter: (f: string) => void;
  searchQuery: string;
  onSearch: (q: string) => void;
}

const filters = [
  { key: "all", label: "All" },
  { key: "best", label: "Best" },
  { key: "partial", label: "Partial" },
  { key: "unreliable", label: "Unreliable" },
  { key: "unmatched", label: "Unmatched" },
];

export function Toolbar({ onOpen, config, filter, onFilter, searchQuery, onSearch }: Props) {
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

            <div className="w-px h-6 bg-[var(--border)]" />

            {/* Search */}
            <input
              type="text"
              value={searchQuery}
              onChange={(e) => onSearch(e.target.value)}
              placeholder="Search function..."
              className="px-3 py-1.5 text-xs w-52 bg-[var(--bg-primary)] border border-[var(--border)]
                         rounded-md text-[var(--text-primary)] placeholder:text-[var(--text-muted)]
                         focus:outline-none focus:border-[var(--accent)] transition-colors"
            />

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
