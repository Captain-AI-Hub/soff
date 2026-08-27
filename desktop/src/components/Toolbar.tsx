import type { SoffConfig } from "../App";

export interface FilterCounts {
  all: number;
  best: number;
  partial: number;
  unreliable: number;
  unmatched: number;
}

interface Props {
  config: SoffConfig | null;
  filter: string;
  onFilter: (f: string) => void;
  searchQuery: string;
  onSearch: (q: string) => void;
  onOpen: () => void;
  counts: FilterCounts | null;
}

const filters: { key: string; label: string; countKey?: keyof FilterCounts }[] = [
  { key: "all", label: "All", countKey: "all" },
  { key: "best", label: "Best", countKey: "best" },
  { key: "partial", label: "Partial", countKey: "partial" },
  { key: "unreliable", label: "Unreliable", countKey: "unreliable" },
  { key: "unmatched", label: "Unmatched", countKey: "unmatched" },
];

export function Toolbar({ config, filter, onFilter, searchQuery, onSearch, onOpen, counts }: Props) {
  return (
    <div className="shrink-0 bg-[var(--bg-secondary)]/90 border-b border-[var(--border)] select-none">
      <div className="flex items-center h-12 px-3 gap-3">
        {/* Open another result */}
        <button
          onClick={onOpen}
          data-tip="Open .soff result"
          className="w-8 h-8 shrink-0 flex items-center justify-center rounded-lg text-[var(--text-muted)]
                     hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)] transition-colors"
        >
          <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.7" strokeLinecap="round" strokeLinejoin="round">
            <path d="M3 7a2 2 0 012-2h4l2 2h8a2 2 0 012 2v9a2 2 0 01-2 2H5a2 2 0 01-2-2V7z" />
          </svg>
        </button>

        {config && (
          <>
            <div className="w-px h-6 bg-[var(--border)]" />

            {/* Filter tabs with counts */}
            <div className="flex gap-1 p-0.5 rounded-lg bg-[var(--bg-primary)] border border-[var(--border)]">
              {filters.map((f) => {
                const active = filter === f.key;
                const count = counts && f.countKey ? counts[f.countKey] : null;
                return (
                  <button
                    key={f.key}
                    onClick={() => onFilter(f.key)}
                    className={`flex items-center gap-1.5 px-2.5 py-1.5 text-[11px] font-medium rounded-md transition-all duration-100 ${
                      active
                        ? "bg-[var(--accent)] text-white shadow-sm"
                        : "text-[var(--text-muted)] hover:text-[var(--text-secondary)] hover:bg-[var(--bg-surface)]"
                    }`}
                  >
                    {f.label}
                    {count !== null && (
                      <span className={`font-mono text-[9px] px-1 rounded ${
                        active ? "bg-white/20 text-white" : "bg-[var(--bg-active)] text-[var(--text-muted)]"
                      }`}>
                        {count.toLocaleString()}
                      </span>
                    )}
                  </button>
                );
              })}
            </div>

            {/* Search with icon + clear */}
            <div className="relative">
              <svg
                width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2"
                className="absolute left-2.5 top-1/2 -translate-y-1/2 text-[var(--text-muted)] pointer-events-none"
                strokeLinecap="round" strokeLinejoin="round"
              >
                <circle cx="11" cy="11" r="7" />
                <path d="M21 21l-4.3-4.3" />
              </svg>
              <input
                type="text"
                value={searchQuery}
                onChange={(e) => onSearch(e.target.value)}
                onKeyDown={(e) => { if (e.key === "Escape") onSearch(""); }}
                placeholder="Search function..."
                className="pl-7 pr-7 py-1.5 text-xs w-56 bg-[var(--bg-primary)] border border-[var(--border)]
                           rounded-lg text-[var(--text-primary)] placeholder:text-[var(--text-muted)]
                           focus:outline-none focus:border-[var(--accent)] focus:ring-2 focus:ring-[var(--accent-soft)] transition-all"
              />
              {searchQuery && (
                <button
                  onClick={() => onSearch("")}
                  aria-label="Clear search"
                  className="absolute right-1.5 top-1/2 -translate-y-1/2 w-[18px] h-[18px] flex items-center justify-center
                             rounded text-[var(--text-muted)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)]"
                >
                  <svg width="10" height="10" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2.5" strokeLinecap="round">
                    <path d="M6 6l12 12M18 6L6 18" />
                  </svg>
                </button>
              )}
            </div>

            {/* Right side stats */}
            <div className="ml-auto flex items-center gap-4 text-xs">
              <Stat label="Matched" value={config.total_matches} color="var(--green)" />
              <Stat label="Unmatched" value={config.total_unmatched} color="var(--yellow)" />
              <span className="text-[var(--text-muted)] pl-4 border-l border-[var(--border)] hidden xl:inline">
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
      <span className="font-mono font-semibold" style={{ color, textShadow: `0 0 12px ${color}40` }}>
        {value.toLocaleString()}
      </span>
    </span>
  );
}
