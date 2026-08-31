mod db;
mod mcp;

use db::{
    CfgData, DiffMatch, FunctionInfo, PagedMatches, PagedUnmatched, SoffConfig, UnmatchedFunction,
};
use mcp::{McpServerState, McpStatus};

#[tauri::command]
fn open_soff(path: String) -> Result<SoffConfig, String> {
    db::load_soff_config(&path).map_err(|e| e.to_string())
}

#[tauri::command]
fn update_soff_paths(path: String, main_db: String, diff_db: String) -> Result<SoffConfig, String> {
    db::update_soff_paths(&path, &main_db, &diff_db).map_err(|e| e.to_string())
}

#[tauri::command]
fn get_matches(
    path: String,
    match_type: String,
    limit: u32,
    offset: u32,
) -> Result<Vec<DiffMatch>, String> {
    db::query_matches(&path, &match_type, limit, offset)
        .map(|page| page.items)
        .map_err(|e| e.to_string())
}

#[tauri::command]
fn get_matches_page(
    path: String,
    match_type: String,
    limit: u32,
    offset: u32,
) -> Result<PagedMatches, String> {
    db::query_matches(&path, &match_type, limit, offset).map_err(|e| e.to_string())
}

#[tauri::command]
fn get_unmatched(path: String, limit: u32, offset: u32) -> Result<Vec<UnmatchedFunction>, String> {
    db::query_unmatched(&path, limit, offset)
        .map(|page| page.items)
        .map_err(|e| e.to_string())
}

#[tauri::command]
fn get_unmatched_page(path: String, limit: u32, offset: u32) -> Result<PagedUnmatched, String> {
    db::query_unmatched(&path, limit, offset).map_err(|e| e.to_string())
}

#[tauri::command]
fn search_matches(
    path: String,
    query: String,
    match_type: String,
    limit: u32,
) -> Result<Vec<DiffMatch>, String> {
    db::search_matches(&path, &query, &match_type, limit, 0)
        .map(|page| page.items)
        .map_err(|e| e.to_string())
}

#[tauri::command]
fn search_matches_page(
    path: String,
    query: String,
    match_type: String,
    limit: u32,
    offset: u32,
) -> Result<PagedMatches, String> {
    db::search_matches(&path, &query, &match_type, limit, offset).map_err(|e| e.to_string())
}

#[tauri::command]
fn search_unmatched(
    path: String,
    query: String,
    limit: u32,
) -> Result<Vec<UnmatchedFunction>, String> {
    db::search_unmatched(&path, &query, limit, 0)
        .map(|page| page.items)
        .map_err(|e| e.to_string())
}

#[tauri::command]
fn search_unmatched_page(
    path: String,
    query: String,
    limit: u32,
    offset: u32,
) -> Result<PagedUnmatched, String> {
    db::search_unmatched(&path, &query, limit, offset).map_err(|e| e.to_string())
}

#[tauri::command]
fn get_function_assembly(db_path: String, address: String) -> Result<String, String> {
    db::query_function_column(&db_path, &address, "assembly").map_err(|e| e.to_string())
}

#[tauri::command]
fn get_function_pseudocode(db_path: String, address: String) -> Result<String, String> {
    db::query_function_column(&db_path, &address, "pseudocode").map_err(|e| e.to_string())
}

#[tauri::command]
fn get_function_info(db_path: String, address: String) -> Result<FunctionInfo, String> {
    db::query_function_info(&db_path, &address).map_err(|e| e.to_string())
}

#[derive(Debug, serde::Serialize, Clone)]
pub struct DiffPair {
    pub left: String,
    pub right: String,
    /// "equal", "added", "removed"
    pub kind: String,
}

