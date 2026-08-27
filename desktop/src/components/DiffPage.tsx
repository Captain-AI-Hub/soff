import { useState, useRef, useEffect } from "react";
import { invoke, Channel } from "@tauri-apps/api/core";
import { open, save } from "@tauri-apps/plugin-dialog";

interface DiffProgress {
  phase: string;
  index?: number;
  total?: number;
  matches?: number;
  name?: string;
  step?: string;
  best?: number;
  partial?: number;
  unreliable?: number;
  unmatched_primary?: number;
  unmatched_secondary?: number;
  out?: string;
}

interface Props {
  onDiffComplete: (path: string) => void;
}

export function DiffPage({ onDiffComplete }: Props) {
  const [primaryDb, setPrimaryDb] = useState("");
  const [secondaryDb, setSecondaryDb] = useState("");
  const [slow, setSlow] = useState(true);
  const [unreliable, setUnreliable] = useState(false);
  const [running, setRunning] = useState(false);
  const [progress, setProgress] = useState<DiffProgress | null>(null);
  const [error, setError] = useState("");
  const [logs, setLogs] = useState<string[]>([]);
  const [doneResult, setDoneResult] = useState<DiffProgress | null>(null);
  const startTime = useRef<number>(0);
  const [elapsed, setElapsed] = useState(0);
  const timerRef = useRef<ReturnType<typeof setInterval> | null>(null);
  const logEndRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    logEndRef.current?.scrollIntoView({ block: "end" });
  }, [logs.length]);

  const pickFile = async (setter: (v: string) => void) => {
    const f = await open({ filters: [{ name: "SQLite", extensions: ["sqlite", "db"] }] });
    if (f) setter(f);
  };

  const addLog = (msg: string) => {
    setLogs(prev => [...prev.slice(-200), msg]);
  };

  const runDiff = async () => {
    if (!primaryDb || !secondaryDb) return;
    setError("");
    setRunning(true);
    setDoneResult(null);
    setLogs([]);
    setProgress({ phase: "starting" });
    startTime.current = Date.now();
    timerRef.current = setInterval(() => setElapsed(Date.now() - startTime.current), 200);

    const outputPath = await save({
      defaultPath: "result.soff",
      filters: [{ name: "Soff Result", extensions: ["soff"] }],
    });
    if (!outputPath) {
      setRunning(false); setProgress(null);
      if (timerRef.current) clearInterval(timerRef.current);
      return;
    }

    addLog(`Primary:   ${primaryDb}`);
    addLog(`Secondary: ${secondaryDb}`);
    addLog(`Output:    ${outputPath}`);
    addLog(`Options:   slow=${slow} unreliable=${unreliable}`);
    addLog("─".repeat(60));

    const channel = new Channel<string>();
    channel.onmessage = (line: string) => {
      try {
        const p: DiffProgress = JSON.parse(line);
        setProgress(p);
        if (p.phase === "heuristic") {
          addLog(`[${String(p.index).padStart(2, " ")}/${p.total}] ${p.name} → ${p.matches} matches`);
        } else if (p.phase === "validate") {
          addLog(`Validating ${p.step} database…`);
        } else if (p.phase === "running") {
          addLog("Loading function data…");
        } else if (p.phase === "done") {
          setDoneResult(p);
          addLog(`─`.repeat(60));
          addLog(`Done: best=${p.best} partial=${p.partial} unreliable=${p.unreliable}`);
        }
      } catch {}
    };

    try {
      await invoke<string>("run_diff", {
        primaryDb, secondaryDb, outputPath, slow, unreliable, channel,
      });
      setRunning(false);
      if (timerRef.current) clearInterval(timerRef.current);
      setElapsed(Date.now() - startTime.current);
      onDiffComplete(outputPath);
    } catch (e: unknown) {
      setRunning(false);
      if (timerRef.current) clearInterval(timerRef.current);
      setError(String(e));
      addLog(`ERROR: ${String(e)}`);
    }
  };

  const progressPercent = progress?.total && progress?.index
    ? Math.round((progress.index / progress.total) * 100) : 0;
  const elapsedStr = (elapsed / 1000).toFixed(1) + "s";

  return (
    <div className="flex-1 flex flex-col p-6 gap-4 overflow-hidden animate-fade-in">
      <div className="flex items-center gap-3">
        <h2 className="text-base font-medium text-[var(--text-primary)]">Diff SQLite Databases</h2>
        {(running || doneResult) && (
          <span className="text-[11px] font-mono text-[var(--text-muted)] px-2 py-0.5 rounded-md bg-[var(--bg-surface)] border border-[var(--border)]">
            {elapsedStr}
          </span>
        )}
      </div>

      <div className="card p-4 space-y-2.5">
        <FileInput label="Primary" value={primaryDb} onPick={() => pickFile(setPrimaryDb)} disabled={running} accent="#7aa2f7" />
        <FileInput label="Secondary" value={secondaryDb} onPick={() => pickFile(setSecondaryDb)} disabled={running} accent="#e0af68" />
      </div>

      <div className="flex items-center gap-4">
        <Toggle checked={slow} onChange={setSlow} disabled={running} label="Slow heuristics" hint="Fuzzy hashing, graph comparison" />
        <Toggle checked={unreliable} onChange={setUnreliable} disabled={running} label="Unreliable" hint="Low-confidence algorithms" />
        <button
          onClick={runDiff}
          disabled={running || !primaryDb || !secondaryDb}
          className="btn-primary ml-auto px-6 py-2 rounded-xl font-medium text-[12px]"
        >
          {running ? (
            <span className="flex items-center gap-2">
              <span className="w-3 h-3 rounded-full border-2 border-white/40 border-t-white animate-spin" />
              Running…
            </span>
          ) : "Start Diff"}
        </button>
      </div>

      {running && progress && (
        <div className="space-y-1.5 animate-fade-in">
          <div className="h-2 rounded-full bg-[var(--bg-surface)] overflow-hidden border border-[var(--border)]">
            <div
              className="h-full transition-all duration-300"
              style={{
                width: `${progressPercent}%`,
                background: "linear-gradient(90deg, var(--accent), #bb9af7)",
                boxShadow: "0 0 10px rgba(122, 162, 247, 0.5)",
              }}
            />
          </div>
          <div className="flex justify-between text-[10px] text-[var(--text-muted)] font-mono">
            <span>{progress.phase === "heuristic" ? progress.name : progress.phase === "validate" ? `Validating ${progress.step}…` : "Loading…"}</span>
            <span>{progress.phase === "heuristic" && `${progress.index}/${progress.total} · ${progress.matches} matches`}</span>
          </div>
        </div>
      )}

      {!running && doneResult && (
        <div className="flex gap-4 text-[11px] font-mono px-4 py-2.5 rounded-xl bg-emerald-500/5 border border-emerald-500/20 animate-fade-in">
          <span className="text-emerald-400 font-semibold">✓ Done</span>
          <span className="text-[var(--text-secondary)]">best={doneResult.best}</span>
          <span className="text-[var(--text-secondary)]">partial={doneResult.partial}</span>
          <span className="text-[var(--text-secondary)]">unreliable={doneResult.unreliable}</span>
          <span className="text-[var(--text-muted)]">unmatched={doneResult.unmatched_primary}/{doneResult.unmatched_secondary}</span>
          <span className="text-[var(--text-muted)] ml-auto">{elapsedStr}</span>
        </div>
      )}

      {error && (
        <p className="text-[11px] text-red-400 font-mono px-4 py-2.5 rounded-xl bg-red-500/5 border border-red-500/20 animate-fade-in">
          {error}
        </p>
      )}

      {logs.length > 0 && (
        <div className="console-panel flex-1 min-h-0 flex flex-col overflow-hidden">
          <div className="flex items-center gap-1.5 px-3 py-2 border-b border-[var(--border)] shrink-0">
            <span className="w-2 h-2 rounded-full bg-rose-500/60" />
            <span className="w-2 h-2 rounded-full bg-amber-500/60" />
            <span className="w-2 h-2 rounded-full bg-emerald-500/60" />
            <span className="ml-2 text-[9px] uppercase tracking-wider text-[var(--text-muted)]">console</span>
          </div>
          <div className="flex-1 overflow-auto p-3">
            <div className="font-mono text-[10.5px] leading-[18px] text-[var(--text-muted)]">
              {logs.map((l, i) => (
                <div key={i} className={l.startsWith("ERROR") ? "text-red-400" : l.startsWith("─") ? "opacity-30" : l.startsWith("Done:") ? "text-emerald-400" : ""}>{l}</div>
              ))}
              <div ref={logEndRef} />
            </div>
          </div>
        </div>
      )}
    </div>
  );
}

