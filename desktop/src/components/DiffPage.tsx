import { useState, useRef } from "react";
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

  const pickFile = async (setter: (v: string) => void) => {
    const f = await open({ filters: [{ name: "SQLite", extensions: ["sqlite", "db"] }] });
    if (f) setter(f);
  };

  const addLog = (msg: string) => {
    setLogs(prev => [...prev.slice(-50), msg]);
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

    addLog(`Primary: ${primaryDb}`);
    addLog(`Secondary: ${secondaryDb}`);
    addLog(`Output: ${outputPath}`);
    addLog(`Options: slow=${slow} unreliable=${unreliable}`);
    addLog("---");

    const channel = new Channel<string>();
    channel.onmessage = (line: string) => {
      try {
        const p: DiffProgress = JSON.parse(line);
        setProgress(p);
        if (p.phase === "heuristic") {
          addLog(`[${p.index}/${p.total}] ${p.name} → ${p.matches} matches`);
        } else if (p.phase === "validate") {
          addLog(`Validating ${p.step} database...`);
        } else if (p.phase === "running") {
          addLog("Loading function data...");
        } else if (p.phase === "done") {
          setDoneResult(p);
          addLog(`--- Done: best=${p.best} partial=${p.partial} unreliable=${p.unreliable}`);
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
    <div className="flex-1 flex flex-col p-6 gap-4 overflow-hidden">
      <div className="flex items-center gap-3">
        <h2 className="text-base font-medium text-[var(--text-primary)]">Diff SQLite Databases</h2>
        {(running || doneResult) && (
          <span className="text-[11px] font-mono text-[var(--text-muted)] px-2 py-0.5 rounded bg-[var(--bg-surface)]">{elapsedStr}</span>
        )}
      </div>

      <div className="space-y-2">
        <FileInput label="Primary" value={primaryDb} onPick={() => pickFile(setPrimaryDb)} disabled={running} />
        <FileInput label="Secondary" value={secondaryDb} onPick={() => pickFile(setSecondaryDb)} disabled={running} />
      </div>

      <div className="flex items-center gap-4">
        <label className="flex items-center gap-1.5 text-[11px] text-[var(--text-secondary)] cursor-pointer">
          <input type="checkbox" checked={slow} onChange={e => setSlow(e.target.checked)} disabled={running} className="accent-[var(--accent)]" />
          Slow heuristics
        </label>
        <label className="flex items-center gap-1.5 text-[11px] text-[var(--text-secondary)] cursor-pointer">
          <input type="checkbox" checked={unreliable} onChange={e => setUnreliable(e.target.checked)} disabled={running} className="accent-[var(--accent)]" />
          Unreliable
        </label>
        <button onClick={runDiff} disabled={running || !primaryDb || !secondaryDb}
          className="ml-auto px-5 py-1.5 rounded-lg font-medium text-[12px] bg-[var(--accent)] text-white hover:brightness-110 disabled:opacity-40 disabled:cursor-not-allowed">
          {running ? "Running..." : "Start Diff"}
        </button>
      </div>

      {running && progress && (
        <div className="space-y-1">
          <div className="h-1.5 rounded-full bg-[var(--bg-surface)] overflow-hidden">
            <div className="h-full bg-[var(--accent)] transition-all duration-300" style={{ width: `${progressPercent}%` }} />
          </div>
          <div className="flex justify-between text-[10px] text-[var(--text-muted)] font-mono">
            <span>{progress.phase === "heuristic" ? progress.name : progress.phase === "validate" ? `Validating ${progress.step}...` : "Loading..."}</span>
            <span>{progress.phase === "heuristic" && `${progress.index}/${progress.total} · ${progress.matches} matches`}</span>
          </div>
        </div>
      )}

      {!running && doneResult && (
        <div className="flex gap-3 text-[11px] font-mono px-3 py-2 rounded-lg bg-emerald-500/5 border border-emerald-500/20">
          <span className="text-emerald-400">Done</span>
          <span className="text-[var(--text-secondary)]">best={doneResult.best}</span>
          <span className="text-[var(--text-secondary)]">partial={doneResult.partial}</span>
          <span className="text-[var(--text-secondary)]">unreliable={doneResult.unreliable}</span>
          <span className="text-[var(--text-muted)]">unmatched={doneResult.unmatched_primary}/{doneResult.unmatched_secondary}</span>
          <span className="text-[var(--text-muted)] ml-auto">{elapsedStr}</span>
        </div>
      )}

      {error && <p className="text-[11px] text-red-400 font-mono px-3 py-2 rounded-lg bg-red-500/5 border border-red-500/20">{error}</p>}

      {logs.length > 0 && (
        <div className="flex-1 min-h-0 overflow-auto rounded-lg bg-[var(--bg-primary)] border border-[var(--border)] p-3">
          <div className="font-mono text-[10px] text-[var(--text-muted)] space-y-0.5">
            {logs.map((l, i) => (
              <div key={i} className={l.startsWith("ERROR") ? "text-red-400" : l.startsWith("---") ? "opacity-40" : ""}>{l}</div>
            ))}
          </div>
        </div>
      )}
    </div>
  );
}

function FileInput({ label, value, onPick, disabled }: { label: string; value: string; onPick: () => void; disabled?: boolean }) {
  const filename = value ? value.split(/[/\\]/).pop() : "";
  return (
    <div className="flex items-center gap-2">
      <span className="text-[10px] text-[var(--text-muted)] uppercase tracking-wider w-20 shrink-0">{label}</span>
      <button onClick={onPick} disabled={disabled} title={value}
        className="flex-1 text-left px-3 py-1.5 rounded-lg bg-[var(--bg-surface)] border border-[var(--border)] text-[11px] font-mono text-[var(--text-secondary)] truncate hover:border-[var(--accent)] disabled:opacity-50 disabled:cursor-not-allowed">
        {filename || "Click to select..."}
      </button>
      {value && <span className="text-[9px] text-[var(--text-muted)] truncate max-w-[200px]" title={value}>{value}</span>}
    </div>
  );
}