#[tauri::command]
fn compute_diff(left: String, right: String) -> Vec<String> {
    use similar::{ChangeTag, TextDiff};
    let diff = TextDiff::from_lines(&left, &right);
    diff.iter_all_changes()
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

#[tauri::command]
fn compute_aligned_diff(left: String, right: String) -> Vec<DiffPair> {
    use similar::{ChangeTag, TextDiff};
    let diff = TextDiff::from_lines(&left, &right);
    let mut result: Vec<DiffPair> = Vec::new();

    for change in diff.iter_all_changes() {
        let text = change.value().trim_end().to_string();
        match change.tag() {
            ChangeTag::Equal => {
                result.push(DiffPair {
                    left: text.clone(),
                    right: text,
                    kind: "equal".into(),
                });
            }
            ChangeTag::Delete => {
                result.push(DiffPair {
                    left: text,
                    right: String::new(),
                    kind: "removed".into(),
                });
            }
            ChangeTag::Insert => {
                result.push(DiffPair {
                    left: String::new(),
                    right: text,
                    kind: "added".into(),
                });
            }
        }
    }
    result
}

#[tauri::command]
fn extract_cfg(db_path: String, address: String) -> Result<CfgData, String> {
    db::query_cfg(&db_path, &address).map_err(|e| e.to_string())
}

#[derive(Debug, serde::Serialize, Clone)]
pub struct AnalyzeStats {
    pub best: u32,
    pub partial: u32,
    pub unreliable: u32,
    pub unmatched_primary: u32,
    pub unmatched_secondary: u32,
    pub primary_functions: u32,
    pub secondary_functions: u32,
    pub avg_ratio: f64,
    pub total_nodes_primary: u64,
    pub total_nodes_secondary: u64,
    pub total_edges_primary: u64,
    pub total_edges_secondary: u64,
    pub ratio_distribution: Vec<u32>,
}

#[tauri::command]
fn get_analyze_stats(path: String) -> Result<AnalyzeStats, String> {
    let s = db::query_analyze_stats(&path).map_err(|e| e.to_string())?;
    Ok(AnalyzeStats {
        best: s.best,
        partial: s.partial,
        unreliable: s.unreliable,
        unmatched_primary: s.unmatched_primary,
        unmatched_secondary: s.unmatched_secondary,
        primary_functions: s.primary_functions,
        secondary_functions: s.secondary_functions,
        avg_ratio: s.avg_ratio,
        total_nodes_primary: s.total_nodes_primary,
        total_nodes_secondary: s.total_nodes_secondary,
        total_edges_primary: s.total_edges_primary,
        total_edges_secondary: s.total_edges_secondary,
        ratio_distribution: s.ratio_distribution,
    })
}

#[tauri::command]
async fn start_mcp_server(
    state: tauri::State<'_, McpServerState>,
    bind_address: Option<String>,
    port: Option<u16>,
) -> Result<McpStatus, String> {
    mcp::start_mcp_server(state, bind_address, port).await
}

#[tauri::command]
fn get_mcp_status(state: tauri::State<'_, McpServerState>) -> Result<McpStatus, String> {
    mcp::get_mcp_status(state)
}

#[tauri::command]
async fn stop_mcp_server(state: tauri::State<'_, McpServerState>) -> Result<McpStatus, String> {
    mcp::stop_mcp_server(state).await
}

use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int, c_void};

#[repr(C)]
struct SoffDiffOptions {
    enable_slow: c_int,
    enable_unreliable: c_int,
    enable_experimental: c_int,
    max_rows: u32,
    timeout_seconds: u32,
}

type SoffProgressFn = extern "C" fn(*const c_char, *mut c_void);

fn find_soff_ffi_path(
    resource_dir: Option<&std::path::Path>,
) -> Result<std::path::PathBuf, String> {
    let exe_dir = std::env::current_exe()
        .map_err(|e| e.to_string())?
        .parent()
        .ok_or("no parent dir")?
        .to_path_buf();

    let name = if cfg!(windows) {
        "soff_ffi.dll"
    } else if cfg!(target_os = "macos") {
        "libsoff_ffi.dylib"
    } else {
        "libsoff_ffi.so"
    };

    let candidate = exe_dir.join(name);
    if candidate.exists() {
        return Ok(candidate);
    }

    let resources = exe_dir.join("resources").join(name);
    if resources.exists() {
        return Ok(resources);
    }

    // Installed layouts (deb/rpm/dmg/nsis): Tauri puts bundled resources in
    // the app resource dir (e.g. /usr/lib/soff-desktop/), not next to the exe.
    if let Some(dir) = resource_dir {
        for candidate in [dir.join(name), dir.join("resources").join(name)] {
            if candidate.exists() {
                return Ok(candidate);
            }
        }
    }

    // Dev: xmake build output
    let dev_candidates = [
        exe_dir
            .join("../../../../build/windows/x64/release")
            .join(name),
        exe_dir
            .join("../../../../build/linux/x86_64/release")
            .join(name),
        exe_dir
            .join("../../../../build/macosx/arm64/release")
            .join(name),
        exe_dir
            .join("../../../../build/macosx/x86_64/release")
            .join(name),
    ];
    for p in &dev_candidates {
        if p.exists() {
            return p.canonicalize().map_err(|e| e.to_string());
        }
    }

    Err(format!(
        "{} not found near {} or in the app resource directory",
        name,
        exe_dir.display()
    ))
}

