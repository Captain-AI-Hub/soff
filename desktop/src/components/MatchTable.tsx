import { useRef, useState, useCallback, useEffect } from "react";
import type { DiffMatch } from "../App";

interface Props {
  matches: DiffMatch[];
  selected: DiffMatch | null;
  onSelect: (m: DiffMatch) => void;
  totalCount: number;
  loadingMore: boolean;
  onLoadMore: () => void;
}

const ROW_HEIGHT = 34;
const OVERSCAN = 5;

function ratioColor(ratio: number): string {
  if (ratio >= 0.95) return "var(--green)";
  if (ratio >= 0.7) return "var(--yellow)";
  if (ratio >= 0.5) return "var(--orange)";
  return "var(--red)";
}

function isUnmatchedRow(m: DiffMatch): boolean {
  return m.match_type === "primary" || m.match_type === "secondary";
}

function typeBadge(m: DiffMatch): { cls: string; label: string } {
  switch (m.match_type) {
    case "best":
      return { cls: "bg-emerald-500/10 text-emerald-400 border-emerald-500/25", label: "best" };
    case "partial":
      return { cls: "bg-amber-500/10 text-amber-400 border-amber-500/25", label: "partial" };
    case "unreliable":
      return { cls: "bg-rose-500/10 text-rose-400 border-rose-500/25", label: "unreliable" };
    case "multimatch":
      return { cls: "bg-violet-500/10 text-violet-400 border-violet-500/25", label: "multi" };
    case "primary":
      return { cls: "bg-slate-500/10 text-slate-400 border-slate-500/25", label: "◀ only" };
    case "secondary":
      return { cls: "bg-slate-500/10 text-slate-400 border-slate-500/25", label: "only ▶" };
    default:
      return { cls: "bg-slate-500/10 text-slate-400 border-slate-500/25", label: m.match_type };
  }
}

function rowTooltip(m: DiffMatch): string {
  if (isUnmatchedRow(m)) {
    return `${m.primary_addr || m.secondary_addr}  ${m.primary_name || m.secondary_name}`;
  }
  return `${m.primary_addr} → ${m.secondary_addr}  ·  ratio ${(m.ratio * 100).toFixed(1)}%  ·  ${m.description}`;
}

