use actix_web::{middleware, web, App, HttpResponse, HttpServer};
use rmcp::transport::streamable_http_server::session::local::LocalSessionManager;
use rmcp::{
    handler::server::{router::tool::ToolRouter, wrapper::Parameters},
    schemars::JsonSchema,
    tool, tool_handler, tool_router, Json, ServerHandler,
};
use rmcp_actix_web::transport::StreamableHttpService;
use rusqlite::{params, Connection, OptionalExtension, Result as SqlResult};
use serde::{Deserialize, Serialize};
use similar::{ChangeTag, TextDiff};
use std::{
    net::TcpListener,
    path::{Path, PathBuf},
    sync::{
        atomic::{AtomicU64, Ordering},
        Arc, Mutex,
    },
    time::Duration,
};

const DEFAULT_MCP_PORT: u16 = 11339;
const DEFAULT_MCP_BIND_ADDRESS: &str = "127.0.0.1";

#[derive(Debug, Serialize, Clone)]
pub struct McpStatus {
    pub running: bool,
    pub bind_address: String,
    pub port: u16,
    pub endpoint: String,
    pub call_count: u64,
}

struct McpRuntime {
    status: McpStatus,
    handle: actix_web::dev::ServerHandle,
}

#[derive(Default)]
pub struct McpServerState {
    runtime: Mutex<Option<McpRuntime>>,
    call_count: Arc<AtomicU64>,
}

#[derive(Debug, Clone)]
struct SoffMcpService {
    tool_router: ToolRouter<Self>,
    call_count: Arc<AtomicU64>,
}

impl SoffMcpService {
    fn new(call_count: Arc<AtomicU64>) -> Self {
        Self {
            tool_router: Self::tool_router(),
            call_count,
        }
    }

    fn record_call(&self) {
        self.call_count.fetch_add(1, Ordering::Relaxed);
    }
}

#[tool_router(router = tool_router)]
impl SoffMcpService {
    #[tool(description = "Query matched function pairs from a .soff result database.")]
    async fn soff_diff_results(
        &self,
        Parameters(request): Parameters<DiffResultsRequest>,
    ) -> Result<Json<DiffResultsResponse>, String> {
        self.record_call();
        query_diff_results(&request)
            .map(Json)
            .map_err(|e| e.to_string())
    }

    #[tool(description = "Query unmatched functions from a .soff result database.")]
    async fn soff_diff_unmatched(
        &self,
        Parameters(request): Parameters<UnmatchedRequest>,
    ) -> Result<Json<UnmatchedResponse>, String> {
        self.record_call();
        query_unmatched(&request)
            .map(Json)
            .map_err(|e| e.to_string())
    }

    #[tool(
        description = "Return a unified assembly diff for a matched function pair using paths stored in the .soff config."
    )]
    async fn soff_diff_asm(
        &self,
        Parameters(request): Parameters<FunctionDiffRequest>,
    ) -> Result<Json<FunctionDiffResponse>, String> {
        self.record_call();
        query_function_diff(&request, "assembly")
            .map(Json)
            .map_err(|e| e.to_string())
    }

    #[tool(
        description = "Return a unified pseudocode diff for a matched function pair using paths stored in the .soff config."
    )]
    async fn soff_diff_pseudo(
        &self,
        Parameters(request): Parameters<FunctionDiffRequest>,
    ) -> Result<Json<FunctionDiffResponse>, String> {
        self.record_call();
        query_function_diff(&request, "pseudocode")
            .map(Json)
            .map_err(|e| e.to_string())
    }
}

#[tool_handler(
    router = self.tool_router,
    name = "soff-mcp",
    version = "0.1.0",
    instructions = "Read-only Soff diff result tools. Start this server from soff-desktop and connect to /mcp/."
)]
impl ServerHandler for SoffMcpService {}

#[derive(Debug, Deserialize, JsonSchema)]
pub struct DiffResultsRequest {
    #[schemars(description = "Path to the .soff result database")]
    pub result_path: String,
    #[schemars(description = "Match type: all, best, partial, unreliable, or multimatch")]
    pub match_type: Option<String>,
    #[schemars(description = "Maximum rows to return")]
    pub limit: Option<u32>,
    #[schemars(description = "Rows to skip")]
    pub offset: Option<u32>,
}

