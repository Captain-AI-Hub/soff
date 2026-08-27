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
      <div className="flex flex-col items-center animate-fade-in relative px-8">
        <div
          className="w-20 h-20 rounded-[24px] flex items-center justify-center text-3xl font-bold text-white select-none"
          style={{
            background: "linear-gradient(135deg, #7aa2f7, #bb9af7)",
            boxShadow: "0 8px 40px rgba(122, 162, 247, 0.4), inset 0 1px 0 rgba(255,255,255,0.25)",
          }}
        >
          S
        </div>

        <h1 className="mt-7 text-2xl font-light text-[var(--text-primary)] tracking-wide">Soff</h1>
        <p className="mt-2 text-[13px] text-[var(--text-secondary)]">Binary Diff Viewer</p>

        <button
          onClick={onOpen}
          className="btn-primary mt-10 px-10 py-3 text-sm font-medium rounded-full"
        >
          Open .soff File
        </button>
        <p className="mt-6 text-[11px] text-[var(--text-muted)]">
          or drop a result file anywhere in this window
        </p>

        <div className="flex gap-4 mt-14">
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
    <span className="flex items-center gap-2 px-4 py-2 rounded-full border border-[var(--border)] bg-[var(--bg-surface)] text-[11px] text-[var(--text-secondary)]">
      <span className="text-[var(--accent)] text-xs">{icon}</span>
      {label}
    </span>
  );
}
