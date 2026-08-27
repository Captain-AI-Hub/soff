import type { Page } from "../App";

interface Props {
  page: Page;
  onPageChange: (p: Page) => void;
  hasData: boolean;
}

const items: { key: Page; icon: React.ReactNode; label: string; hint: string }[] = [
  {
    key: "analyze",
    label: "Analyze",
    hint: "Overview, statistics and MCP server",
    icon: (
      <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round" strokeLinejoin="round">
        <path d="M3 3v18h18" strokeOpacity="0.55" />
        <path d="M7 15l4-5 3 3 5-7" />
      </svg>
    ),
  },
  {
    key: "soff",
    label: "Matches",
    hint: "Matched function pairs",
    icon: (
      <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round" strokeLinejoin="round">
        <path d="M8 6h13M8 12h13M8 18h13" strokeOpacity="0.55" />
        <path d="M3.5 6l1 1 2-2M3.5 12l1 1 2-2M3.5 18l1 1 2-2" />
      </svg>
    ),
  },
  {
    key: "graph",
    label: "Graph",
    hint: "Side-by-side diff and CFG view",
    icon: (
      <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round" strokeLinejoin="round">
        <rect x="3" y="3" width="7" height="6" rx="1.5" />
        <rect x="14" y="15" width="7" height="6" rx="1.5" />
        <path d="M10 6h5a2 2 0 012 2v7" strokeOpacity="0.7" />
      </svg>
    ),
  },
  {
    key: "diff",
    label: "Diff",
    hint: "Run a new diff from two exports",
    icon: (
      <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round" strokeLinejoin="round">
        <path d="M12 3v18" strokeOpacity="0.4" strokeDasharray="2 2" />
        <path d="M7 8l-3 4 3 4M17 8l3 4-3 4" />
      </svg>
    ),
  },
];

export function Sidebar({ page, onPageChange, hasData }: Props) {
  return (
    <div className="w-14 shrink-0 bg-[var(--bg-secondary)]/80 border-r border-[var(--border)] flex flex-col items-center py-2.5 gap-1">
      {/* Brand mark */}
      <div
        className="w-9 h-9 mb-2 rounded-xl flex items-center justify-center text-[13px] font-bold text-white select-none"
        style={{
          background: "linear-gradient(135deg, #7aa2f7, #bb9af7)",
          boxShadow: "0 2px 12px rgba(122, 162, 247, 0.35)",
        }}
        data-tip="Soff — binary diff viewer"
        data-tip-right
        aria-label="Soff"
      >
        S
      </div>

      {items.map((item) => {
        const active = page === item.key;
        const disabled = item.key !== "soff" && item.key !== "diff" && !hasData;
        return (
          <button
            key={item.key}
            onClick={() => !disabled && onPageChange(item.key)}
            data-tip={item.hint}
            data-tip-right
            aria-label={item.label}
            className={`relative w-10 h-11 flex flex-col items-center justify-center gap-[3px] rounded-xl transition-all duration-150 ${
              active
                ? "bg-[var(--accent-soft)] text-[var(--accent)]"
                : disabled
                ? "text-[var(--text-muted)]/30 cursor-not-allowed"
                : "text-[var(--text-muted)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)]"
            }`}
          >
            {active && (
              <span className="absolute left-[-8px] top-1/2 -translate-y-1/2 w-[3px] h-5 rounded-r-full bg-[var(--accent)]"
                style={{ boxShadow: "0 0 8px var(--accent)" }} />
            )}
            {item.icon}
            <span className={`text-[8px] font-medium tracking-wide leading-none ${active ? "" : "opacity-70"}`}>
              {item.label}
            </span>
          </button>
        );
      })}

      <div className="mt-auto pb-1 text-[8px] font-mono text-[var(--text-muted)]/50 select-none rotate-180" style={{ writingMode: "vertical-rl" }}>
        soff
      </div>
    </div>
  );
}
