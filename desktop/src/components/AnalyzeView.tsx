import { useEffect, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { open } from "@tauri-apps/plugin-dialog";
import type { McpStatus, SoffConfig } from "../App";

interface AnalyzeStats {
  best: number;
  partial: number;
  unreliable: number;
  unmatched_primary: number;
  unmatched_secondary: number;
  avg_ratio: number;
  total_nodes_primary: number;
  total_nodes_secondary: number;
  total_edges_primary: number;
  total_edges_secondary: number;
  ratio_distribution: number[];
}

interface Props {
  soffPath: string;
  config: SoffConfig;
  onConfigChange: (config: SoffConfig) => void;
  mcpStatus: McpStatus | null;
  mcpBusy: boolean;
  mcpError: string;
  mcpBindAddress: string;
  mcpPort: number;
  onMcpBindAddressChange: (value: string) => void;
  onMcpPortChange: (value: number) => void;
  onStartMcp: () => void;
  onStopMcp: () => void;
  onRefreshMcp: () => void;
}

function basename(path: string): string {
  return path.replace(/\\/g, "/").split("/").pop()?.replace(/\.(idb|i64|sqlite)$/i, "") || path;
}

export function AnalyzeView({
  soffPath,
  config,
  onConfigChange,
  mcpStatus,
  mcpBusy,
  mcpError,
  mcpBindAddress,
  mcpPort,
  onMcpBindAddressChange,
  onMcpPortChange,
  onStartMcp,
  onStopMcp,
  onRefreshMcp,
}: Props) {
  const [stats, setStats] = useState<AnalyzeStats | null>(null);
  const [mainDb, setMainDb] = useState(config.main_db);
  const [diffDb, setDiffDb] = useState(config.diff_db);
  const [savingPaths, setSavingPaths] = useState(false);
  const [pathError, setPathError] = useState("");
  useEffect(() => { invoke<AnalyzeStats>("get_analyze_stats", { path: soffPath }).then(setStats); }, [soffPath]);
  useEffect(() => {
    setMainDb(config.main_db);
    setDiffDb(config.diff_db);
    setPathError("");
  }, [config.main_db, config.diff_db]);

  const primaryName = basename(mainDb);
  const secondaryName = basename(diffDb);
  const pathsChanged = mainDb !== config.main_db || diffDb !== config.diff_db;

  const pickSqlite = async (setter: (path: string) => void) => {
    const path = await open({ filters: [{ name: "SQLite", extensions: ["sqlite", "db"] }] });
    if (path) setter(path);
  };

  const savePaths = async () => {
    if (!pathsChanged || savingPaths) return;
    setSavingPaths(true);
    setPathError("");
    try {
      const next = await invoke<SoffConfig>("update_soff_paths", {
        path: soffPath,
        mainDb,
        diffDb,
      });
      onConfigChange(next);
    } catch (e) {
      setPathError(String(e));
    } finally {
      setSavingPaths(false);
    }
  };

  if (!stats) return <div className="flex-1 flex items-center justify-center"><div className="animate-glow w-2 h-2 rounded-full bg-[var(--accent)]" /></div>;

  const totalMatched = stats.best + stats.partial + stats.unreliable;
  const totalFunctions = totalMatched + stats.unmatched_primary + stats.unmatched_secondary;
  const matchRate = totalFunctions > 0 ? totalMatched / totalFunctions : 0;

  return (
    <div className="flex-1 flex flex-col overflow-auto p-5 gap-5 animate-fade-in">
      {/* Header: program names */}
      <div className="flex items-center gap-4 px-2 shrink-0">
        <div className="flex items-center gap-2">
          <span className="w-3 h-3 rounded-full bg-[#7aa2f7]" />
          <span className="text-sm text-[var(--text-primary)] font-mono">{primaryName}</span>
          <span className="text-[10px] text-[var(--text-muted)]">Primary</span>
        </div>
        <span className="text-[var(--text-muted)]">vs</span>
        <div className="flex items-center gap-2">
          <span className="w-3 h-3 rounded-full bg-[#e0af68]" />
          <span className="text-sm text-[var(--text-primary)] font-mono">{secondaryName}</span>
          <span className="text-[10px] text-[var(--text-muted)]">Secondary</span>
        </div>
        <span className="ml-auto text-[11px] text-[var(--text-muted)]">{config.date}</span>
      </div>

      <div className="grid grid-cols-[1fr_1fr_auto] gap-2 px-2 shrink-0 items-center">
        <PathButton label="Primary DB" value={mainDb} onClick={() => pickSqlite(setMainDb)} />
        <PathButton label="Secondary DB" value={diffDb} onClick={() => pickSqlite(setDiffDb)} />
        <button
          onClick={savePaths}
          disabled={!pathsChanged || savingPaths}
          className="h-9 px-3 rounded-lg text-[11px] font-medium bg-[var(--bg-surface)] border border-[var(--border)] text-[var(--text-secondary)] hover:border-[var(--accent)] disabled:opacity-40 disabled:cursor-not-allowed"
        >
          {savingPaths ? "Saving..." : "Update paths"}
        </button>
        {pathError && <div className="col-span-3 text-[10px] text-red-400 font-mono">{pathError}</div>}
      </div>

      <McpPanel
        status={mcpStatus}
        busy={mcpBusy}
        error={mcpError}
        bindAddress={mcpBindAddress}
        port={mcpPort}
        onBindAddressChange={onMcpBindAddressChange}
        onPortChange={onMcpPortChange}
        onStart={onStartMcp}
        onStop={onStopMcp}
        onRefresh={onRefreshMcp}
      />

      <div className="flex gap-5 flex-1 min-h-0">
        <div className="flex flex-col items-center justify-center rounded-xl border border-[var(--border)] p-8 w-[260px] shrink-0 overflow-visible"
             style={{ background: "linear-gradient(145deg, var(--bg-surface), var(--bg-secondary))", boxShadow: "0 8px 32px rgba(0,0,0,0.4), inset 0 1px 0 rgba(255,255,255,0.05)" }}>
          <Donut3D value={matchRate} color="#9ece6a" size={150} />
          <span className="text-sm text-[var(--text-secondary)] mt-3">Match Rate</span>
          <span className="text-[11px] text-[var(--text-muted)]">{totalMatched} / {totalFunctions}</span>
        </div>
        <div className="flex-1 grid grid-cols-3 grid-rows-2 gap-3 min-h-0">
          <MetricCard label="Best" value={stats.best} color="#9ece6a" icon={"✓"} />
          <MetricCard label="Partial" value={stats.partial} color="#e0af68" icon={"◐"} />
          <MetricCard label="Unreliable" value={stats.unreliable} color="#f7768e" icon="?" />
          <MetricCard label="Unmatched Pri" value={stats.unmatched_primary} color="#ff9e64" icon={"←"} />
          <MetricCard label="Unmatched Sec" value={stats.unmatched_secondary} color="#ff9e64" icon={"→"} />
          <MetricCard label="Avg Similarity" value={Math.round(stats.avg_ratio * 100)} color="#7aa2f7" icon="%" isPercent />
        </div>
      </div>
      {/* PLACEHOLDER_BAR_CHART */}
      <div className="rounded-xl border border-[var(--border)] p-5 shrink-0"
           style={{ background: "linear-gradient(145deg, var(--bg-surface), var(--bg-secondary))", boxShadow: "0 4px 20px rgba(0,0,0,0.3), inset 0 1px 0 rgba(255,255,255,0.03)" }}>
        <h3 className="text-[10px] uppercase tracking-wider text-[var(--text-muted)] mb-4">Similarity Distribution</h3>
        <BarChart3D data={stats.ratio_distribution} />
      </div>
      <div className="grid grid-cols-2 gap-4 shrink-0">
        <CompareCard label="Basic Blocks" left={stats.total_nodes_primary} right={stats.total_nodes_secondary} />
        <CompareCard label="Total Functions" left={totalMatched + stats.unmatched_primary} right={totalMatched + stats.unmatched_secondary} />
      </div>
    </div>
  );
}

function McpPanel({
  status,
  busy,
  error,
  bindAddress,
  port,
  onBindAddressChange,
  onPortChange,
  onStart,
  onStop,
  onRefresh,
}: {
  status: McpStatus | null;
  busy: boolean;
  error: string;
  bindAddress: string;
  port: number;
  onBindAddressChange: (value: string) => void;
  onPortChange: (value: number) => void;
  onStart: () => void;
  onStop: () => void;
  onRefresh: () => void;
}) {
  const running = !!status?.running;
  const endpoint = status?.endpoint || previewEndpoint(bindAddress, port);
  const callCount = status?.call_count || 0;

  const toggle = () => {
    if (busy) return;
    if (running) onStop();
    else onStart();
  };

  return (
    <div className="mx-2 shrink-0 rounded-xl border border-[var(--border)] bg-[var(--bg-surface)] px-4 py-3">
      <div className="flex items-center gap-3 min-w-0">
        <div className="flex items-center gap-2 w-36 shrink-0">
          <span className={`w-2.5 h-2.5 rounded-full ${running ? "bg-[var(--green)]" : "bg-[var(--text-muted)]"}`} />
          <span className="text-[11px] uppercase tracking-wider text-[var(--text-muted)]">MCP</span>
          <span className={`text-xs font-medium ${running ? "text-[var(--green)]" : "text-[var(--text-secondary)]"}`}>
            {running ? "Running" : "Stopped"}
          </span>
        </div>

        <label className="flex items-center gap-2 text-[11px] text-[var(--text-secondary)] cursor-pointer select-none">
          <input
            type="checkbox"
            checked={running}
            disabled={busy}
            onChange={toggle}
            className="accent-[var(--accent)]"
          />
          {busy ? "Updating" : "Enabled"}
        </label>

        <select
          value={bindAddress}
          disabled={running || busy}
          onChange={(e) => onBindAddressChange(e.target.value)}
          className="h-8 px-2 rounded-md bg-[var(--bg-primary)] border border-[var(--border)] text-[11px] text-[var(--text-secondary)] focus:outline-none focus:border-[var(--accent)]"
        >
          <option value="127.0.0.1">127.0.0.1</option>
          <option value="0.0.0.0">0.0.0.0</option>
          <option value="::1">::1</option>
        </select>

        <input
          type="number"
          min={1}
          max={65535}
          value={port}
          disabled={running || busy}
          onChange={(e) => onPortChange(clampPort(e.target.value))}
          className="h-8 w-20 px-2 rounded-md bg-[var(--bg-primary)] border border-[var(--border)] text-[11px] font-mono text-[var(--text-secondary)] focus:outline-none focus:border-[var(--accent)]"
        />

        <div className="min-w-0 flex-1 font-mono text-[10px] text-[var(--text-muted)] truncate" title={endpoint}>
          {endpoint}
        </div>

        <div className="flex items-center gap-1.5 text-[11px] text-[var(--text-muted)] shrink-0">
          Calls
          <span className="font-mono text-[var(--text-primary)]">{callCount.toLocaleString()}</span>
        </div>

        <button
          onClick={onRefresh}
          disabled={busy}
          title="Refresh MCP status"
          className="h-8 px-3 rounded-md bg-[var(--bg-primary)] border border-[var(--border)] text-[11px] text-[var(--text-secondary)] hover:border-[var(--accent)] disabled:opacity-40"
        >
          Refresh
        </button>
      </div>
      {error && <div className="mt-2 font-mono text-[10px] text-red-400">{error}</div>}
    </div>
  );
}

function clampPort(value: string): number {
  const parsed = Number.parseInt(value, 10);
  if (Number.isNaN(parsed)) return 11339;
  return Math.min(65535, Math.max(1, parsed));
}

function previewEndpoint(bindAddress: string, port: number): string {
  return bindAddress.includes(":")
    ? `http://[${bindAddress}]:${port}/mcp/`
    : `http://${bindAddress}:${port}/mcp/`;
}

function PathButton({ label, value, onClick }: { label: string; value: string; onClick: () => void }) {
  return (
    <button
      onClick={onClick}
      title={value}
      className="h-9 min-w-0 text-left px-3 rounded-lg bg-[var(--bg-surface)] border border-[var(--border)] hover:border-[var(--accent)] transition-colors"
    >
      <span className="block text-[9px] uppercase tracking-wider text-[var(--text-muted)]">{label}</span>
      <span className="block text-[11px] font-mono text-[var(--text-secondary)] truncate">{value || "Select..."}</span>
    </button>
  );
}

function Donut3D({ value, color, size }: { value: number; color: string; size: number }) {
  const r = (size - 24) / 2;
  const circ = 2 * Math.PI * r;
  const offset = circ * (1 - Math.min(value, 1));
  const cx = size / 2, cy = size / 2;
  return (
    <svg width={size} height={size} viewBox={`0 0 ${size} ${size}`} className="overflow-visible">
      <defs>
        <filter id="glow">
          <feGaussianBlur stdDeviation="3" result="blur" />
          <feMerge><feMergeNode in="blur" /><feMergeNode in="SourceGraphic" /></feMerge>
        </filter>
      </defs>
      <circle cx={cx} cy={cy + 2} r={r} fill="none" stroke="rgba(0,0,0,0.4)" strokeWidth="14" />
      <circle cx={cx} cy={cy} r={r} fill="none" stroke="var(--bg-hover)" strokeWidth="12" />
      <circle cx={cx} cy={cy} r={r} fill="none" stroke={color} strokeWidth="12"
        strokeDasharray={circ} strokeDashoffset={offset} strokeLinecap="round"
        transform={`rotate(-90 ${cx} ${cy})`} filter="url(#glow)"
        style={{ transition: "stroke-dashoffset 1s ease" }} />
      <circle cx={cx} cy={cy} r={r} fill="none" stroke="rgba(255,255,255,0.12)" strokeWidth="4"
        strokeDasharray={circ} strokeDashoffset={offset} strokeLinecap="round"
        transform={`rotate(-90 ${cx} ${cy})`} style={{ transition: "stroke-dashoffset 1s ease" }} />
      <text x={cx} y={cy} textAnchor="middle" dominantBaseline="middle"
        fill="var(--text-primary)" fontSize="26" fontFamily="monospace" fontWeight="300">
        {(value * 100).toFixed(0)}%
      </text>
    </svg>
  );
}

function MetricCard({ label, value, color, icon, isPercent }: { label: string; value: number; color: string; icon: string; isPercent?: boolean }) {
  return (
    <div className="rounded-xl border border-[var(--border)] flex flex-col items-center justify-center text-center"
         style={{ background: "linear-gradient(145deg, var(--bg-surface), var(--bg-secondary))", boxShadow: "0 4px 16px rgba(0,0,0,0.25), inset 0 1px 0 rgba(255,255,255,0.04)" }}>
      <span className="text-sm opacity-25 mb-0.5">{icon}</span>
      <span className="text-2xl font-mono font-light" style={{ color, textShadow: `0 0 16px ${color}50` }}>
        {value.toLocaleString()}{isPercent ? "%" : ""}
      </span>
      <span className="text-[10px] text-[var(--text-muted)] mt-1">{label}</span>
    </div>
  );
}

function BarChart3D({ data }: { data: number[] }) {
  const max = Math.max(...data, 1);
  const labels = ["0-10","10-20","20-30","30-40","40-50","50-60","60-70","70-80","80-90","90-100"];
  const colors = ["#f7768e","#f7768e","#ff9e64","#ff9e64","#e0af68","#e0af68","#9ece6a","#9ece6a","#7aa2f7","#7aa2f7"];
  return (
    <div className="flex items-end gap-2 h-28" style={{ perspective: "800px" }}>
      {data.map((v, i) => {
        const pct = (v / max) * 100;
        return (
          <div key={i} className="flex-1 flex flex-col items-center gap-1 h-full">
            <span className="text-[10px] font-mono text-[var(--text-muted)]">{v || ""}</span>
            <div className="flex-1 w-full flex items-end justify-center" style={{ transform: "rotateX(8deg)", transformOrigin: "bottom" }}>
              <div className="w-full rounded-t relative overflow-hidden transition-all duration-700"
                style={{ height: `${pct}%`, minHeight: v > 0 ? "4px" : "0", background: `linear-gradient(180deg, ${colors[i]}, ${colors[i]}66)`, boxShadow: v > 0 ? `0 -2px 12px ${colors[i]}30, inset 0 1px 0 rgba(255,255,255,0.25), inset -2px 0 4px rgba(0,0,0,0.2)` : "none" }}>
                <div className="absolute inset-0 bg-gradient-to-r from-white/15 via-transparent to-black/10" />
              </div>
            </div>
            <span className="text-[8px] text-[var(--text-muted)]">{labels[i]}</span>
          </div>
        );
      })}
    </div>
  );
}

function CompareCard({ label, left, right }: { label: string; left: number; right: number }) {
  const total = left + right || 1;
  return (
    <div className="rounded-xl border border-[var(--border)] p-4"
         style={{ background: "linear-gradient(145deg, var(--bg-surface), var(--bg-secondary))", boxShadow: "0 4px 16px rgba(0,0,0,0.2)" }}>
      <h3 className="text-[10px] uppercase tracking-wider text-[var(--text-muted)] mb-2">{label}</h3>
      <div className="flex items-center justify-center gap-4 mb-2">
        <div className="flex items-center gap-1.5">
          <span className="w-2 h-2 rounded-full bg-[#7aa2f7]" />
          <span className="font-mono text-sm text-[#7aa2f7]">{left.toLocaleString()}</span>
          <span className="text-[9px] text-[var(--text-muted)]">Primary</span>
        </div>
        <div className="w-px h-4 bg-[var(--border)]" />
        <div className="flex items-center gap-1.5">
          <span className="w-2 h-2 rounded-full bg-[#e0af68]" />
          <span className="font-mono text-sm text-[#e0af68]">{right.toLocaleString()}</span>
          <span className="text-[9px] text-[var(--text-muted)]">Secondary</span>
        </div>
      </div>
      <div className="h-2.5 rounded-full overflow-hidden flex" style={{ background: "var(--bg-hover)", boxShadow: "inset 0 2px 4px rgba(0,0,0,0.5)" }}>
        <div className="h-full rounded-l-full transition-all duration-700"
             style={{ width: `${(left / total) * 100}%`, background: "linear-gradient(90deg, #7aa2f7, #7aa2f7dd)", boxShadow: "0 0 8px #7aa2f744" }} />
        <div className="h-full rounded-r-full transition-all duration-700"
             style={{ width: `${(right / total) * 100}%`, background: "linear-gradient(90deg, #e0af68dd, #e0af68)", boxShadow: "0 0 8px #e0af6844" }} />
      </div>
    </div>
  );
}
