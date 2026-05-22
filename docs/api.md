# Soff API Reference

## Database Schema

### Export Database (.sqlite)

Each exported IDB produces one .sqlite file with a `functions` table:

| Column | Type | Description |
|--------|------|-------------|
| address | TEXT | Function start address (decimal) |
| name | TEXT | Function name |
| nodes | INTEGER | Basic block count |
| edges | INTEGER | CFG edge count |
| size | INTEGER | Function byte size |
| instructions | INTEGER | Instruction count |
| mnemonics | TEXT | Space-separated mnemonic list |
| assembly | TEXT | Full disassembly text |
| pseudocode | TEXT | Hex-Rays decompiled C code |
| pseudocode_hash | TEXT | MD5 of cleaned pseudocode |
| strongly_connected | INTEGER | SCC count |
| strongly_connected_spp | TEXT | SCC small-primes-product |
| loops | INTEGER | Loop count |
| constants | TEXT | JSON array of numeric constants |
| md_index | REAL | Metadata complexity index |
| kgh_hash | TEXT | Graph structure hash |
| bytes_hash | TEXT | MD5 of function bytes |
| source_file | TEXT | Source file (if available) |

### Result Database (.soff)

Diff results stored as SQLite:

```sql
-- Source databases
CREATE TABLE config (main_db TEXT, diff_db TEXT, version TEXT, date TEXT);

-- Matched function pairs
CREATE TABLE results (
    type TEXT,        -- 'best' | 'partial' | 'unreliable'
    line INTEGER,     -- display order
    address TEXT,     -- primary address (decimal)
    name TEXT,        -- primary name
    address2 TEXT,    -- secondary address
    name2 TEXT,       -- secondary name
    ratio REAL,       -- similarity 0.0-1.0
    nodes1 INTEGER,
    nodes2 INTEGER,
    description TEXT  -- heuristic that matched
);
CREATE UNIQUE INDEX uq_results ON results(address, address2);

-- Functions without a match
CREATE TABLE unmatched (
    type TEXT,        -- 'primary' | 'secondary'
    line INTEGER,
    address TEXT,
    name TEXT
);
```

## CLI (soff_cli)

```bash
# Diff two exported databases
soff_cli diff-db <primary.sqlite> <secondary.sqlite> \
    --out <result.soff> \
    --slow \
    --unreliable \
    --ml-model <model.json>

# Environment variables
DIAPHORA_EXPORT_FILE=output.sqlite    # override export path
DIAPHORA_AUTO=1                       # headless mode in IDA
```

## MCP Tools (IDA-MCP)

Available via `api_soff.py` in the IDA-MCP gateway:

### soff_export (requires IDA)
Export current IDB to SQLite.
```
soff_export(output_path="C:/out.sqlite")
→ {"exported": 84, "skipped": 5, "total": 89, "output": "..."}
```

### soff_diff_results (no IDA needed)
Query matches/unmatched from a .soff file.
```
soff_diff_results(result_path="r.soff", match_type="best", limit=100, offset=0)
→ {"main_db": "...", "diff_db": "...", "total": 42, "items": [...]}
```

### soff_diff_asm (no IDA needed)
Unified diff of two functions' assembly.
```
soff_diff_asm(main_db="a.sqlite", diff_db="b.sqlite",
              primary_addr="0x140001000", secondary_addr="0x140001000")
→ " push rbp\n mov rbp, rsp\n-mov ecx, [rsp+8]\n+mov ecx, [rsp+10h]\n"
```

### soff_diff_pseudo (no IDA needed)
Unified diff of two functions' pseudocode.
```
soff_diff_pseudo(main_db="a.sqlite", diff_db="b.sqlite",
                 primary_addr="5368709120", secondary_addr="5368709120")
→ unified diff text
```

Address formats accepted: `0x140001000`, `140001000h`, `5368709120` (decimal).

## Diff Engine

### Heuristic Pipeline

1. **Exact matches** — identical hash (bytes, pseudocode, graph)
2. **Name matching** — same function name (pre-heuristic pass)
3. **SQL heuristics** — 50+ queries comparing features
4. **Propagation** — caller/callee relationship propagation
5. **ML post-filter** — optional random forest to reject false positives

### Match Types

| Type | Meaning |
|------|---------|
| best | High confidence (ratio >= threshold, unique match) |
| partial | Medium confidence (ratio above minimum) |
| unreliable | Low confidence (experimental heuristics) |

### Auto-Tuning

| Condition | Action |
|-----------|--------|
| total_functions >= 4001 | Disable slow heuristics |
| total_functions >= 8001 | Enable relaxed_ratio mode |
| name_match >= 90% | Patchdiff fast path (skip heuristics) |

## Desktop App

Tauri 2 + React + TypeScript. Reads .soff and .sqlite directly via rusqlite.

### Tauri Commands

| Command | Parameters | Returns |
|---------|-----------|---------|
| open_soff | path | SoffConfig (main_db, diff_db, totals) |
| get_matches | path, match_type, limit, offset | Vec<DiffMatch> |
| get_unmatched | path, limit, offset | Vec<UnmatchedFunction> |
| get_function_assembly | db_path, address | String |
| get_function_pseudocode | db_path, address | String |
| get_function_info | db_path, address | FunctionInfo |
| compute_diff | left, right | Vec<String> (unified diff lines) |

### Build

```bash
cd desktop
bun install
bun run tauri dev    # development
bun run tauri build  # production (.exe/.msi)
```
