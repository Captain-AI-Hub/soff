import { useEffect, useState, useRef } from "react";
import { invoke } from "@tauri-apps/api/core";
import { open } from "@tauri-apps/plugin-dialog";
import { Sidebar } from "./components/Sidebar";
import { Toolbar } from "./components/Toolbar";
import { MatchTable } from "./components/MatchTable";
import { DiffViewer } from "./components/DiffViewer";
import { AnalyzeView } from "./components/AnalyzeView";
import { DiffPage } from "./components/DiffPage";
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

export interface McpStatus {
  running: boolean;
  bind_address: string;
  port: number;
  endpoint: string;
  call_count: number;
}

interface UnmatchedFunction {
  side: string;
  address: string;
  name: string;
}

export type Page = "analyze" | "soff" | "graph" | "diff";

export default function App() {
  const [config, setConfig] = useState<SoffConfig | null>(null);
  const [matches, setMatches] = useState<DiffMatch[]>([]);
  const [selected, setSelected] = useState<DiffMatch | null>(null);
  const [filter, setFilter] = useState("all");
  const [page, setPage] = useState<Page>("analyze");
  const [soffPath, setSoffPath] = useState("");
  const [searchQuery, setSearchQuery] = useState("");
  const [mcpStatus, setMcpStatus] = useState<McpStatus | null>(null);
  const [mcpBusy, setMcpBusy] = useState(false);
  const [mcpBindAddress, setMcpBindAddress] = useState("127.0.0.1");
  const [mcpPort, setMcpPort] = useState(11339);
  const [mcpError, setMcpError] = useState("");
  const searchTimer = useRef<ReturnType<typeof setTimeout> | null>(null);

  useEffect(() => {
    refreshMcpStatus();
    const timer = setInterval(refreshMcpStatus, 3000);
    return () => clearInterval(timer);
  }, []);

  const handleOpen = async () => {
    const path = await open({
      filters: [{ name: "Soff Results", extensions: ["soff"] }],
    });
    if (!path) return;
    await loadSoffFile(path);
  };

  const loadSoffFile = async (path: string) => {
    setSoffPath(path);
    const cfg = await invoke<SoffConfig>("open_soff", { path });
    setConfig(cfg);
    const data = await invoke<DiffMatch[]>("get_matches", {
      path, matchType: "all", limit: 500000, offset: 0,
    });
    setMatches(data);
    setSelected(null);
    setPage("analyze");
  };

  const handleSelectMatch = (m: DiffMatch) => {
    setSelected(m);
    setPage("graph");
  };

  const handleStartMcp = async () => {
    if (mcpBusy) return;
    setMcpBusy(true);
    setMcpError("");
    try {
      const status = await invoke<McpStatus>("start_mcp_server", {
        bindAddress: mcpBindAddress,
        port: mcpPort,
      });
      setMcpStatus(status);
    } catch (e) {
      setMcpError(String(e));
    } finally {
      setMcpBusy(false);
    }
  };

  const handleStopMcp = async () => {
    if (mcpBusy) return;
    setMcpBusy(true);
    setMcpError("");
    try {
      const status = await invoke<McpStatus>("stop_mcp_server");
      setMcpStatus(status);
    } catch (e) {
      setMcpError(String(e));
    } finally {
      setMcpBusy(false);
    }
  };

  const refreshMcpStatus = () => {
    invoke<McpStatus>("get_mcp_status")
      .then((status) => {
        setMcpStatus(status);
        if (status.running) {
          setMcpBindAddress(status.bind_address);
          setMcpPort(status.port);
        }
      })
      .catch(() => {});
  };

  const loadFilteredMatches = async (type: string) => {
    setFilter(type);
    if (!soffPath) return;
    if (type === "unmatched") {
      const data = await invoke<UnmatchedFunction[]>("get_unmatched", {
        path: soffPath, limit: 500000, offset: 0,
      });
      // Convert unmatched to DiffMatch format for display
      setMatches(data.map((u) => ({
        match_type: u.side,
        primary_addr: u.side === "primary" ? u.address : "",
        primary_name: u.side === "primary" ? u.name : "",
        secondary_addr: u.side === "secondary" ? u.address : "",
        secondary_name: u.side === "secondary" ? u.name : "",
        ratio: 0,
        nodes1: 0,
        nodes2: 0,
        description: "Unmatched " + u.side,
      })));
    } else {
      const data = await invoke<DiffMatch[]>("get_matches", {
        path: soffPath, matchType: type, limit: 500000, offset: 0,
      });
      setMatches(data);
    }
  };

  const handleSearch = (query: string) => {
    setSearchQuery(query);
    if (searchTimer.current) clearTimeout(searchTimer.current);
    if (!soffPath) return;
    if (!query.trim()) {
      // Empty search: reload current filter
      searchTimer.current = setTimeout(() => loadFilteredMatches(filter), 100);
      return;
    }
    searchTimer.current = setTimeout(async () => {
      if (filter === "unmatched") {
        const data = await invoke<UnmatchedFunction[]>("search_unmatched", {
          path: soffPath, query: query.trim(), limit: 10000,
        });
        setMatches(data.map((u) => ({
          match_type: u.side,
          primary_addr: u.side === "primary" ? u.address : "",
          primary_name: u.side === "primary" ? u.name : "",
          secondary_addr: u.side === "secondary" ? u.address : "",
          secondary_name: u.side === "secondary" ? u.name : "",
          ratio: 0,
          nodes1: 0,
          nodes2: 0,
          description: "Unmatched " + u.side,
        })));
      } else {
        const data = await invoke<DiffMatch[]>("search_matches", {
          path: soffPath, query: query.trim(), matchType: filter, limit: 10000,
        });
        setMatches(data);
      }
    }, 250);
  };

  const filtered = matches;

  return (
    <div className="flex h-screen">
      <Sidebar
        page={page}
        onPageChange={setPage}
        hasData={!!config}
      />
      <div className="flex flex-col flex-1 min-w-0">
        {!config && page !== "soff" && page !== "diff" && <EmptyState onOpen={handleOpen} />}

        {page === "analyze" && config && (
          <AnalyzeView
            soffPath={soffPath}
            config={config}
            onConfigChange={setConfig}
            mcpStatus={mcpStatus}
            mcpBusy={mcpBusy}
            mcpError={mcpError}
            mcpBindAddress={mcpBindAddress}
            mcpPort={mcpPort}
            onMcpBindAddressChange={setMcpBindAddress}
            onMcpPortChange={setMcpPort}
            onStartMcp={handleStartMcp}
            onStopMcp={handleStopMcp}
            onRefreshMcp={refreshMcpStatus}
          />
        )}

        {page === "soff" && (
          <>
            {config ? (
              <>
                <Toolbar config={config} filter={filter} onFilter={loadFilteredMatches} searchQuery={searchQuery} onSearch={handleSearch} />
                <MatchTable matches={filtered} selected={selected} onSelect={handleSelectMatch} />
              </>
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

        <div className={page === "diff" ? "flex-1 flex flex-col min-h-0" : "hidden"}>
          <DiffPage onDiffComplete={(path) => { loadSoffFile(path); }} />
        </div>
      </div>
    </div>
  );
}
