import { useEffect, useRef, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import type { DiffMatch } from "../App";

interface BasicBlock { id: number; lines: string[]; successors: number[]; }
interface CfgData { blocks: BasicBlock[]; }
interface Props { match: DiffMatch; mainDb: string; diffDb: string; }

export function CfgView({ match, mainDb, diffDb }: Props) {
  const [leftCfg, setLeftCfg] = useState<CfgData | null>(null);
  const [rightCfg, setRightCfg] = useState<CfgData | null>(null);
  const [loading, setLoading] = useState(true);

  useEffect(() => {
    setLoading(true);
    Promise.all([
      invoke<CfgData>("extract_cfg", { dbPath: mainDb, address: match.primary_addr }),
      invoke<CfgData>("extract_cfg", { dbPath: diffDb, address: match.secondary_addr }),
    ]).then(([l, r]) => { setLeftCfg(l); setRightCfg(r); setLoading(false); });
  }, [match, mainDb, diffDb]);

  if (loading) return <div className="flex-1 flex items-center justify-center"><div className="animate-glow w-2 h-2 rounded-full bg-[var(--accent)]" /></div>;

  return (
    <div className="flex h-full">
      <CfgPanel cfg={leftCfg} label="Primary" blocks={leftCfg?.blocks.length ?? 0} color="#7aa2f7" />
      <CfgPanel cfg={rightCfg} label="Secondary" blocks={rightCfg?.blocks.length ?? 0} color="#bb9af7" />
    </div>
  );
}

function CfgPanel({ cfg, label, blocks, color }: { cfg: CfgData | null; label: string; blocks: number; color: string }) {
  const ref = useRef<HTMLDivElement>(null);
  if (!cfg || cfg.blocks.length === 0) return (
    <div className="flex-1 flex flex-col items-center justify-center text-[var(--text-muted)] border-r border-[var(--border)] last:border-r-0">
      No blocks
    </div>
  );

  const layouts = layoutCfg(cfg);
  const maxX = Math.max(...layouts.map(l => l.x + l.w)) + 40;
  const maxY = Math.max(...layouts.map(l => l.y + l.h)) + 40;
  const layoutMap = new Map(layouts.map(l => [l.block.id, l]));

  return (
    <div className="flex-1 flex flex-col min-w-0 border-r border-[var(--border)] last:border-r-0">
      <div className="px-3 py-1.5 bg-[var(--bg-surface)] text-[10px] text-[var(--text-muted)] uppercase tracking-wider shrink-0 border-b border-[var(--border)] text-center">
        {label} ({blocks} blocks)
      </div>
      <div ref={ref} className="flex-1 overflow-auto relative" style={{ minHeight: 0 }}>
        <div style={{ width: maxX, height: maxY, position: "relative" }}>
          <svg width={maxX} height={maxY} className="absolute inset-0" style={{ pointerEvents: "none" }}>
            <defs>
              <marker id={`arr-${label}`} markerWidth="8" markerHeight="8" refX="7" refY="4" orient="auto">
                <path d="M0,0 L8,4 L0,8" fill="none" stroke={color} strokeWidth="1.5" />
              </marker>
            </defs>
            {layouts.flatMap(l => l.block.successors.map(sid => {
              const t = layoutMap.get(sid);
              if (!t) return null;
              const x1 = l.x + l.w / 2, y1 = l.y + l.h;
              const x2 = t.x + t.w / 2, y2 = t.y;
              const isBack = y2 <= y1;
              if (isBack) {
                const sx = Math.max(...layouts.map(ll => ll.x + ll.w)) + 30;
                return <path key={`${l.block.id}-${sid}`} d={`M${x1},${y1} Q${sx},${(y1+y2)/2} ${x2},${y2}`}
                  fill="none" stroke={color} strokeWidth="1.5" opacity="0.4" strokeDasharray="4,3" markerEnd={`url(#arr-${label})`} />;
              }
              const dx = x2 - x1;
              const midY = (y1 + y2) / 2;
              return <path key={`${l.block.id}-${sid}`}
                d={dx === 0 ? `M${x1},${y1} L${x2},${y2}` : `M${x1},${y1} C${x1},${midY} ${x2},${midY} ${x2},${y2}`}
                fill="none" stroke={color} strokeWidth="1.5" opacity="0.6" markerEnd={`url(#arr-${label})`} />;
            }))}
          </svg>
          {layouts.map(l => (
            <div key={l.block.id} className="absolute rounded border border-[var(--border)]"
              style={{ left: l.x, top: l.y, width: l.w, background: "var(--bg-surface)", boxShadow: "0 2px 8px rgba(0,0,0,0.3)" }}>
              <div className="px-2 py-0.5 text-[9px] font-mono border-b border-[var(--border)] flex justify-between"
                style={{ background: `${color}12`, color }}>
                <span>Block {l.block.id}</span>
                <span className="text-[var(--text-muted)]">{l.block.lines.length}</span>
              </div>
              <div className="px-2 py-1 font-mono text-[10px] leading-[15px] whitespace-pre overflow-x-auto">
                {l.block.lines.map((line, i) => <AsmLine key={i} text={line} />)}
              </div>
            </div>
          ))}
        </div>
      </div>
    </div>
  );
}

const BLOCK_W = 280;
const BLOCK_PAD_Y = 24;
const LINE_H = 15;
const GAP_Y = 50;
const GAP_X = 40;

interface BlockLayout { x: number; y: number; w: number; h: number; block: BasicBlock; }

function layoutCfg(cfg: CfgData): BlockLayout[] {
  if (cfg.blocks.length === 0) return [];
  const depth = new Map<number, number>();
  const queue: number[] = [0];
  depth.set(0, 0);
  while (queue.length > 0) {
    const id = queue.shift()!;
    const d = depth.get(id)!;
    const block = cfg.blocks[id];
    if (!block) continue;
    for (const s of block.successors) {
      if (!depth.has(s) && s < cfg.blocks.length) { depth.set(s, d + 1); queue.push(s); }
    }
  }
  for (let i = 0; i < cfg.blocks.length; i++) {
    if (!depth.has(i)) depth.set(i, Math.max(...depth.values()) + 1);
  }
  const layers: number[][] = [];
  for (const [id, d] of depth.entries()) { while (layers.length <= d) layers.push([]); layers[d].push(id); }

  const layouts: BlockLayout[] = [];
  let y = 20;
  for (const layer of layers) {
    let x = 20;
    let maxH = 0;
    for (const id of layer) {
      const block = cfg.blocks[id];
      const h = block.lines.length * LINE_H + BLOCK_PAD_Y;
      layouts.push({ x, y, w: BLOCK_W, h, block });
      maxH = Math.max(maxH, h);
      x += BLOCK_W + GAP_X;
    }
    y += maxH + GAP_Y;
  }
  return layouts;
}

function AsmLine({ text }: { text: string }) {
  const t = text.trim();
  if (!t) return <div className="h-[15px]" />;
  if (t.endsWith(":")) return <div className="text-[#e0af68]">{t}</div>;
  return <div>{hlAsm(t)}</div>;
}

function hlAsm(line: string): React.ReactNode[] {
  const parts: React.ReactNode[] = [];
  const ci = line.indexOf(";");
  const code = ci >= 0 ? line.slice(0, ci) : line;
  const comment = ci >= 0 ? line.slice(ci) : "";
  const re = /(\b(?:mov|push|pop|call|ret|retn|jmp|je|jne|jz|jnz|jg|jl|jge|jle|ja|jb|jae|jbe|lea|add|sub|mul|imul|div|idiv|xor|or|and|not|shl|shr|sar|cmp|test|nop|dec|inc|neg|movzx|movsx|movsxd|cdqe|cmov\w*)\b|\b(?:rax|rbx|rcx|rdx|rsi|rdi|rbp|rsp|r8|r9|r10|r11|r12|r13|r14|r15|eax|ebx|ecx|edx|esi|edi|ebp|esp|al|bl|cl|dl|ah|bh|ch|dh|xmm\d+)\b|0[0-9a-fA-F]+h|0x[0-9a-fA-F]+|\b\d+\b)/g;
  const tokens = code.split(re);
  let k = 0;
  for (const tok of tokens) {
    if (/^(mov|push|pop|call|ret|retn|jmp|je|jne|jz|jnz|jg|jl|jge|jle|ja|jb|jae|jbe|lea|add|sub|mul|imul|div|idiv|xor|or|and|not|shl|shr|sar|cmp|test|nop|dec|inc|neg|movzx|movsx|movsxd|cdqe|cmov\w*)$/i.test(tok))
      parts.push(<span key={k++} className="text-[#7aa2f7]">{tok}</span>);
    else if (/^(rax|rbx|rcx|rdx|rsi|rdi|rbp|rsp|r\d+|eax|ebx|ecx|edx|esi|edi|ebp|esp|al|bl|cl|dl|ah|bh|ch|dh|xmm\d+)$/i.test(tok))
      parts.push(<span key={k++} className="text-[#bb9af7]">{tok}</span>);
    else if (/^(0[0-9a-fA-F]+h|0x[0-9a-fA-F]+|\d+)$/.test(tok))
      parts.push(<span key={k++} className="text-[#ff9e64]">{tok}</span>);
    else parts.push(<span key={k++}>{tok}</span>);
  }
  if (comment) parts.push(<span key={k++} className="text-[#565f89]">{comment}</span>);
  return parts;
}