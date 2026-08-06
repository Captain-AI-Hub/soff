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

interface PagedResponse<T> {
  total: number;
  items: T[];
}

function matchKey(match: DiffMatch): string {
  return [
    match.match_type,
    match.primary_addr,
    match.secondary_addr,
    match.primary_name,
    match.secondary_name,
  ].join("\u0000");
}

export type Page = "analyze" | "soff" | "graph" | "diff";

const PAGE_SIZE = 2000;

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
  const [totalRows, setTotalRows] = useState(0);
  const [loadingRows, setLoadingRows] = useState(false);
  const [rowError, setRowError] = useState("");
  const searchTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const requestSeq = useRef(0);
  const loadingRowsRef = useRef(false);

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
    const requestId = ++requestSeq.current;
    setSoffPath(path);
    setRowError("");
    try {
      const cfg = await invoke<SoffConfig>("open_soff", { path });
      if (requestId !== requestSeq.current) return;
      setConfig(cfg);
      await loadRowsPage({ path, type: "all", query: "", offset: 0, append: false, requestId });
      if (requestId !== requestSeq.current) return;
      setSelected(null);
      setPage("analyze");
    } catch (error) {
      if (requestId !== requestSeq.current) return;
      loadingRowsRef.current = false;
      setLoadingRows(false);
      setConfig(null);
      setMatches([]);
      setTotalRows(0);
      setRowError(String(error));
    }
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

  const unmatchedToMatch = (u: UnmatchedFunction): DiffMatch => ({
    match_type: u.side,
    primary_addr: u.side === "primary" ? u.address : "",
    primary_name: u.side === "primary" ? u.name : "",
    secondary_addr: u.side === "secondary" ? u.address : "",
    secondary_name: u.side === "secondary" ? u.name : "",
    ratio: 0,
    nodes1: 0,
    nodes2: 0,
    description: "Unmatched " + u.side,
  });

  const loadRowsPage = async ({
    path,
    type,
    query,
    offset,
    append,
    requestId,
  }: {
    path: string;
    type: string;
    query: string;
    offset: number;
    append: boolean;
    requestId: number;
  }) => {
    if (append && loadingRowsRef.current) return;
    loadingRowsRef.current = true;
    setLoadingRows(true);
    setRowError("");
    try {
      const trimmed = query.trim();
      if (type === "unmatched") {
        const command = trimmed ? "search_unmatched_page" : "get_unmatched_page";
        const page = trimmed
          ? await invoke<PagedResponse<UnmatchedFunction>>(command, {
              path, query: trimmed, limit: PAGE_SIZE, offset,
            })
          : await invoke<PagedResponse<UnmatchedFunction>>(command, {
              path, limit: PAGE_SIZE, offset,
            });
        if (requestId !== requestSeq.current) return;
        const next = page.items.map(unmatchedToMatch);
        setTotalRows(page.total);
        setMatches((current) => {
          if (!append) return next;
          const seen = new Set(current.map(matchKey));
          return [...current, ...next.filter((item) => {
            const key = matchKey(item);
            if (seen.has(key)) return false;
            seen.add(key);
            return true;
          })];
        });
      } else {
        const command = trimmed ? "search_matches_page" : "get_matches_page";
        const page = trimmed
          ? await invoke<PagedResponse<DiffMatch>>(command, {
              path, query: trimmed, matchType: type, limit: PAGE_SIZE, offset,
            })
          : await invoke<PagedResponse<DiffMatch>>(command, {
              path, matchType: type, limit: PAGE_SIZE, offset,
            });
        if (requestId !== requestSeq.current) return;
        setTotalRows(page.total);
        setMatches((current) => {
          if (!append) return page.items;
          const seen = new Set(current.map(matchKey));
          const unique = page.items.filter((item) => {
            const key = matchKey(item);
            if (seen.has(key)) return false;
            seen.add(key);
            return true;
          });
          return [...current, ...unique];
        });
      }
    } catch (e) {
      if (requestId === requestSeq.current) {
        setRowError(String(e));
        if (!append) {
          setMatches([]);
          setTotalRows(0);
        }
      }
    } finally {
      if (requestId === requestSeq.current) {
        loadingRowsRef.current = false;
        setLoadingRows(false);
      }
    }
  };

  const loadFilteredMatches = async (type: string) => {
    setFilter(type);
    if (!soffPath) return;
    setSelected(null);
    const requestId = ++requestSeq.current;
    await loadRowsPage({
      path: soffPath,
      type,
      query: searchQuery,
      offset: 0,
      append: false,
      requestId,
    });
  };

  const handleSearch = (query: string) => {
    setSearchQuery(query);
    if (searchTimer.current) clearTimeout(searchTimer.current);
    if (!soffPath) return;
    searchTimer.current = setTimeout(async () => {
      const requestId = ++requestSeq.current;
      await loadRowsPage({
        path: soffPath,
        type: filter,
        query,
        offset: 0,
        append: false,
        requestId,
      });
    }, 250);
  };

  const loadMoreRows = () => {
    if (!soffPath || loadingRowsRef.current || matches.length >= totalRows) return;
    const requestId = requestSeq.current;
    void loadRowsPage({
      path: soffPath,
      type: filter,
      query: searchQuery,
      offset: matches.length,
      append: true,
      requestId,
    });
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
                {rowError && <div className="px-3 py-2 text-[11px] font-mono text-red-400 bg-[var(--bg-secondary)] border-b border-[var(--border)]">{rowError}</div>}
                <MatchTable
                  key={`${soffPath}:${filter}:${searchQuery}`}
                  matches={filtered}
                  selected={selected}
                  onSelect={handleSelectMatch}
                  totalCount={totalRows}
                  loadingMore={loadingRows}
                  onLoadMore={loadMoreRows}
                />
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
