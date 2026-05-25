import { useEffect, useRef, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import type { DiffMatch } from "../App";
import { CfgView } from "./CfgView";

interface Props {
  match: DiffMatch;
  mainDb: string;
  diffDb: string;
  height: number;
  onHeightChange: (h: number) => void;
}

interface DiffPair {
  left: string;
  right: string;
  kind: "equal" | "added" | "removed";
}

type ViewMode = "side-pseudo" | "side-asm" | "unified-pseudo" | "unified-asm" | "cfg";

export function DiffViewer({ match, mainDb, diffDb, height, onHeightChange }: Props) {
  const [mode, setMode] = useState<ViewMode>("side-pseudo");
  const [pairs, setPairs] = useState<DiffPair[]>([]);
  const [diffLines, setDiffLines] = useState<string[]>([]);
  const [loading, setLoading] = useState(false);
  const dragRef = useRef<{ startY: number; startH: number } | null>(null);

  useEffect(() => {
    let cancelled = false;
    async function load() {
      if (mode === "cfg") return;
      setLoading(true);
      const col = mode.includes("pseudo") ? "get_function_pseudocode" : "get_function_assembly";
      const [left, right] = await Promise.all([
        invoke<string>(col, { dbPath: mainDb, address: match.primary_addr }),
        invoke<string>(col, { dbPath: diffDb, address: match.secondary_addr }),
      ]);
      if (cancelled) return;
      if (mode.startsWith("side-")) {
        const p = await invoke<DiffPair[]>("compute_aligned_diff", { left, right });
        if (!cancelled) setPairs(p);
      } else {
        const lines = await invoke<string[]>("compute_diff", { left, right });
        if (!cancelled) setDiffLines(lines);
      }
      setLoading(false);
    }
    load();
    return () => { cancelled = true; };
  }, [match, mode, mainDb, diffDb]);

  const handleMouseDown = (e: React.MouseEvent) => {
    e.preventDefault();
    dragRef.current = { startY: e.clientY, startH: height };
    const onMove = (ev: MouseEvent) => {
      if (!dragRef.current) return;
      const delta = dragRef.current.startY - ev.clientY;
      onHeightChange(Math.max(150, Math.min(window.innerHeight - 200, dragRef.current.startH + delta)));
    };
    const onUp = () => { dragRef.current = null; document.removeEventListener("mousemove", onMove); document.removeEventListener("mouseup", onUp); };
    document.addEventListener("mousemove", onMove);
    document.addEventListener("mouseup", onUp);
  };

  const isAsm = mode.includes("asm");

  return (
    <div className="flex flex-col flex-1 min-h-0" style={height ? { height, flex: "none" } : undefined}>
      {height > 0 && <div onMouseDown={handleMouseDown} className="h-1.5 cursor-row-resize bg-[var(--border)] hover:bg-[var(--accent)] transition-colors shrink-0" />}
      <div className="flex items-center px-3 py-1.5 bg-[var(--bg-secondary)] border-b border-[var(--border)] shrink-0 gap-3">
        <span className="font-mono text-[11px] text-[var(--text-primary)] truncate max-w-[180px]">{match.primary_name}</span>
        <span className="text-[var(--text-muted)] text-[10px]">↔</span>
        <span className="font-mono text-[11px] text-[var(--text-secondary)] truncate max-w-[180px]">{match.secondary_name}</span>
        <span className="font-mono text-[10px] px-1.5 py-0.5 rounded bg-[var(--bg-surface)] text-[var(--text-muted)]">{(match.ratio * 100).toFixed(0)}%</span>
        <div className="ml-auto flex items-center gap-0.5 p-0.5 rounded-lg bg-[var(--bg-primary)] border border-[var(--border)]">
          <TabBtn active={mode === "side-pseudo"} onClick={() => setMode("side-pseudo")} icon="⇔" label="Pseudo" />
          <TabBtn active={mode === "side-asm"} onClick={() => setMode("side-asm")} icon="⇔" label="ASM" />
          <div className="w-px h-4 bg-[var(--border)] mx-0.5" />
          <TabBtn active={mode === "unified-pseudo"} onClick={() => setMode("unified-pseudo")} icon="±" label="Pseudo" />
          <TabBtn active={mode === "unified-asm"} onClick={() => setMode("unified-asm")} icon="±" label="ASM" />
          <div className="w-px h-4 bg-[var(--border)] mx-0.5" />
          <TabBtn active={mode === "cfg"} onClick={() => setMode("cfg")} icon="◈" label="CFG" />
        </div>
      </div>
      <div className="flex-1 overflow-hidden">
        {loading ? (
          <div className="flex items-center justify-center h-full"><div className="animate-glow w-2 h-2 rounded-full bg-[var(--accent)]" /></div>
        ) : mode === "cfg" ? (
          <CfgView match={match} mainDb={mainDb} diffDb={diffDb} />
        ) : mode.startsWith("side-") ? (
          <AlignedSideView pairs={pairs} isAsm={isAsm} />
        ) : (
          <UnifiedView lines={diffLines} isAsm={isAsm} />
        )}
      </div>
    </div>
  );
}

/* ===== Aligned Side-by-Side ===== */
function AlignedSideView({ pairs, isAsm }: { pairs: DiffPair[]; isAsm: boolean }) {
  const leftRef = useRef<HTMLDivElement>(null);
  const rightRef = useRef<HTMLDivElement>(null);
  const syncScroll = (src: "l" | "r") => {
    const from = src === "l" ? leftRef.current : rightRef.current;
    const to = src === "l" ? rightRef.current : leftRef.current;
    if (from && to) { to.scrollTop = from.scrollTop; to.scrollLeft = from.scrollLeft; }
  };

  return (
    <div className="flex h-full">
      <div className="flex-1 flex flex-col min-w-0 border-r border-[var(--border)]">
        <div className="px-3 py-1 bg-[var(--bg-surface)] text-[10px] text-[var(--text-muted)] uppercase tracking-wider shrink-0 border-b border-[var(--border)]">Primary</div>
        <div ref={leftRef} onScroll={() => syncScroll("l")} className="flex-1 overflow-auto font-mono text-[11px] leading-[20px]">
          {pairs.map((p, i) => <AlignedLine key={i} text={p.left} kind={p.kind} side="left" num={i + 1} isAsm={isAsm} />)}
        </div>
      </div>
      <div className="flex-1 flex flex-col min-w-0">
        <div className="px-3 py-1 bg-[var(--bg-surface)] text-[10px] text-[var(--text-muted)] uppercase tracking-wider shrink-0 border-b border-[var(--border)]">Secondary</div>
        <div ref={rightRef} onScroll={() => syncScroll("r")} className="flex-1 overflow-auto font-mono text-[11px] leading-[20px]">
          {pairs.map((p, i) => <AlignedLine key={i} text={p.right} kind={p.kind} side="right" num={i + 1} isAsm={isAsm} />)}
        </div>
      </div>
    </div>
  );
}

function AlignedLine({ text, kind, side, num, isAsm }: { text: string; kind: string; side: "left" | "right"; num: number; isAsm: boolean }) {
  let bg = "";
  if (kind === "removed" && side === "left") bg = "bg-rose-500/10";
  else if (kind === "removed" && side === "right") bg = "bg-[var(--bg-secondary)]";
  else if (kind === "added" && side === "right") bg = "bg-emerald-500/10";
  else if (kind === "added" && side === "left") bg = "bg-[var(--bg-secondary)]";

  const isEmpty = text === "";

  return (
    <div className={`flex min-h-[20px] ${bg}`}>
      <span className="w-9 shrink-0 text-right pr-2 text-[10px] text-[var(--text-muted)]/50 select-none">{isEmpty ? "" : num}</span>
      <span className="whitespace-pre">{isEmpty ? "" : <HighlightedCode text={text} isAsm={isAsm} />}</span>
    </div>
  );
}

/* ===== Syntax Highlighting ===== */
function HighlightedCode({ text, isAsm }: { text: string; isAsm: boolean }) {
  if (isAsm) return <>{highlightAsm(text)}</>;
  return <>{highlightPseudo(text)}</>;
}

function highlightAsm(line: string): React.ReactNode[] {
  const parts: React.ReactNode[] = [];
  // Comment
  const commentIdx = line.indexOf(";");
  const code = commentIdx >= 0 ? line.slice(0, commentIdx) : line;
  const comment = commentIdx >= 0 ? line.slice(commentIdx) : "";

  // Tokenize code part
  const tokens = code.split(/(\b(?:mov|push|pop|call|ret|jmp|je|jne|jz|jnz|jg|jl|jge|jle|ja|jb|jae|jbe|lea|add|sub|mul|imul|div|idiv|xor|or|and|not|shl|shr|sar|cmp|test|nop|int|syscall|endbr64|cdqe|movsxd|movzx|movsx|cmov\w+)\b|\b(?:rax|rbx|rcx|rdx|rsi|rdi|rbp|rsp|r8|r9|r10|r11|r12|r13|r14|r15|eax|ebx|ecx|edx|esi|edi|ebp|esp|ax|bx|cx|dx|al|bl|cl|dl|ah|bh|ch|dh|cs|ds|es|fs|gs|ss|xmm\d+|ymm\d+)\b|0x[0-9a-fA-F]+h?|\b[0-9][0-9a-fA-F]*h\b|\b\d+\b)/g);

  let key = 0;
  for (const tok of tokens) {
    if (/^(mov|push|pop|call|ret|jmp|je|jne|jz|jnz|jg|jl|jge|jle|ja|jb|jae|jbe|lea|add|sub|mul|imul|div|idiv|xor|or|and|not|shl|shr|sar|cmp|test|nop|int|syscall|endbr64|cdqe|movsxd|movzx|movsx|cmov\w+)$/i.test(tok)) {
      parts.push(<span key={key++} className="text-[#7aa2f7]">{tok}</span>);
    } else if (/^(rax|rbx|rcx|rdx|rsi|rdi|rbp|rsp|r\d+|eax|ebx|ecx|edx|esi|edi|ebp|esp|ax|bx|cx|dx|al|bl|cl|dl|ah|bh|ch|dh|cs|ds|es|fs|gs|ss|xmm\d+|ymm\d+)$/i.test(tok)) {
      parts.push(<span key={key++} className="text-[#bb9af7]">{tok}</span>);
    } else if (/^(0x[0-9a-fA-F]+h?|[0-9][0-9a-fA-F]*h|\d+)$/.test(tok)) {
      parts.push(<span key={key++} className="text-[#ff9e64]">{tok}</span>);
    } else {
      parts.push(<span key={key++}>{tok}</span>);
    }
  }
  if (comment) parts.push(<span key={key++} className="text-[#565f89]">{comment}</span>);
  return parts;
}

function highlightPseudo(line: string): React.ReactNode[] {
  const parts: React.ReactNode[] = [];
  const tokens = line.split(/(\b(?:if|else|while|for|do|return|break|continue|switch|case|default|goto|sizeof|typedef|struct|union|enum|void|int|unsigned|signed|char|short|long|float|double|bool|const|static|extern|volatile|inline|__int64|__fastcall|__cdecl|__stdcall|BOOL|DWORD|BYTE|WORD|QWORD|LPVOID|HANDLE|NULL|nullptr|true|false)\b|\/\/.*$|"[^"]*"|'[^']*'|0x[0-9a-fA-F]+|\b\d+\b)/g);

  let key = 0;
  for (const tok of tokens) {
    if (/^(if|else|while|for|do|return|break|continue|switch|case|default|goto|sizeof)$/.test(tok)) {
      parts.push(<span key={key++} className="text-[#bb9af7]">{tok}</span>);
    } else if (/^(void|int|unsigned|signed|char|short|long|float|double|bool|const|static|extern|volatile|inline|__int64|__fastcall|__cdecl|__stdcall|BOOL|DWORD|BYTE|WORD|QWORD|LPVOID|HANDLE|typedef|struct|union|enum)$/.test(tok)) {
      parts.push(<span key={key++} className="text-[#2ac3de]">{tok}</span>);
    } else if (/^(NULL|nullptr|true|false)$/.test(tok)) {
      parts.push(<span key={key++} className="text-[#ff9e64]">{tok}</span>);
    } else if (/^\/\//.test(tok)) {
      parts.push(<span key={key++} className="text-[#565f89]">{tok}</span>);
    } else if (/^["']/.test(tok)) {
      parts.push(<span key={key++} className="text-[#9ece6a]">{tok}</span>);
    } else if (/^(0x[0-9a-fA-F]+|\d+)$/.test(tok)) {
      parts.push(<span key={key++} className="text-[#ff9e64]">{tok}</span>);
    } else {
      parts.push(<span key={key++}>{tok}</span>);
    }
  }
  return parts;
}

/* ===== Unified View ===== */
function UnifiedView({ lines, isAsm }: { lines: string[]; isAsm: boolean }) {
  return (
    <div className="h-full overflow-auto font-mono text-[11px] leading-[20px] py-1">
      {lines.map((line, i) => <UnifiedLine key={i} line={line} num={i + 1} isAsm={isAsm} />)}
    </div>
  );
}

function UnifiedLine({ line, num, isAsm }: { line: string; num: number; isAsm: boolean }) {
  const prefix = line[0] || " ";
  let bg = "";
  let gutterCls = "text-[var(--text-muted)]/50";
  if (prefix === "+") { bg = "bg-emerald-500/8"; gutterCls = "text-emerald-800"; }
  else if (prefix === "-") { bg = "bg-rose-500/8"; gutterCls = "text-rose-800"; }

  return (
    <div className={`flex ${bg}`}>
      <span className={`w-9 shrink-0 text-right pr-2 select-none text-[10px] ${gutterCls}`}>{num}</span>
      <span className="whitespace-pre"><HighlightedCode text={line} isAsm={isAsm} /></span>
    </div>
  );
}

/* ===== CFG Placeholder ===== */
function TabBtn({ active, onClick, icon, label }: { active: boolean; onClick: () => void; icon: string; label: string }) {
  return (
    <button
      onClick={onClick}
      className={`flex items-center gap-1 px-2.5 py-1 text-[10px] font-medium rounded-md transition-all duration-100 ${
        active
          ? "bg-[var(--accent)] text-white shadow-sm shadow-[var(--accent)]/30"
          : "text-[var(--text-muted)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)]"
      }`}
    >
      <span className={active ? "opacity-90" : "opacity-50"}>{icon}</span>
      <span>{label}</span>
    </button>
  );
}
