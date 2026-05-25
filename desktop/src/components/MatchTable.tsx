import { useRef, useState, useCallback, useEffect } from "react";
import type { DiffMatch } from "../App";

interface Props {
  matches: DiffMatch[];
  selected: DiffMatch | null;
  onSelect: (m: DiffMatch) => void;
}

const ROW_HEIGHT = 32;
const OVERSCAN = 5;

function ratioColor(ratio: number): string {
  if (ratio >= 0.95) return "var(--green)";
  if (ratio >= 0.7) return "var(--yellow)";
  if (ratio >= 0.5) return "var(--orange)";
  return "var(--red)";
}

function typeStyle(t: string) {
  switch (t) {
    case "best": return "bg-emerald-500/10 text-emerald-400 border-emerald-500/20";
    case "partial": return "bg-amber-500/10 text-amber-400 border-amber-500/20";
    default: return "bg-rose-500/10 text-rose-400 border-rose-500/20";
  }
}

export function MatchTable({ matches, selected, onSelect }: Props) {
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
    if (containerRef.current) setScrollTop(containerRef.current.scrollTop);
  }, []);

  const totalHeight = matches.length * ROW_HEIGHT;
  const startIdx = Math.max(0, Math.floor(scrollTop / ROW_HEIGHT) - OVERSCAN);
  const visibleCount = Math.ceil(containerHeight / ROW_HEIGHT) + OVERSCAN * 2;
  const endIdx = Math.min(matches.length, startIdx + visibleCount);

  return (
    <div className="flex-1 flex flex-col min-h-0">
      <div className="shrink-0 bg-[var(--bg-secondary)] border-b border-[var(--border)]">
        <table className="w-full text-[12px] table-fixed">
          <thead>
            <tr className="text-[var(--text-muted)] text-[10px] uppercase tracking-wider">
              <th className="px-3 py-2.5 text-left font-medium w-[70px]">Type</th>
              <th className="px-3 py-2.5 text-left font-medium w-[25%]">Primary</th>
              <th className="px-3 py-2.5 text-left font-medium w-[25%]">Secondary</th>
              <th className="px-3 py-2.5 text-left font-medium w-[100px]">Ratio</th>
              <th className="px-3 py-2.5 text-center font-medium w-[70px]">Nodes</th>
              <th className="px-3 py-2.5 text-left font-medium">Heuristic</th>
            </tr>
          </thead>
        </table>
      </div>
      <div ref={containerRef} className="flex-1 overflow-auto min-h-0" onScroll={onScroll}>
        <div style={{ height: totalHeight, position: "relative" }}>
          <table className="w-full text-[12px] table-fixed absolute left-0" style={{ top: startIdx * ROW_HEIGHT }}>
            <tbody>
              {matches.slice(startIdx, endIdx).map((m, i) => {
                const sel = selected?.primary_addr === m.primary_addr && selected?.secondary_addr === m.secondary_addr;
                return (
                  <tr key={startIdx + i} onClick={() => onSelect(m)} style={{ height: ROW_HEIGHT }}
                    className={`row-hover cursor-pointer border-b border-[var(--border-subtle)] ${sel ? "!bg-[var(--accent-glow)]" : ""}`}>
                    <td className="px-3 py-1 w-[70px]">
                      <span className={`inline-block px-1.5 py-0.5 text-[10px] font-medium rounded border ${typeStyle(m.match_type)}`}>{m.match_type}</span>
                    </td>
                    <td className="px-3 py-1 font-mono truncate overflow-hidden w-[25%]">{m.primary_name}</td>
                    <td className="px-3 py-1 font-mono truncate overflow-hidden text-[var(--text-secondary)] w-[25%]">{m.secondary_name}</td>
                    <td className="px-3 py-1 w-[100px]">
                      <div className="flex items-center gap-2">
                        <span className="font-mono text-[11px] w-[30px] shrink-0" style={{ color: ratioColor(m.ratio) }}>{(m.ratio * 100).toFixed(0)}%</span>
                        <div className="ratio-bar flex-1"><div className="ratio-bar-fill" style={{ width: `${m.ratio * 100}%`, background: ratioColor(m.ratio) }} /></div>
                      </div>
                    </td>
                    <td className="px-3 py-1 text-center font-mono text-[var(--text-muted)] w-[70px]">{m.nodes1}/{m.nodes2}</td>
                    <td className="px-3 py-1 text-[var(--text-muted)] truncate overflow-hidden">{m.description}</td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        </div>
        {matches.length === 0 && (
          <div className="flex items-center justify-center h-32 text-[var(--text-muted)] text-sm">No matches found</div>
        )}
      </div>
    </div>
  );
}