#[derive(Debug, Serialize, JsonSchema)]
pub struct DiffResultsResponse {
    pub main_db: String,
    pub diff_db: String,
    pub total: u32,
    pub items: Vec<DiffMatchItem>,
}

#[derive(Debug, Serialize, JsonSchema)]
pub struct DiffMatchItem {
    pub match_type: String,
    pub primary_addr: String,
    pub primary_name: String,
    pub secondary_addr: String,
    pub secondary_name: String,
    pub ratio: f64,
    pub nodes1: u32,
    pub nodes2: u32,
    pub description: String,
}

#[derive(Debug, Deserialize, JsonSchema)]
pub struct UnmatchedRequest {
    #[schemars(description = "Path to the .soff result database")]
    pub result_path: String,
    #[schemars(description = "Unmatched side: all, primary, or secondary")]
    pub side: Option<String>,
    #[schemars(description = "Maximum rows to return")]
    pub limit: Option<u32>,
    #[schemars(description = "Rows to skip")]
    pub offset: Option<u32>,
}

#[derive(Debug, Serialize, JsonSchema)]
pub struct UnmatchedResponse {
    pub total: u32,
    pub items: Vec<UnmatchedItem>,
}

#[derive(Debug, Serialize, JsonSchema)]
pub struct UnmatchedItem {
    pub side: String,
    pub address: String,
    pub name: String,
}

#[derive(Debug, Deserialize, JsonSchema)]
pub struct FunctionDiffRequest {
    #[schemars(description = "Path to the .soff result database")]
    pub result_path: String,
    #[schemars(
        description = "Primary function address; accepts decimal, 0x-prefixed hex, or trailing-h hex"
    )]
    pub primary_addr: String,
    #[schemars(
        description = "Secondary function address; accepts decimal, 0x-prefixed hex, or trailing-h hex"
    )]
    pub secondary_addr: String,
}

#[derive(Debug, Serialize, JsonSchema)]
pub struct FunctionDiffResponse {
    pub main_db: String,
    pub diff_db: String,
    pub primary_addr: String,
    pub secondary_addr: String,
    pub lines: Vec<String>,
}

pub async fn start_mcp_server(
    state: tauri::State<'_, McpServerState>,
    bind_address: Option<String>,
    port: Option<u16>,
) -> Result<McpStatus, String> {
    if let Some(status) = mcp_status_from_state(&state) {
        return Ok(status);
    }

    let port = port.unwrap_or(DEFAULT_MCP_PORT);
    let bind_address = bind_address.unwrap_or_else(|| DEFAULT_MCP_BIND_ADDRESS.to_string());
    validate_bind_address(&bind_address)?;
    let bind_addr = socket_address(&bind_address, port);
    let listener = TcpListener::bind(&bind_addr).map_err(|e| e.to_string())?;
    let endpoint = endpoint_url(&bind_address, port);
    state.call_count.store(0, Ordering::Relaxed);
    let call_count = state.call_count.clone();

    let http_service = StreamableHttpService::builder()
        .service_factory(Arc::new(move || {
            Ok(SoffMcpService::new(call_count.clone()))
        }))
        .session_manager(Arc::new(LocalSessionManager::default()))
        .stateful_mode(true)
        .sse_keep_alive(Duration::from_secs(30))
        .build();

    let server = HttpServer::new(move || {
        App::new()
            .wrap(middleware::NormalizePath::trim())
            .wrap(
                middleware::DefaultHeaders::new()
                    .add(("Access-Control-Allow-Origin", "*"))
                    .add(("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS"))
                    .add((
                        "Access-Control-Allow-Headers",
                        "Content-Type, Accept, Mcp-Session-Id",
                    )),
            )
            .route("/health", web::get().to(health))
            .service(web::scope("/mcp").service(http_service.clone().scope()))
    })
    .listen(listener)
    .map_err(|e| e.to_string())?
    .run();

    let handle = server.handle();
    let status = McpStatus {
        running: true,
        bind_address,
        port,
        endpoint,
        call_count: 0,
    };
    {
        let mut runtime = state.runtime.lock().map_err(|e| e.to_string())?;
        *runtime = Some(McpRuntime {
            status: status.clone(),
            handle,
        });
    }

    std::thread::spawn(move || {
        let system = actix_web::rt::System::new();
        let _ = system.block_on(server);
    });

    Ok(status)
}