function Toggle({ checked, onChange, disabled, label, hint }: {
  checked: boolean;
  onChange: (v: boolean) => void;
  disabled: boolean;
  label: string;
  hint: string;
}) {
  return (
    <label
      data-tip={hint}
      className={`flex items-center gap-2 text-[11px] text-[var(--text-secondary)] select-none ${disabled ? "opacity-50 cursor-not-allowed" : "cursor-pointer"}`}
    >
      <button
        type="button"
        role="switch"
        aria-checked={checked}
        disabled={disabled}
        onClick={() => onChange(!checked)}
        className={`w-8 h-[18px] rounded-full relative transition-colors duration-150 ${
          checked ? "bg-[var(--accent)]" : "bg-[var(--bg-active)]"
        }`}
      >
        <span
          className="absolute top-[2px] w-[14px] h-[14px] rounded-full bg-white transition-all duration-150"
          style={{ left: checked ? 16 : 2, boxShadow: "0 1px 3px rgba(0,0,0,0.4)" }}
        />
      </button>
      {label}
    </label>
  );
}

function FileInput({ label, value, onPick, disabled, accent }: {
  label: string;
  value: string;
  onPick: () => void;
  disabled?: boolean;
  accent: string;
}) {
  const filename = value ? value.split(/[/\\]/).pop() : "";
  return (
    <div className="flex items-center gap-3">
      <span className="text-[10px] uppercase tracking-wider w-20 shrink-0 font-medium" style={{ color: accent }}>{label}</span>
      <button
        onClick={onPick}
        disabled={disabled}
        title={value}
        className="flex-1 flex items-center gap-2.5 text-left px-3 py-2 rounded-lg bg-[var(--bg-primary)] border border-[var(--border)]
                   hover:border-[var(--accent)] disabled:opacity-50 disabled:cursor-not-allowed transition-colors group"
      >
        <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.6"
          className={value ? "text-[var(--accent)]" : "text-[var(--text-muted)] group-hover:text-[var(--accent)]"} strokeLinecap="round" strokeLinejoin="round">
          <path d="M4 4v16a1 1 0 001 1h14a1 1 0 001-1V9l-6-5H5a1 1 0 00-1 1z" />
          <path d="M14 4v5h6" strokeOpacity="0.6" />
        </svg>
        <span className={`text-[11px] font-mono truncate ${value ? "text-[var(--text-primary)]" : "text-[var(--text-muted)]"}`}>
          {filename || "Click to select a .sqlite export…"}
        </span>
        {value && <span className="ml-auto text-[9px] text-[var(--text-muted)]/70 truncate max-w-[40%]">{value}</span>}
      </button>
    </div>
  );
}
