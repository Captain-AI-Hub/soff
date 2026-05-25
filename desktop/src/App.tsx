import { useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { open } from "@tauri-apps/plugin-dialog";
import { Sidebar } from "./components/Sidebar";
import { Toolbar } from "./components/Toolbar";
import { MatchTable } from "./components/MatchTable";
import { DiffViewer } from "./components/DiffViewer";
import { AnalyzeView } from "./components/AnalyzeView";
import { EmptyState } from "./components/EmptyState";
import "./index.css";

export interface SoffConfig {
  main_db: string;
  diff_db: string;
  version: string;
  date: string;
  total_matches: number;
  total_unmatched: number;
}

export interface DiffMatch {
  match_type: string;
  primary_addr: string;
  primary_name: string;
  secondary_addr: string;
  secondary_name: string;
  ratio: number;
  nodes1: number;
  nodes2: number;
  description: string;
}

export type Page = "analyze" | "soff" | "graph";

export default function App() {
  const [config, setConfig] = useState<SoffConfig | null>(null);
  const [matches, setMatches] = useState<DiffMatch[]>([]);
  const [selected, setSelected] = useState<DiffMatch | null>(null);
  const [filter, setFilter] = useState("all");
  const [page, setPage] = useState<Page>("analyze");
  const [soffPath, setSoffPath] = useState("");

  const handleOpen = async () => {
    const path = await open({
      filters: [{ name: "Soff Results", extensions: ["soff"] }],
    });
    if (!path) return;
    setSoffPath(path);
    const cfg = await invoke<SoffConfig>("open_soff", { path });
    setConfig(cfg);
    const data = await invoke<DiffMatch[]>("get_matches", {
      path, matchType: "all", limit: 100000, offset: 0,
    });
    setMatches(data);
    setSelected(null);
    setPage("analyze");
  };

  const handleSelectMatch = (m: DiffMatch) => {
    setSelected(m);
    setPage("graph");
  };

  const filtered = (filter === "all"
    ? matches
    : matches.filter((m) => m.match_type === filter)
  ).slice().sort((a, b) => a.ratio - b.ratio);

  return (
    <div className="flex h-screen">
      <Sidebar page={page} onPageChange={setPage} hasData={!!config} />
      <div className="flex flex-col flex-1 min-w-0">
        {!config && page !== "soff" && <EmptyState onOpen={handleOpen} />}

        {page === "analyze" && config && (
          <AnalyzeView soffPath={soffPath} config={config} />
        )}

        {page === "soff" && (
          <>
            <Toolbar onOpen={handleOpen} config={config} filter={filter} onFilter={setFilter} />
            {config ? (
              <MatchTable matches={filtered} selected={selected} onSelect={handleSelectMatch} />
            ) : (
              <EmptyState onOpen={handleOpen} />
            )}
          </>
        )}

        {page === "graph" && selected && config && (
          <DiffViewer
            match={selected}
            mainDb={config.main_db}
            diffDb={config.diff_db}
            height={0}
            onHeightChange={() => {}}
          />
        )}
        {page === "graph" && !selected && config && (
          <div className="flex-1 flex items-center justify-center text-[var(--text-muted)] text-sm">
            Select a match from the Soff panel to view
          </div>
        )}
      </div>
    </div>
  );
}