pub fn get_mcp_status(state: tauri::State<'_, McpServerState>) -> Result<McpStatus, String> {
    Ok(mcp_status_from_state(&state).unwrap_or_else(|| McpStatus {
        running: false,
        bind_address: DEFAULT_MCP_BIND_ADDRESS.to_string(),
        port: DEFAULT_MCP_PORT,
        endpoint: format!("http://127.0.0.1:{DEFAULT_MCP_PORT}/mcp/"),
        call_count: state.call_count.load(Ordering::Relaxed),
    }))
}

pub async fn stop_mcp_server(state: tauri::State<'_, McpServerState>) -> Result<McpStatus, String> {
    let runtime = {
        let mut guard = state.runtime.lock().map_err(|e| e.to_string())?;
        guard.take()
    };
    if let Some(runtime) = runtime {
        runtime.handle.stop(true).await;
    }
    get_mcp_status(state)
}

fn mcp_status_from_state(state: &tauri::State<'_, McpServerState>) -> Option<McpStatus> {
    state.runtime.lock().ok().and_then(|runtime| {
        runtime.as_ref().map(|runtime| {
            let mut status = runtime.status.clone();
            status.call_count = state.call_count.load(Ordering::Relaxed);
            status
        })
    })
}

fn validate_bind_address(bind_address: &str) -> Result<(), String> {
    if matches!(bind_address, "127.0.0.1" | "0.0.0.0" | "::1") {
        return Ok(());
    }
    Err("bind_address must be 127.0.0.1, 0.0.0.0, or ::1".to_string())
}

fn socket_address(bind_address: &str, port: u16) -> String {
    if bind_address.contains(':') {
        format!("[{bind_address}]:{port}")
    } else {
        format!("{bind_address}:{port}")
    }
}

fn endpoint_url(bind_address: &str, port: u16) -> String {
    if bind_address.contains(':') {
        format!("http://[{bind_address}]:{port}/mcp/")
    } else {
        format!("http://{bind_address}:{port}/mcp/")
    }
}

async fn health() -> HttpResponse {
    HttpResponse::Ok().json(serde_json::json!({
        "status": "ok",
        "service": "soff-mcp",
        "endpoint": "/mcp/"
    }))
}

fn query_diff_results(request: &DiffResultsRequest) -> SqlResult<DiffResultsResponse> {
    let conn = Connection::open(&request.result_path)?;
    let (main_db, diff_db) = load_paths(&conn)?;
    let match_type = request.match_type.as_deref().unwrap_or("all");
    let limit = request.limit.unwrap_or(100).min(1000);
    let offset = request.offset.unwrap_or(0);

    let total = if match_type == "all" {
        conn.query_row("SELECT count(*) FROM results", [], |row| row.get(0))?
    } else {
        conn.query_row(
            "SELECT count(*) FROM results WHERE type = ?1",
            [match_type],
            |row| row.get(0),
        )?
    };

    let mut items = Vec::new();
    if match_type == "all" {
        let mut stmt = conn.prepare(
            "SELECT type, address, name, address2, name2, ratio, nodes1, nodes2, description \
             FROM results ORDER BY ratio ASC, line LIMIT ?1 OFFSET ?2",
        )?;
        let rows = stmt.query_map(params![limit, offset], read_match_item)?;
        for row in rows {
            items.push(row?);
        }
    } else {
        let mut stmt = conn.prepare(
            "SELECT type, address, name, address2, name2, ratio, nodes1, nodes2, description \
             FROM results WHERE type = ?1 ORDER BY ratio ASC, line LIMIT ?2 OFFSET ?3",
        )?;
        let rows = stmt.query_map(params![match_type, limit, offset], read_match_item)?;
        for row in rows {
            items.push(row?);
        }
    }

    Ok(DiffResultsResponse {
        main_db,
        diff_db,
        total,
        items,
    })
}

