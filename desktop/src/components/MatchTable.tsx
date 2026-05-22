import type { DiffMatch } from "../App";

interface Props {
  matches: DiffMatch[];
  selected: DiffMatch | null;
  onSelect: (m: DiffMatch) => void;
}

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
  return (
    <div className="flex-1 overflow-auto min-h-0">
      <table className="w-full text-[12px] table-fixed">
        <thead className="sticky top-0 z-10 bg-[var(--bg-secondary)]">
          <tr className="text-[var(--text-muted)] text-[10px] uppercase tracking-wider">
            <th className="px-3 py-2.5 text-left font-medium w-[70px]">Type</th>
            <th className="px-3 py-2.5 text-left font-medium w-[25%]">Primary</th>
            <th className="px-3 py-2.5 text-left font-medium w-[25%]">Secondary</th>
            <th className="px-3 py-2.5 text-left font-medium w-[100px]">Ratio</th>
            <th className="px-3 py-2.5 text-center font-medium w-[70px]">Nodes</th>
            <th className="px-3 py-2.5 text-left font-medium">Heuristic</th>
          </tr>
        </thead>
        <tbody>
          {matches.map((m, i) => {
            const isSelected =
              selected?.primary_addr === m.primary_addr &&
              selected?.secondary_addr === m.secondary_addr;
            return (
              <tr
                key={i}
                onClick={() => onSelect(m)}
                className={`row-hover cursor-pointer border-b border-[var(--border-subtle)] ${
                  isSelected ? "!bg-[var(--accent-glow)]" : ""
                }`}
              >
                <td className="px-3 py-1.5">
                  <span className={`inline-block px-1.5 py-0.5 text-[10px] font-medium rounded border ${typeStyle(m.match_type)}`}>
                    {m.match_type}
                  </span>
                </td>
                <td className="px-3 py-1.5 font-mono truncate overflow-hidden">
                  {m.primary_name}
                </td>
                <td className="px-3 py-1.5 font-mono truncate overflow-hidden text-[var(--text-secondary)]">
                  {m.secondary_name}
                </td>
                <td className="px-3 py-1.5">
                  <div className="flex items-center gap-2">
                    <span className="font-mono text-[11px] w-[30px] shrink-0" style={{ color: ratioColor(m.ratio) }}>
                      {(m.ratio * 100).toFixed(0)}%
                    </span>
                    <div className="ratio-bar flex-1">
                      <div
                        className="ratio-bar-fill"
                        style={{ width: `${m.ratio * 100}%`, background: ratioColor(m.ratio) }}
                      />
                    </div>
                  </div>
                </td>
                <td className="px-3 py-1.5 text-center font-mono text-[var(--text-muted)]">
                  {m.nodes1}/{m.nodes2}
                </td>
                <td className="px-3 py-1.5 text-[var(--text-muted)] truncate overflow-hidden">
                  {m.description}
                </td>
              </tr>
            );
          })}
        </tbody>
      </table>
      {matches.length === 0 && (
        <div className="flex items-center justify-center h-32 text-[var(--text-muted)] text-sm">
          No matches found
        </div>
      )}
    </div>
  );
}
