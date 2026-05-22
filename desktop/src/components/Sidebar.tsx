import type { Page } from "../App";

interface Props {
  page: Page;
  onPageChange: (p: Page) => void;
  hasData: boolean;
}

const items: { key: Page; icon: React.ReactNode; label: string }[] = [
  {
    key: "analyze",
    label: "Analyze",
    icon: (
      <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.5">
        <circle cx="12" cy="12" r="9" />
        <path d="M12 3v9l6.36 3.64" />
        <path d="M12 12L5.64 15.64" strokeOpacity="0.5" />
      </svg>
    ),
  },
  {
    key: "soff",
    label: "Soff",
    icon: (
      <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.5">
        <path d="M9 5H7a2 2 0 00-2 2v12a2 2 0 002 2h10a2 2 0 002-2V7a2 2 0 00-2-2h-2" />
        <rect x="9" y="3" width="6" height="4" rx="1" />
        <path d="M9 12h6M9 16h4" strokeLinecap="round" />
      </svg>
    ),
  },
  {
    key: "graph",
    label: "Graph",
    icon: (
      <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.5">
        <rect x="3" y="3" width="7" height="5" rx="1" />
        <rect x="14" y="3" width="7" height="5" rx="1" />
        <rect x="3" y="16" width="7" height="5" rx="1" />
        <rect x="14" y="16" width="7" height="5" rx="1" />
        <path d="M6.5 8v3.5h11V8M6.5 11.5V16M17.5 11.5V16" />
      </svg>
    ),
  },
];

export function Sidebar({ page, onPageChange, hasData }: Props) {
  return (
    <div className="w-12 shrink-0 bg-[var(--bg-secondary)] border-r border-[var(--border)] flex flex-col items-center py-2 gap-1">
      {items.map((item) => {
        const active = page === item.key;
        const disabled = item.key !== "soff" && !hasData;
        return (
          <button
            key={item.key}
            onClick={() => !disabled && onPageChange(item.key)}
            title={item.label}
            className={`w-9 h-9 flex items-center justify-center rounded-lg transition-all duration-100 ${
              active
                ? "bg-[var(--accent)] text-white"
                : disabled
                ? "text-[var(--text-muted)]/30 cursor-not-allowed"
                : "text-[var(--text-muted)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)]"
            }`}
          >
            {item.icon}
          </button>
        );
      })}
    </div>
  );
}
