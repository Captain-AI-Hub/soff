interface Props {
  onOpen: () => void;
}

export function EmptyState({ onOpen }: Props) {
  return (
    <div className="flex-1 flex items-center justify-center relative overflow-hidden">
      {/* Ambient background glow */}
      <div
        className="absolute inset-0 pointer-events-none"
        style={{
          background:
            "radial-gradient(600px 300px at 50% 35%, rgba(122, 162, 247, 0.08), transparent 70%)",
        }}
      />
      <div className="flex flex-col items-center animate-fade-in relative">
        <div
          className="w-16 h-16 rounded-2xl flex items-center justify-center text-2xl font-bold text-white mb-5 select-none"
          style={{
            background: "linear-gradient(135deg, #7aa2f7, #bb9af7)",
            boxShadow: "0 8px 40px rgba(122, 162, 247, 0.4), inset 0 1px 0 rgba(255,255,255,0.25)",
          }}
        >
          S
        </div>
        <h1 className="text-2xl font-light text-[var(--text-primary)] tracking-wide mb-1">Soff</h1>
        <p className="text-[13px] text-[var(--text-muted)] mb-8">Binary Diff Viewer</p>

        <button onClick={onOpen} className="btn-primary px-6 py-2.5 text-sm font-medium rounded-xl">
          Open .soff File
        </button>
        <p className="mt-4 text-[11px] text-[var(--text-muted)]">
          or drop a result file anywhere in this window
        </p>

        <div className="flex gap-6 mt-10 text-[10px] text-[var(--text-muted)]">
          <Feature icon="◈" label="CFG diff" />
          <Feature icon="⇔" label="Pseudo / ASM diff" />
          <Feature icon="⚡" label="MCP server" />
        </div>
      </div>
    </div>
  );
}

function Feature({ icon, label }: { icon: string; label: string }) {
  return (
    <span className="flex items-center gap-1.5 px-3 py-1.5 rounded-full border border-[var(--border)] bg-[var(--bg-surface)]/60">
      <span className="text-[var(--accent)]">{icon}</span>
      {label}
    </span>
  );
}
