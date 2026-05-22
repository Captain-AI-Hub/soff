interface Props {
  onOpen: () => void;
}

export function EmptyState({ onOpen }: Props) {
  return (
    <div className="flex-1 flex items-center justify-center">
      <div className="flex flex-col items-center animate-fade-in">
        <svg width="64" height="64" viewBox="0 0 24 24" fill="none" className="opacity-30 mb-6">
          <path d="M9 5H7a2 2 0 00-2 2v12a2 2 0 002 2h10a2 2 0 002-2V7a2 2 0 00-2-2h-2" stroke="currentColor" strokeWidth="1.5"/>
          <rect x="9" y="3" width="6" height="4" rx="1" stroke="currentColor" strokeWidth="1.5"/>
          <path d="M9 12h6M9 16h4" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round"/>
        </svg>
        <h1 className="text-xl font-light text-[var(--text-secondary)] mb-1">Soff</h1>
        <p className="text-sm text-[var(--text-muted)] mb-6">Binary Diff Viewer</p>
        <button
          onClick={onOpen}
          className="px-5 py-2.5 bg-[var(--accent)] text-white text-sm font-medium rounded-lg
                     hover:bg-[var(--accent-hover)] transition-all duration-200
                     shadow-[0_0_20px_rgba(122,162,247,0.2)] hover:shadow-[0_0_30px_rgba(122,162,247,0.3)]"
        >
          Open .soff File
        </button>
        <p className="mt-4 text-xs text-[var(--text-muted)]">
          or drag and drop a file here
        </p>
      </div>
    </div>
  );
}
