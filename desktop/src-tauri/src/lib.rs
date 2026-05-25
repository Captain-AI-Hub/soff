mod db;

use db::{DiffMatch, FunctionInfo, SoffConfig, UnmatchedFunction};

#[tauri::command]
fn open_soff(path: String) -> Result<SoffConfig, String> {
    db::load_soff_config(&path).map_err(|e| e.to_string())
}

#[tauri::command]
fn get_matches(path: String, match_type: String, limit: u32, offset: u32) -> Result<Vec<DiffMatch>, String> {
    db::query_matches(&path, &match_type, limit, offset).map_err(|e| e.to_string())
}

#[tauri::command]
fn get_unmatched(path: String, limit: u32, offset: u32) -> Result<Vec<UnmatchedFunction>, String> {
    db::query_unmatched(&path, limit, offset).map_err(|e| e.to_string())
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
                result.push(DiffPair { left: text.clone(), right: text, kind: "equal".into() });
            }
            ChangeTag::Delete => {
                result.push(DiffPair { left: text, right: String::new(), kind: "removed".into() });
            }
            ChangeTag::Insert => {
                result.push(DiffPair { left: String::new(), right: text, kind: "added".into() });
            }
        }
    }
    result
}

#[derive(Debug, serde::Serialize, Clone)]
pub struct BasicBlock {
    pub id: usize,
    pub lines: Vec<String>,
    pub successors: Vec<usize>,
}

#[derive(Debug, serde::Serialize, Clone)]
pub struct CfgData {
    pub blocks: Vec<BasicBlock>,
}

#[tauri::command]
fn extract_cfg(db_path: String, address: String) -> Result<CfgData, String> {
    let asm = db::query_function_column(&db_path, &address, "assembly").map_err(|e| e.to_string())?;
    Ok(parse_cfg_from_asm(&asm))
}

fn parse_cfg_from_asm(asm: &str) -> CfgData {
    let lines: Vec<&str> = asm.lines().collect();
    if lines.is_empty() {
        return CfgData { blocks: vec![] };
    }

    let is_jump = |m: &str| -> bool {
        matches!(m, "jmp"|"je"|"jne"|"jz"|"jnz"|"jg"|"jl"|"jge"|"jle"|
            "ja"|"jb"|"jae"|"jbe"|"jc"|"jnc"|"jo"|"jno"|"js"|"jns"|
            "jp"|"jnp"|"jcxz"|"jecxz"|"jrcxz"|"loop")
    };
    let is_ret = |m: &str| -> bool { matches!(m, "ret"|"retn") };

    // Collect all labels (lines ending with ':' or containing ':' before code)
    let mut label_lines: std::collections::HashMap<String, usize> = std::collections::HashMap::new();
    for (i, line) in lines.iter().enumerate() {
        let t = line.trim();
        if let Some(colon_pos) = t.find(':') {
            let label = t[..colon_pos].trim().to_lowercase();
            if !label.is_empty() && !label.contains(' ') {
                label_lines.insert(label, i);
            }
        }
    }

    // Identify block boundaries
    let mut block_starts: Vec<usize> = vec![0];
    for (i, line) in lines.iter().enumerate() {
        let t = line.trim();
        // Label starts a new block
        if t.contains(':') && i > 0 {
            let colon_pos = t.find(':').unwrap();
            let before = t[..colon_pos].trim();
            if !before.is_empty() && !before.contains(' ') {
                if !block_starts.contains(&i) { block_starts.push(i); }
            }
        }
        // After jump/ret, next line starts new block
        let mnemonic = t.split_whitespace().next().unwrap_or("");
        // Skip label prefix if present
        let mnemonic = if mnemonic.ends_with(':') {
            t.split_whitespace().nth(1).unwrap_or("")
        } else { mnemonic }.to_lowercase();

        if (is_jump(&mnemonic) || is_ret(&mnemonic)) && i + 1 < lines.len() {
            if !block_starts.contains(&(i + 1)) { block_starts.push(i + 1); }
        }
    }
    block_starts.sort();
    block_starts.dedup();

    // Create blocks with successor detection
    let mut blocks: Vec<BasicBlock> = Vec::new();
    for (bi, &start) in block_starts.iter().enumerate() {
        let end = block_starts.get(bi + 1).copied().unwrap_or(lines.len());
        let block_lines: Vec<String> = (start..end).map(|i| lines[i].to_string()).collect();
        let mut succs: Vec<usize> = Vec::new();

        if let Some(last) = block_lines.last() {
            let t = last.trim();
            let parts: Vec<&str> = t.split_whitespace().collect();
            // Get mnemonic (skip label if present)
            let (mn_idx, mn) = if parts.first().map_or(false, |p| p.ends_with(':')) {
                (1, parts.get(1).unwrap_or(&"").to_lowercase())
            } else {
                (0, parts.first().unwrap_or(&"").to_lowercase())
            };

            if is_ret(&mn) {
                // no successors
            } else if is_jump(&mn) {
                // Find target
                let operand = parts.get(mn_idx + 1).unwrap_or(&"");
                // Remove "short", "near", "far" prefixes
                let target_name = operand.trim_start_matches("short ")
                    .trim_start_matches("near ")
                    .trim_end_matches(',')
                    .to_lowercase();
                // Also check next part if operand was "short"
                let target_name = if target_name == "short" || target_name == "near" {
                    parts.get(mn_idx + 2).unwrap_or(&"").trim_end_matches(',').to_lowercase()
                } else { target_name };

                if let Some(&target_line) = label_lines.get(&target_name) {
                    // Find which block contains this line
                    for (ti, &ts) in block_starts.iter().enumerate() {
                        if ts == target_line { succs.push(ti); break; }
                    }
                }
                // Conditional jumps also fall through
                if mn != "jmp" && bi + 1 < block_starts.len() {
                    succs.push(bi + 1);
                }
            } else {
                // Fallthrough
                if bi + 1 < block_starts.len() { succs.push(bi + 1); }
            }
        }
        succs.sort();
        succs.dedup();
        blocks.push(BasicBlock { id: bi, lines: block_lines, successors: succs });
    }

    CfgData { blocks }
}