struct ChannelUserdata {
    channel: tauri::ipc::Channel<String>,
}

extern "C" fn progress_callback(json_line: *const c_char, userdata: *mut c_void) {
    if json_line.is_null() || userdata.is_null() {
        return;
    }
    let data = unsafe { &*(userdata as *const ChannelUserdata) };
    let line = unsafe { CStr::from_ptr(json_line) }
        .to_string_lossy()
        .to_string();
    let _ = data.channel.send(line);
}

#[tauri::command]
async fn run_diff(
    app: tauri::AppHandle,
    primary_db: String,
    secondary_db: String,
    output_path: String,
    slow: bool,
    unreliable: bool,
    channel: tauri::ipc::Channel<String>,
) -> Result<String, String> {
    let resource_dir = {
        use tauri::Manager;
        app.path().resource_dir().ok()
    };
    let lib_path = find_soff_ffi_path(resource_dir.as_deref())?;
    let output = output_path.clone();

    tauri::async_runtime::spawn_blocking(move || unsafe {
        let lib = libloading::Library::new(&lib_path)
            .map_err(|e| format!("failed to load soff_ffi: {e}"))?;

        let soff_diff_run: libloading::Symbol<
            unsafe extern "C" fn(
                *const c_char,
                *const c_char,
                *const c_char,
                *const SoffDiffOptions,
                SoffProgressFn,
                *mut c_void,
                *mut c_char,
                c_int,
            ) -> c_int,
        > = lib
            .get(b"soff_diff_run")
            .map_err(|e| format!("symbol not found: {e}"))?;

        let c_primary = CString::new(primary_db).map_err(|e| e.to_string())?;
        let c_secondary = CString::new(secondary_db).map_err(|e| e.to_string())?;
        let c_output = CString::new(output.clone()).map_err(|e| e.to_string())?;
        let options = SoffDiffOptions {
            enable_slow: if slow { 1 } else { 0 },
            enable_unreliable: if unreliable { 1 } else { 0 },
            enable_experimental: 0,
            max_rows: 1_000_000,
            timeout_seconds: 300,
        };
        let mut error_buf = vec![0u8; 1024];
        let userdata = ChannelUserdata { channel };
        let userdata_ptr = &userdata as *const ChannelUserdata as *mut c_void;
        let ret = soff_diff_run(
            c_primary.as_ptr(),
            c_secondary.as_ptr(),
            c_output.as_ptr(),
            &options,
            progress_callback,
            userdata_ptr,
            error_buf.as_mut_ptr() as *mut c_char,
            error_buf.len() as c_int,
        );
        if ret != 0 {
            let error = CStr::from_ptr(error_buf.as_ptr() as *const c_char)
                .to_string_lossy()
                .into_owned();
            return Err(error);
        }
        Ok(output)
    })
    .await
    .map_err(|e| format!("diff worker failed: {e}"))?
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .manage(McpServerState::default())
        .plugin(tauri_plugin_opener::init())
        .plugin(tauri_plugin_dialog::init())
        .invoke_handler(tauri::generate_handler![
            open_soff,
            update_soff_paths,
            get_matches,
            get_matches_page,
            get_unmatched,
            get_unmatched_page,
            search_matches,
            search_matches_page,
            search_unmatched,
            search_unmatched_page,
            get_function_assembly,
            get_function_pseudocode,
            get_function_info,
            compute_diff,
            compute_aligned_diff,
            get_analyze_stats,
            extract_cfg,
            run_diff,
            start_mcp_server,
            get_mcp_status,
            stop_mcp_server,
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
