use rusqlite::{params, Connection, OpenFlags, Result as SqlResult};
use serde::Serialize;
use std::path::{Path, PathBuf};

#[derive(Debug, Serialize, Clone)]
pub struct SoffConfig {
    pub main_db: String,
    pub diff_db: String,
    pub version: String,
    pub date: String,
    pub total_matches: u32,
    pub total_unmatched: u32,
}

#[derive(Debug, Serialize, Clone)]
pub struct DiffMatch {
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

#[derive(Debug, Serialize, Clone)]
pub struct PagedMatches {
    pub total: u32,
    pub items: Vec<DiffMatch>,
}

#[derive(Debug, Serialize, Clone)]
pub struct UnmatchedFunction {
    pub side: String,
    pub address: String,
    pub name: String,
}

#[derive(Debug, Serialize, Clone)]
pub struct PagedUnmatched {
    pub total: u32,
    pub items: Vec<UnmatchedFunction>,
}

#[derive(Debug, Serialize, Clone)]
pub struct FunctionInfo {
    pub address: String,
    pub name: String,
    pub nodes: u32,
    pub edges: u32,
    pub size: u32,
    pub instructions: u32,
}

#[derive(Debug, Serialize, Clone)]
pub struct BasicBlock {
    pub id: usize,
    pub address: Option<String>,
    pub lines: Vec<String>,
    pub successors: Vec<usize>,
}

#[derive(Debug, Serialize, Clone)]
pub struct CfgData {
    pub blocks: Vec<BasicBlock>,
}

pub(crate) fn open_read_only(path: &str) -> SqlResult<Connection> {
    Connection::open_with_flags(path, OpenFlags::SQLITE_OPEN_READ_ONLY)
}

pub(crate) fn resolve_db_path(result_path: &str, db_path: &str) -> PathBuf {
    let path = Path::new(db_path);
    if path.is_absolute() {
        return path.to_path_buf();
    }
    Path::new(result_path)
        .parent()
        .map(|parent| parent.join(path))
        .unwrap_or_else(|| path.to_path_buf())
}

fn open_read_write(path: &str) -> SqlResult<Connection> {
    Connection::open_with_flags(path, OpenFlags::SQLITE_OPEN_READ_WRITE)
}

fn normalize_match_type(match_type: &str) -> SqlResult<Option<&str>> {
    match match_type {
        "all" => Ok(None),
        "best" | "partial" | "unreliable" | "multimatch" => Ok(Some(match_type)),
        _ => Err(rusqlite::Error::InvalidParameterName(format!(
            "invalid match type: {match_type}"
        ))),
    }
}

fn normalize_limit(limit: u32) -> i64 {
    i64::from(limit.min(50_000))
}

fn normalize_offset(offset: u32) -> i64 {
    i64::from(offset)
}

pub fn load_soff_config(path: &str) -> SqlResult<SoffConfig> {
    let conn = open_read_only(path)?;
    let mut stmt = conn.prepare("SELECT main_db, diff_db, version, date FROM config LIMIT 1")?;
    let config = stmt.query_row([], |row| {
        let stored_main_db: String = row.get(0)?;
        let stored_diff_db: String = row.get(1)?;
        Ok(SoffConfig {
            main_db: resolve_db_path(path, &stored_main_db)
                .to_string_lossy()
                .into_owned(),
            diff_db: resolve_db_path(path, &stored_diff_db)
                .to_string_lossy()
                .into_owned(),
            version: row.get(2)?,
            date: row.get(3)?,
            total_matches: 0,
            total_unmatched: 0,
        })
    })?;
    let total_matches: u32 = conn.query_row("SELECT count(*) FROM results", [], |r| r.get(0))?;
    let total_unmatched: u32 =
        conn.query_row("SELECT count(*) FROM unmatched", [], |r| r.get(0))?;
    Ok(SoffConfig {
        total_matches,
        total_unmatched,
        ..config
    })
}

pub fn update_soff_paths(path: &str, main_db: &str, diff_db: &str) -> SqlResult<SoffConfig> {
    let conn = open_read_write(path)?;
    let changed = conn.execute(
        "UPDATE config SET main_db = ?1, diff_db = ?2 WHERE rowid = (SELECT rowid FROM config LIMIT 1)",
        params![main_db, diff_db],
    )?;
    if changed == 0 {
        conn.execute(
            "INSERT INTO config (main_db, diff_db, version, date) VALUES (?1, ?2, '3.4', '')",
            params![main_db, diff_db],
        )?;
    }
    drop(conn);
    load_soff_config(path)
}

pub fn query_matches(
    path: &str,
    match_type: &str,
    limit: u32,
    offset: u32,
) -> SqlResult<PagedMatches> {
    let conn = open_read_only(path)?;
    let limit = normalize_limit(limit);
    let offset = normalize_offset(offset);
    let match_type = normalize_match_type(match_type)?;
    let total: u32;
    let items = if let Some(t) = match_type {
        total = conn.query_row("SELECT count(*) FROM results WHERE type = ?1", [t], |r| {
            r.get(0)
        })?;
        let mut stmt = conn.prepare(
            "SELECT type, address, name, address2, name2, ratio, nodes1, nodes2, description \
             FROM results WHERE type = ?1 ORDER BY ratio ASC, line LIMIT ?2 OFFSET ?3",
        )?;
        let rows = stmt.query_map(params![t, limit, offset], read_match)?;
        rows.collect::<SqlResult<Vec<_>>>()?
    } else {
        total = conn.query_row("SELECT count(*) FROM results", [], |r| r.get(0))?;
        let mut stmt = conn.prepare(
            "SELECT type, address, name, address2, name2, ratio, nodes1, nodes2, description \
             FROM results ORDER BY ratio ASC, line LIMIT ?1 OFFSET ?2",
        )?;
        let rows = stmt.query_map(params![limit, offset], read_match)?;
        rows.collect::<SqlResult<Vec<_>>>()?
    };
    Ok(PagedMatches { total, items })
}

pub fn search_matches(
    path: &str,
    query: &str,
    match_type: &str,
    limit: u32,
    offset: u32,
) -> SqlResult<PagedMatches> {
    let conn = open_read_only(path)?;
    let limit = normalize_limit(limit);
    let offset = normalize_offset(offset);
    let pattern = format!("%{}%", query.replace('%', "\\%").replace('_', "\\_"));
    let match_type = normalize_match_type(match_type)?;
    let total: u32;
    let items = if let Some(t) = match_type {
        total = conn.query_row(
            "SELECT count(*) FROM results WHERE type = ?1 AND (name LIKE ?2 ESCAPE '\\' OR name2 LIKE ?2 ESCAPE '\\' \
             OR address LIKE ?2 ESCAPE '\\' OR address2 LIKE ?2 ESCAPE '\\')",
            params![t, pattern],
            |r| r.get(0),
        )?;
        let mut stmt = conn.prepare(
            "SELECT type, address, name, address2, name2, ratio, nodes1, nodes2, description \
             FROM results WHERE type = ?1 AND (name LIKE ?2 ESCAPE '\\' OR name2 LIKE ?2 ESCAPE '\\' \
             OR address LIKE ?2 ESCAPE '\\' OR address2 LIKE ?2 ESCAPE '\\') \
             ORDER BY ratio ASC LIMIT ?3 OFFSET ?4",
        )?;
        let rows = stmt.query_map(params![t, pattern, limit, offset], read_match)?;
        rows.collect::<SqlResult<Vec<_>>>()?
    } else {
        total = conn.query_row(
            "SELECT count(*) FROM results WHERE (name LIKE ?1 ESCAPE '\\' OR name2 LIKE ?1 ESCAPE '\\' \
             OR address LIKE ?1 ESCAPE '\\' OR address2 LIKE ?1 ESCAPE '\\')",
            params![pattern],
            |r| r.get(0),
        )?;
        let mut stmt = conn.prepare(
            "SELECT type, address, name, address2, name2, ratio, nodes1, nodes2, description \
             FROM results WHERE (name LIKE ?1 ESCAPE '\\' OR name2 LIKE ?1 ESCAPE '\\' \
             OR address LIKE ?1 ESCAPE '\\' OR address2 LIKE ?1 ESCAPE '\\') \
             ORDER BY ratio ASC LIMIT ?2 OFFSET ?3",
        )?;
        let rows = stmt.query_map(params![pattern, limit, offset], read_match)?;
        rows.collect::<SqlResult<Vec<_>>>()?
    };
    Ok(PagedMatches { total, items })
}

pub fn search_unmatched(
    path: &str,
    query: &str,
    limit: u32,
    offset: u32,
) -> SqlResult<PagedUnmatched> {
    let conn = open_read_only(path)?;
    let limit = normalize_limit(limit);
    let offset = normalize_offset(offset);
    let pattern = format!("%{}%", query.replace('%', "\\%").replace('_', "\\_"));
    let total: u32 = conn.query_row(
        "SELECT count(*) FROM unmatched \
         WHERE (name LIKE ?1 ESCAPE '\\' OR address LIKE ?1 ESCAPE '\\')",
        params![pattern],
        |r| r.get(0),
    )?;
    let mut stmt = conn.prepare(
        "SELECT type, address, name FROM unmatched \
         WHERE (name LIKE ?1 ESCAPE '\\' OR address LIKE ?1 ESCAPE '\\') \
         ORDER BY line LIMIT ?2 OFFSET ?3",
    )?;
    let rows = stmt.query_map(rusqlite::params![pattern, limit, offset], read_unmatched)?;
    let items = rows.collect::<SqlResult<Vec<_>>>()?;
    Ok(PagedUnmatched { total, items })
}

pub fn query_unmatched(path: &str, limit: u32, offset: u32) -> SqlResult<PagedUnmatched> {
    let conn = open_read_only(path)?;
    let total: u32 = conn.query_row("SELECT count(*) FROM unmatched", [], |r| r.get(0))?;
    let mut stmt =
        conn.prepare("SELECT type, address, name FROM unmatched ORDER BY line LIMIT ?1 OFFSET ?2")?;
    let rows = stmt.query_map(
        params![normalize_limit(limit), normalize_offset(offset)],
        read_unmatched,
    )?;
    let items = rows.collect::<SqlResult<Vec<_>>>()?;
    Ok(PagedUnmatched { total, items })
}

pub fn query_function_column(db_path: &str, address: &str, column: &str) -> SqlResult<String> {
    let conn = open_read_only(db_path)?;
    if !matches!(column, "assembly" | "pseudocode") {
        return Err(rusqlite::Error::InvalidParameterName(format!(
            "invalid function column: {column}"
        )));
    }
    let sql = format!(
        "SELECT {} FROM functions WHERE address = ?1 LIMIT 1",
        column
    );
    conn.query_row(&sql, [address], |row| row.get(0))
}

pub fn query_function_info(db_path: &str, address: &str) -> SqlResult<FunctionInfo> {
    let conn = open_read_only(db_path)?;
    conn.query_row(
        "SELECT address, name, nodes, edges, size, instructions \
         FROM functions WHERE address = ?1 LIMIT 1",
        [address],
        |row| {
            Ok(FunctionInfo {
                address: row.get(0)?,
                name: row.get(1)?,
                nodes: row.get(2)?,
                edges: row.get(3)?,
                size: row.get(4)?,
                instructions: row.get(5)?,
            })
        },
    )
}

fn read_match(row: &rusqlite::Row<'_>) -> SqlResult<DiffMatch> {
    Ok(DiffMatch {
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

fn read_unmatched(row: &rusqlite::Row<'_>) -> SqlResult<UnmatchedFunction> {
    Ok(UnmatchedFunction {
        side: row.get(0)?,
        address: row.get(1)?,
        name: row.get(2)?,
    })
}

fn has_table(conn: &Connection, table: &str) -> SqlResult<bool> {
    let count: u32 = conn.query_row(
        "SELECT count(*) FROM sqlite_master WHERE type = 'table' AND name = ?1",
        [table],
        |row| row.get(0),
    )?;
    Ok(count > 0)
}

pub fn query_cfg(db_path: &str, address: &str) -> SqlResult<CfgData> {
    let conn = open_read_only(db_path)?;
    if has_table(&conn, "function_bblocks")?
        && has_table(&conn, "basic_blocks")?
        && has_table(&conn, "bb_instructions")?
        && has_table(&conn, "bb_relations")?
    {
        let cfg = query_cfg_tables(&conn, address)?;
        if !cfg.blocks.is_empty() {
            return Ok(cfg);
        }
    }
    let asm: String = conn.query_row(
        "SELECT assembly FROM functions WHERE address = ?1 LIMIT 1",
        [address],
        |row| row.get(0),
    )?;
    Ok(parse_cfg_from_asm(&asm))
}

fn query_cfg_tables(conn: &Connection, address: &str) -> SqlResult<CfgData> {
    struct DbBlock {
        rowid: i64,
        address: Option<String>,
    }

    let mut stmt = conn.prepare(
        "SELECT bb.id, bb.address \
         FROM function_bblocks fb \
         INNER JOIN functions f ON f.id = fb.function_id \
         INNER JOIN basic_blocks bb ON bb.id = fb.basic_block_id \
         WHERE f.address = ?1 AND coalesce(fb.asm_type, '') = '' \
         ORDER BY bb.num, bb.id",
    )?;
    let rows = stmt.query_map([address], |row| {
        Ok(DbBlock {
            rowid: row.get(0)?,
            address: row.get(1)?,
        })
    })?;
    let db_blocks = rows.collect::<SqlResult<Vec<_>>>()?;

    let mut index_by_rowid = std::collections::HashMap::new();
    for (index, block) in db_blocks.iter().enumerate() {
        index_by_rowid.insert(block.rowid, index);
    }

    let mut blocks = Vec::with_capacity(db_blocks.len());
    for (index, block) in db_blocks.iter().enumerate() {
        let mut instruction_stmt = conn.prepare(
            "SELECT i.disasm \
             FROM bb_instructions bi \
             INNER JOIN instructions i ON i.id = bi.instruction_id \
             WHERE bi.basic_block_id = ?1 AND coalesce(i.asm_type, '') = '' \
             ORDER BY bi.id",
        )?;
        let lines = instruction_stmt
            .query_map([block.rowid], |row| row.get::<_, String>(0))?
            .collect::<SqlResult<Vec<_>>>()?;

        let mut successor_stmt =
            conn.prepare("SELECT child_id FROM bb_relations WHERE parent_id = ?1 ORDER BY id")?;
        let successor_rows = successor_stmt
            .query_map([block.rowid], |row| row.get::<_, i64>(0))?
            .collect::<SqlResult<Vec<_>>>()?;
        let mut successors = Vec::new();
        for child_id in successor_rows {
            if let Some(child_index) = index_by_rowid.get(&child_id) {
                successors.push(*child_index);
            }
        }
        successors.sort_unstable();
        successors.dedup();

        blocks.push(BasicBlock {
            id: index,
            address: block.address.clone(),
            lines,
            successors,
        });
    }
    Ok(CfgData { blocks })
}

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

pub fn query_analyze_stats(path: &str) -> SqlResult<AnalyzeStats> {
    let conn = open_read_only(path)?;

    let best: u32 = conn.query_row("SELECT count(*) FROM results WHERE type='best'", [], |r| {
        r.get(0)
    })?;
    let partial: u32 = conn.query_row(
        "SELECT count(*) FROM results WHERE type='partial'",
        [],
        |r| r.get(0),
    )?;
    let unreliable: u32 = conn.query_row(
        "SELECT count(*) FROM results WHERE type='unreliable'",
        [],
        |r| r.get(0),
    )?;
    let unmatched_primary: u32 = conn.query_row(
        "SELECT count(*) FROM unmatched WHERE type='primary'",
        [],
        |r| r.get(0),
    )?;
    let unmatched_secondary: u32 = conn.query_row(
        "SELECT count(*) FROM unmatched WHERE type='secondary'",
        [],
        |r| r.get(0),
    )?;
    let avg_ratio: f64 =
        conn.query_row("SELECT coalesce(avg(ratio), 0) FROM results", [], |r| {
            r.get(0)
        })?;

    let config = load_soff_config(path)?;
    let primary_conn = open_read_only(&config.main_db)?;
    let secondary_conn = open_read_only(&config.diff_db)?;
    let primary_functions: u32 =
        primary_conn.query_row("SELECT count(*) FROM functions", [], |r| r.get(0))?;
    let secondary_functions: u32 =
        secondary_conn.query_row("SELECT count(*) FROM functions", [], |r| r.get(0))?;
    let total_nodes_primary: u64 =
        primary_conn.query_row("SELECT coalesce(sum(nodes), 0) FROM functions", [], |r| {
            r.get(0)
        })?;
    let total_nodes_secondary: u64 =
        secondary_conn.query_row("SELECT coalesce(sum(nodes), 0) FROM functions", [], |r| {
            r.get(0)
        })?;
    let total_edges_primary: u64 =
        primary_conn.query_row("SELECT coalesce(sum(edges), 0) FROM functions", [], |r| {
            r.get(0)
        })?;
    let total_edges_secondary: u64 =
        secondary_conn.query_row("SELECT coalesce(sum(edges), 0) FROM functions", [], |r| {
            r.get(0)
        })?;

    // Ratio distribution: 10 buckets
    let mut dist = vec![0u32; 10];
    let mut stmt = conn.prepare("SELECT ratio FROM results")?;
    let ratios = stmt.query_map([], |r| r.get::<_, f64>(0))?;
    for ratio in ratios {
        let r = ratio?;
        let bucket = ((r * 10.0).floor() as usize).min(9);
        dist[bucket] += 1;
    }

    Ok(AnalyzeStats {
        best,
        partial,
        unreliable,
        unmatched_primary,
        unmatched_secondary,
        primary_functions,
        secondary_functions,
        avg_ratio,
        total_nodes_primary,
        total_nodes_secondary,
        total_edges_primary,
        total_edges_secondary,
        ratio_distribution: dist,
    })
}

fn parse_cfg_from_asm(asm: &str) -> CfgData {
    let lines: Vec<&str> = asm.lines().collect();
    if lines.is_empty() {
        return CfgData { blocks: vec![] };
    }

    let is_jump = |m: &str| -> bool {
        matches!(
            m,
            "jmp"
                | "je"
                | "jne"
                | "jz"
                | "jnz"
                | "jg"
                | "jl"
                | "jge"
                | "jle"
                | "ja"
                | "jb"
                | "jae"
                | "jbe"
                | "jc"
                | "jnc"
                | "jo"
                | "jno"
                | "js"
                | "jns"
                | "jp"
                | "jnp"
                | "jcxz"
                | "jecxz"
                | "jrcxz"
                | "loop"
        )
    };
    let is_ret = |m: &str| -> bool { matches!(m, "ret" | "retn") };

    let mut label_lines: std::collections::HashMap<String, usize> =
        std::collections::HashMap::new();
    for (i, line) in lines.iter().enumerate() {
        let t = line.trim();
        if let Some(colon_pos) = t.find(':') {
            let label = t[..colon_pos].trim().to_lowercase();
            if !label.is_empty() && !label.contains(' ') {
                label_lines.insert(label, i);
            }
        }
    }

    let mut block_starts: Vec<usize> = vec![0];
    for (i, line) in lines.iter().enumerate() {
        let t = line.trim();
        if t.contains(':') && i > 0 {
            let colon_pos = t.find(':').unwrap();
            let before = t[..colon_pos].trim();
            if !before.is_empty() && !before.contains(' ') && !block_starts.contains(&i) {
                block_starts.push(i);
            }
        }

        let mnemonic = t.split_whitespace().next().unwrap_or("");
        let mnemonic = if mnemonic.ends_with(':') {
            t.split_whitespace().nth(1).unwrap_or("")
        } else {
            mnemonic
        }
        .to_lowercase();

        if (is_jump(&mnemonic) || is_ret(&mnemonic))
            && i + 1 < lines.len()
            && !block_starts.contains(&(i + 1))
        {
            block_starts.push(i + 1);
        }
    }
    block_starts.sort();
    block_starts.dedup();

    let mut blocks: Vec<BasicBlock> = Vec::new();
    for (bi, &start) in block_starts.iter().enumerate() {
        let end = block_starts.get(bi + 1).copied().unwrap_or(lines.len());
        let block_lines: Vec<String> = (start..end).map(|i| lines[i].to_string()).collect();
        let mut succs: Vec<usize> = Vec::new();

        if let Some(last) = block_lines.last() {
            let t = last.trim();
            let parts: Vec<&str> = t.split_whitespace().collect();
            let (mn_idx, mn) = if parts.first().is_some_and(|p| p.ends_with(':')) {
                (1, parts.get(1).unwrap_or(&"").to_lowercase())
            } else {
                (0, parts.first().unwrap_or(&"").to_lowercase())
            };

            if is_ret(&mn) {
            } else if is_jump(&mn) {
                let operand = parts.get(mn_idx + 1).unwrap_or(&"");
                let target_name = operand
                    .trim_start_matches("short ")
                    .trim_start_matches("near ")
                    .trim_end_matches(',')
                    .to_lowercase();
                let target_name = if target_name == "short" || target_name == "near" {
                    parts
                        .get(mn_idx + 2)
                        .unwrap_or(&"")
                        .trim_end_matches(',')
                        .to_lowercase()
                } else {
                    target_name
                };

                if let Some(&target_line) = label_lines.get(&target_name) {
                    for (ti, &ts) in block_starts.iter().enumerate() {
                        if ts == target_line {
                            succs.push(ti);
                            break;
                        }
                    }
                }
                if mn != "jmp" && bi + 1 < block_starts.len() {
                    succs.push(bi + 1);
                }
            } else if bi + 1 < block_starts.len() {
                succs.push(bi + 1);
            }
        }
        succs.sort();
        succs.dedup();
        blocks.push(BasicBlock {
            id: bi,
            address: None,
            lines: block_lines,
            successors: succs,
        });
    }

    CfgData { blocks }
}

#[cfg(test)]
mod tests {
    use super::resolve_db_path;
    use std::path::PathBuf;

    #[test]
    fn resolves_relative_database_paths_from_result_directory() {
        assert_eq!(
            resolve_db_path("workspace/results/sample.soff", "exports/main.sqlite"),
            PathBuf::from("workspace/results/exports/main.sqlite")
        );
    }

    #[test]
    fn preserves_absolute_database_paths() {
        let absolute = std::env::current_dir().unwrap().join("main.sqlite");
        assert_eq!(
            resolve_db_path("workspace/results/sample.soff", &absolute.to_string_lossy()),
            absolute
        );
    }
}