fn query_unmatched(request: &UnmatchedRequest) -> SqlResult<UnmatchedResponse> {
    let conn = Connection::open(&request.result_path)?;
    let side = request.side.as_deref().unwrap_or("all");
    let limit = request.limit.unwrap_or(100).min(1000);
    let offset = request.offset.unwrap_or(0);

    let total = if side == "all" {
        conn.query_row("SELECT count(*) FROM unmatched", [], |row| row.get(0))?
    } else {
        conn.query_row(
            "SELECT count(*) FROM unmatched WHERE type = ?1",
            [side],
            |row| row.get(0),
        )?
    };

    let mut items = Vec::new();
    if side == "all" {
        let mut stmt = conn.prepare(
            "SELECT type, address, name FROM unmatched ORDER BY line LIMIT ?1 OFFSET ?2",
        )?;
        let rows = stmt.query_map(params![limit, offset], read_unmatched_item)?;
        for row in rows {
            items.push(row?);
        }
    } else {
        let mut stmt = conn.prepare(
            "SELECT type, address, name FROM unmatched WHERE type = ?1 ORDER BY line LIMIT ?2 OFFSET ?3",
        )?;
        let rows = stmt.query_map(params![side, limit, offset], read_unmatched_item)?;
        for row in rows {
            items.push(row?);
        }
    }

    Ok(UnmatchedResponse { total, items })
}

fn query_function_diff(
    request: &FunctionDiffRequest,
    column: &str,
) -> SqlResult<FunctionDiffResponse> {
    let conn = Connection::open(&request.result_path)?;
    let (main_db, diff_db) = load_paths(&conn)?;
    let main_db = resolve_db_path(&request.result_path, &main_db);
    let diff_db = resolve_db_path(&request.result_path, &diff_db);
    let primary_addr = normalize_address(&request.primary_addr);
    let secondary_addr = normalize_address(&request.secondary_addr);

    let left = query_function_column(&main_db.to_string_lossy(), &primary_addr, column)?
        .unwrap_or_default();
    let right = query_function_column(&diff_db.to_string_lossy(), &secondary_addr, column)?
        .unwrap_or_default();

    Ok(FunctionDiffResponse {
        main_db: main_db.to_string_lossy().to_string(),
        diff_db: diff_db.to_string_lossy().to_string(),
        primary_addr,
        secondary_addr,
        lines: unified_diff_lines(&left, &right),
    })
}

fn resolve_db_path(result_path: &str, db_path: &str) -> PathBuf {
    let path = Path::new(db_path);
    if path.is_absolute() {
        return path.to_path_buf();
    }
    Path::new(result_path)
        .parent()
        .map(|parent| parent.join(path))
        .unwrap_or_else(|| path.to_path_buf())
}

fn load_paths(conn: &Connection) -> SqlResult<(String, String)> {
    conn.query_row("SELECT main_db, diff_db FROM config LIMIT 1", [], |row| {
        Ok((row.get(0)?, row.get(1)?))
    })
}

fn read_match_item(row: &rusqlite::Row<'_>) -> SqlResult<DiffMatchItem> {
    Ok(DiffMatchItem {
        match_type: row.get(0)?,
        primary_addr: row.get(1)?,
        primary_name: row.get(2)?,
        secondary_addr: row.get(3)?,
        secondary_name: row.get(4)?,
        ratio: row.get(5)?,
        nodes1: row.get(6)?,
        nodes2: row.get(7)?,
        description: row.get(8)?,
    })
}

fn read_unmatched_item(row: &rusqlite::Row<'_>) -> SqlResult<UnmatchedItem> {
    Ok(UnmatchedItem {
        side: row.get(0)?,
        address: row.get(1)?,
        name: row.get(2)?,
    })
}

fn query_function_column(db_path: &str, address: &str, column: &str) -> SqlResult<Option<String>> {
    let conn = Connection::open(db_path)?;
    let sql = format!("SELECT {column} FROM functions WHERE address = ?1 LIMIT 1");
    conn.query_row(&sql, [address], |row| row.get(0)).optional()
}

fn normalize_address(text: &str) -> String {
    let trimmed = text.trim();
    let hex = trimmed
        .strip_prefix("0x")
        .or_else(|| trimmed.strip_prefix("0X"))
        .map(str::to_string)
        .or_else(|| {
            trimmed
                .strip_suffix('h')
                .or_else(|| trimmed.strip_suffix('H'))
                .map(str::to_string)
        });
    if let Some(hex) = hex {
        if let Ok(value) = u64::from_str_radix(&hex, 16) {
            return value.to_string();
        }
    }
    trimmed.to_string()
}

fn unified_diff_lines(left: &str, right: &str) -> Vec<String> {
    TextDiff::from_lines(left, right)
        .iter_all_changes()
        .map(|change| {
            let prefix = match change.tag() {
                ChangeTag::Equal => " ",
                ChangeTag::Delete => "-",
                ChangeTag::Insert => "+",
            };
            format!("{}{}", prefix, change.value().trim_end())
        })
        .collect()
}
