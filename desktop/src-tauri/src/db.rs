use rusqlite::{Connection, Result as SqlResult};
use serde::Serialize;

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
pub struct UnmatchedFunction {
    pub side: String,
    pub address: String,
    pub name: String,
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

pub fn load_soff_config(path: &str) -> SqlResult<SoffConfig> {
    let conn = Connection::open(path)?;
    let mut stmt = conn.prepare(
        "SELECT main_db, diff_db, version, date FROM config LIMIT 1",
    )?;
    let config = stmt.query_row([], |row| {
        Ok(SoffConfig {
            main_db: row.get(0)?,
            diff_db: row.get(1)?,
            version: row.get(2)?,
            date: row.get(3)?,
            total_matches: 0,
            total_unmatched: 0,
        })
    })?;
    let total_matches: u32 = conn.query_row(
        "SELECT count(*) FROM results", [], |r| r.get(0),
    )?;
    let total_unmatched: u32 = conn.query_row(
        "SELECT count(*) FROM unmatched", [], |r| r.get(0),
    )?;
    Ok(SoffConfig { total_matches, total_unmatched, ..config })
}

pub fn query_matches(path: &str, match_type: &str, limit: u32, offset: u32) -> SqlResult<Vec<DiffMatch>> {
    let conn = Connection::open(path)?;
    let sql = if match_type == "all" {
        format!(
            "SELECT type, address, name, address2, name2, ratio, nodes1, nodes2, description \
             FROM results ORDER BY ratio ASC, line LIMIT {} OFFSET {}", limit, offset
        )
    } else {
        format!(
            "SELECT type, address, name, address2, name2, ratio, nodes1, nodes2, description \
             FROM results WHERE type = '{}' ORDER BY ratio ASC, line LIMIT {} OFFSET {}",
            match_type, limit, offset
        )
    };
    let mut stmt = conn.prepare(&sql)?;
    let rows = stmt.query_map([], |row| {
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
    })?;
    rows.collect()
}

pub fn search_matches(path: &str, query: &str, match_type: &str, limit: u32) -> SqlResult<Vec<DiffMatch>> {
    let conn = Connection::open(path)?;
    let pattern = format!("%{}%", query.replace('%', "\\%").replace('_', "\\_"));
    let sql = if match_type == "all" {
        "SELECT type, address, name, address2, name2, ratio, nodes1, nodes2, description \
         FROM results WHERE (name LIKE ?1 ESCAPE '\\' OR name2 LIKE ?1 ESCAPE '\\' \
         OR address LIKE ?1 ESCAPE '\\' OR address2 LIKE ?1 ESCAPE '\\') \
         ORDER BY ratio ASC LIMIT ?2".to_string()
    } else {
        format!(
            "SELECT type, address, name, address2, name2, ratio, nodes1, nodes2, description \
             FROM results WHERE type = '{}' AND (name LIKE ?1 ESCAPE '\\' OR name2 LIKE ?1 ESCAPE '\\' \
             OR address LIKE ?1 ESCAPE '\\' OR address2 LIKE ?1 ESCAPE '\\') \
             ORDER BY ratio ASC LIMIT ?2", match_type)
    };
    let mut stmt = conn.prepare(&sql)?;
    let rows = stmt.query_map(rusqlite::params![pattern, limit], |row| {
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
    })?;
    rows.collect()
}

pub fn search_unmatched(path: &str, query: &str, limit: u32) -> SqlResult<Vec<UnmatchedFunction>> {
    let conn = Connection::open(path)?;
    let pattern = format!("%{}%", query.replace('%', "\\%").replace('_', "\\_"));
    let mut stmt = conn.prepare(
        "SELECT type, address, name FROM unmatched \
         WHERE (name LIKE ?1 ESCAPE '\\' OR address LIKE ?1 ESCAPE '\\') \
         ORDER BY line LIMIT ?2"
    )?;
    let rows = stmt.query_map(rusqlite::params![pattern, limit], |row| {
        Ok(UnmatchedFunction {
            side: row.get(0)?,
            address: row.get(1)?,
            name: row.get(2)?,
        })
    })?;
    rows.collect()
}

pub fn query_unmatched(path: &str, limit: u32, offset: u32) -> SqlResult<Vec<UnmatchedFunction>> {
    let conn = Connection::open(path)?;
    let mut stmt = conn.prepare(&format!(
        "SELECT type, address, name FROM unmatched ORDER BY line LIMIT {} OFFSET {}",
        limit, offset
    ))?;
    let rows = stmt.query_map([], |row| {
        Ok(UnmatchedFunction {
            side: row.get(0)?,
            address: row.get(1)?,
            name: row.get(2)?,
        })
    })?;
    rows.collect()
}

pub fn query_function_column(db_path: &str, address: &str, column: &str) -> SqlResult<String> {
    let conn = Connection::open(db_path)?;
    let sql = format!(
        "SELECT {} FROM functions WHERE address = ?1 LIMIT 1", column
    );
    conn.query_row(&sql, [address], |row| row.get(0))
}

pub fn query_function_info(db_path: &str, address: &str) -> SqlResult<FunctionInfo> {
    let conn = Connection::open(db_path)?;
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

pub fn query_analyze_stats(path: &str) -> SqlResult<AnalyzeStats> {
    let conn = Connection::open(path)?;

    let best: u32 = conn.query_row("SELECT count(*) FROM results WHERE type='best'", [], |r| r.get(0))?;
    let partial: u32 = conn.query_row("SELECT count(*) FROM results WHERE type='partial'", [], |r| r.get(0))?;
    let unreliable: u32 = conn.query_row("SELECT count(*) FROM results WHERE type='unreliable'", [], |r| r.get(0))?;
    let unmatched_primary: u32 = conn.query_row("SELECT count(*) FROM unmatched WHERE type='primary'", [], |r| r.get(0))?;
    let unmatched_secondary: u32 = conn.query_row("SELECT count(*) FROM unmatched WHERE type='secondary'", [], |r| r.get(0))?;
    let avg_ratio: f64 = conn.query_row("SELECT coalesce(avg(ratio), 0) FROM results", [], |r| r.get(0))?;

    let total_nodes_primary: u64 = conn.query_row("SELECT coalesce(sum(nodes1), 0) FROM results", [], |r| r.get(0))?;
    let total_nodes_secondary: u64 = conn.query_row("SELECT coalesce(sum(nodes2), 0) FROM results", [], |r| r.get(0))?;
    // edges not stored in .soff, use nodes as proxy
    let total_edges_primary = total_nodes_primary;
    let total_edges_secondary = total_nodes_secondary;

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
        best, partial, unreliable,
        unmatched_primary, unmatched_secondary,
        avg_ratio, total_nodes_primary, total_nodes_secondary,
        total_edges_primary, total_edges_secondary,
        ratio_distribution: dist,
    })
}