export function MatchTable({ matches, selected, onSelect, totalCount, loadingMore, onLoadMore }: Props) {
  const containerRef = useRef<HTMLDivElement>(null);
  const [scrollTop, setScrollTop] = useState(0);
  const [containerHeight, setContainerHeight] = useState(600);

  useEffect(() => {
    const el = containerRef.current;
    if (!el) return;
    const ro = new ResizeObserver(() => setContainerHeight(el.clientHeight));
    ro.observe(el);
    return () => ro.disconnect();
  }, []);

  const onScroll = useCallback(() => {
    const el = containerRef.current;
    if (!el) return;
    setScrollTop(el.scrollTop);
    if (!loadingMore && matches.length < totalCount && el.scrollTop + el.clientHeight >= el.scrollHeight - 640) {
      onLoadMore();
    }
  }, [loadingMore, matches.length, onLoadMore, totalCount]);

  // Keyboard navigation: ↑/↓ move the selection, Enter opens the graph view.
  const onKeyDown = useCallback((e: React.KeyboardEvent) => {
    if (matches.length === 0) return;
    if (e.key !== "ArrowDown" && e.key !== "ArrowUp" && e.key !== "Enter") return;
    e.preventDefault();
    const currentIdx = selected
      ? matches.findIndex((m) => m.primary_addr === selected.primary_addr && m.secondary_addr === selected.secondary_addr)
      : -1;
    if (e.key === "Enter") {
      if (currentIdx >= 0) onSelect(matches[currentIdx]);
      return;
    }
    const nextIdx = e.key === "ArrowDown"
      ? Math.min(matches.length - 1, currentIdx + 1)
      : Math.max(0, currentIdx === -1 ? 0 : currentIdx - 1);
    const el = containerRef.current;
    if (el) {
      const top = nextIdx * ROW_HEIGHT;
      if (top < el.scrollTop) el.scrollTop = top;
      else if (top + ROW_HEIGHT > el.scrollTop + el.clientHeight) el.scrollTop = top + ROW_HEIGHT - el.clientHeight;
    }
    onSelect(matches[nextIdx]);
  }, [matches, selected, onSelect]);

  const totalHeight = matches.length * ROW_HEIGHT;
  const startIdx = Math.max(0, Math.floor(scrollTop / ROW_HEIGHT) - OVERSCAN);
  const visibleCount = Math.ceil(containerHeight / ROW_HEIGHT) + OVERSCAN * 2;
  const endIdx = Math.min(matches.length, startIdx + visibleCount);

  return (
    <div className="flex-1 flex flex-col min-h-0">
      <div className="shrink-0 bg-[var(--bg-secondary)]/90 border-b border-[var(--border)]">
        <table className="w-full text-[12px] table-fixed">
          <thead>
            <tr className="text-[var(--text-muted)] text-[10px] uppercase tracking-wider">
              <th className="px-3 py-2.5 text-left font-medium w-[86px]">Type</th>
              <th className="px-3 py-2.5 text-left font-medium w-[25%]">Primary</th>
              <th className="px-3 py-2.5 text-left font-medium w-[25%]">Secondary</th>
              <th className="px-3 py-2.5 text-left font-medium w-[110px]">Ratio</th>
              <th className="px-3 py-2.5 text-center font-medium w-[70px]">Nodes</th>
              <th className="px-3 py-2.5 text-left font-medium">Heuristic</th>
            </tr>
          </thead>
        </table>
      </div>
      <div
        ref={containerRef}
        className="flex-1 overflow-auto min-h-0 focus:outline-none"
        onScroll={onScroll}
        tabIndex={0}
        onKeyDown={onKeyDown}
      >
        <div style={{ height: totalHeight, position: "relative" }}>
          <table className="w-full text-[12px] table-fixed absolute left-0" style={{ top: startIdx * ROW_HEIGHT }}>
            <tbody>
              {matches.slice(startIdx, endIdx).map((m, i) => {
                const sel = selected?.primary_addr === m.primary_addr && selected?.secondary_addr === m.secondary_addr;
                const badge = typeBadge(m);
                const unmatched = isUnmatchedRow(m);
                return (
                  <tr key={startIdx + i} onClick={() => onSelect(m)} style={{ height: ROW_HEIGHT }}
                    data-tip={rowTooltip(m)}
                    className={`row-hover cursor-pointer border-b border-[var(--border-subtle)] ${
                      sel ? "bg-[var(--accent-soft)] shadow-[inset_2px_0_0_var(--accent)]" : ""
                    }`}>
                    <td className="px-3 py-1 w-[86px]">
                      <span className={`inline-block px-1.5 py-0.5 text-[10px] font-medium rounded-md border ${badge.cls}`}>
                        {badge.label}
                      </span>
                    </td>
                    <td className="px-3 py-1 font-mono truncate overflow-hidden w-[25%]">{m.primary_name}</td>
                    <td className="px-3 py-1 font-mono truncate overflow-hidden text-[var(--text-secondary)] w-[25%]">{m.secondary_name}</td>
                    <td className="px-3 py-1 w-[110px]">
                      {unmatched ? (
                        <span className="text-[var(--text-muted)]/60 text-[11px] font-mono">—</span>
                      ) : (
                        <div className="flex items-center gap-2">
                          <span className="font-mono text-[11px] w-[30px] shrink-0" style={{ color: ratioColor(m.ratio) }}>{(m.ratio * 100).toFixed(0)}%</span>
                          <div className="ratio-bar flex-1"><div className="ratio-bar-fill" style={{ width: `${m.ratio * 100}%`, background: ratioColor(m.ratio) }} /></div>
                        </div>
                      )}
                    </td>
                    <td className="px-3 py-1 text-center font-mono text-[var(--text-muted)] w-[70px]">
                      {unmatched ? "" : `${m.nodes1}/${m.nodes2}`}
                    </td>
                    <td className="px-3 py-1 text-[var(--text-muted)] truncate overflow-hidden">{m.description}</td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        </div>
        {matches.length === 0 && (
          <div className="flex flex-col items-center justify-center h-40 gap-2 text-[var(--text-muted)]">
            <svg width="28" height="28" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.5" className="opacity-40">
              <circle cx="11" cy="11" r="7" /><path d="M21 21l-4.3-4.3" strokeLinecap="round" />
            </svg>
            <span className="text-sm">No matches found</span>
          </div>
        )}
      </div>
      <div className="shrink-0 h-7 px-3 flex items-center justify-between bg-[var(--bg-secondary)]/90 border-t border-[var(--border)] text-[10px] text-[var(--text-muted)] font-mono">
        <span>{matches.length.toLocaleString()} / {totalCount.toLocaleString()}</span>
        <span className="opacity-60">↑↓ navigate · Enter opens</span>
        <span>{loadingMore ? "Loading…" : matches.length < totalCount ? "Scroll for more" : "End"}</span>
      </div>
    </div>
  );
}