fn find_target_block(_block_starts: &[usize], _lines: &[&str], _jump_line: &str) -> Option<usize> {
    None // unused now
}

#[derive(Debug, serde::Serialize, Clone)]
pub struct AnalyzeStats {
    pub best: u32,
    pub partial: u32,
    pub unreliable: u32,
    pub unmatched_primary: u32,
    pub unmatched_secondary: u32,
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
        best: s.best, partial: s.partial, unreliable: s.unreliable,
        unmatched_primary: s.unmatched_primary, unmatched_secondary: s.unmatched_secondary,
        avg_ratio: s.avg_ratio, total_nodes_primary: s.total_nodes_primary,
        total_nodes_secondary: s.total_nodes_secondary, total_edges_primary: s.total_edges_primary,
        total_edges_secondary: s.total_edges_secondary, ratio_distribution: s.ratio_distribution,
    })
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

fn find_soff_ffi_path() -> Result<std::path::PathBuf, String> {
    let exe_dir = std::env::current_exe()
        .map_err(|e| e.to_string())?
        .parent()
        .ok_or("no parent dir")?
        .to_path_buf();

    let name = if cfg!(windows) { "soff_ffi.dll" }
        else if cfg!(target_os = "macos") { "libsoff_ffi.dylib" }
        else { "libsoff_ffi.so" };

    let candidate = exe_dir.join(name);
    if candidate.exists() { return Ok(candidate); }

    let resources = exe_dir.join("resources").join(name);
    if resources.exists() { return Ok(resources); }

    // Dev: xmake build output
    let dev_candidates = [
        exe_dir.join("../../../../build/windows/x64/release").join(name),
        exe_dir.join("../../../../build/linux/x86_64/release").join(name),
        exe_dir.join("../../../../build/macosx/arm64/release").join(name),
        exe_dir.join("../../../../build/macosx/x86_64/release").join(name),
    ];
    for p in &dev_candidates {
        if p.exists() {
            return p.canonicalize().map_err(|e| e.to_string());
        }
    }

    Err(format!("{} not found near {}", name, exe_dir.display()))
}

struct ChannelUserdata {
    channel: tauri::ipc::Channel<String>,
}

extern "C" fn progress_callback(json_line: *const c_char, userdata: *mut c_void) {
    if json_line.is_null() || userdata.is_null() { return; }
    let data = unsafe { &*(userdata as *const ChannelUserdata) };
    let line = unsafe { CStr::from_ptr(json_line) }.to_string_lossy().to_string();
    let _ = data.channel.send(line);
}

#[tauri::command]
async fn run_diff(
    primary_db: String,
    secondary_db: String,
    output_path: String,
    slow: bool,
    unreliable: bool,
    channel: tauri::ipc::Channel<String>,
) -> Result<String, String> {
    let lib_path = find_soff_ffi_path()?;
    let output = output_path.clone();

    // Run in a separate thread to avoid blocking the async runtime
    let (tx, rx) = std::sync::mpsc::channel::<Result<String, String>>();
    std::thread::spawn(move || {
        let result = (|| -> Result<String, String> {
            unsafe {
                let lib = libloading::Library::new(&lib_path)
                    .map_err(|e| format!("failed to load soff_ffi: {}", e))?;

                let soff_diff_run: libloading::Symbol<unsafe extern "C" fn(
                    *const c_char, *const c_char, *const c_char,
                    *const SoffDiffOptions, SoffProgressFn, *mut c_void,
                    *mut c_char, c_int,
                ) -> c_int> = lib.get(b"soff_diff_run")
                    .map_err(|e| format!("symbol not found: {}", e))?;

                let c_primary = CString::new(primary_db).map_err(|e| e.to_string())?;
                let c_secondary = CString::new(secondary_db).map_err(|e| e.to_string())?;
                let c_output = CString::new(output.clone()).map_err(|e| e.to_string())?;

                let options = SoffDiffOptions {
                    enable_slow: if slow { 1 } else { 0 },
                    enable_unreliable: if unreliable { 1 } else { 0 },
                    enable_experimental: 0,
                    max_rows: 1000000,
                    timeout_seconds: 300,
                };

                let mut error_buf = vec![0u8; 1024];
                let userdata = ChannelUserdata { channel };
                let userdata_ptr = &userdata as *const ChannelUserdata as *mut c_void;

                let ret = soff_diff_run(
                    c_primary.as_ptr(), c_secondary.as_ptr(), c_output.as_ptr(),
                    &options, progress_callback, userdata_ptr,
                    error_buf.as_mut_ptr() as *mut c_char, 1024,
                );

                if ret != 0 {
                    let err = CStr::from_ptr(error_buf.as_ptr() as *const c_char)
                        .to_string_lossy().to_string();
                    return Err(err);
                }
                Ok(output)
            }
        })();
        let _ = tx.send(result);
    });

    rx.recv().map_err(|e| e.to_string())?
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .plugin(tauri_plugin_dialog::init())
        .invoke_handler(tauri::generate_handler![
            open_soff,
            get_matches,
            get_unmatched,
            get_function_assembly,
            get_function_pseudocode,
            get_function_info,
            compute_diff,
            compute_aligned_diff,
            get_analyze_stats,
            extract_cfg,
            run_diff,
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
