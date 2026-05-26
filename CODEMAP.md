# Soff Code Map

## Architecture

```
soff/
├── include/soff/          Public headers
│   ├── analysis/model.hpp    FunctionFeature, BasicBlock, ProgramSnapshot
│   ├── core/                 Error, hooks, thread_pool, version, config
│   ├── db/                   Database, SnapshotRepository, ResultRepository, schema
│   ├── diff/                 Session, heuristics, sql_runner, ratio, propagation, ML
│   └── ui/                   html_diff, import_plan, line_diff
├── src/
│   ├── analysis/model.cpp    Snapshot validation
│   ├── cli/main.cpp          soff_cli binary (diff-db, export tools)
│   ├── core/version.cpp      Version string
│   ├── db/
│   │   ├── database.cpp      SQLite wrapper (open, execute, query)
│   │   ├── repository.cpp    Export DB read/write (functions table, 49 columns)
│   │   ├── result_repository.cpp  Result DB read/write (results/unmatched tables)
│   │   └── schema.cpp        CREATE TABLE statements
│   ├── diff/
│   │   ├── heuristics.cpp    50 SQL heuristic definitions (best/partial/unreliable)
│   │   ├── sql_runner.cpp    Execute heuristics, compute ratio, parallel scoring
│   │   ├── session.cpp       DiffSession orchestration (equal→name→heuristics→propagation)
│   │   ├── propagation.cpp   Post-heuristic propagation (name, diffing, constants, CU, affine)
│   │   ├── ratio.cpp         sequence_matcher_quick_ratio, ast_prime_difference_ratio
│   │   ├── patch_diff.cpp    Stripped binary fast path
│   │   ├── ml_features.cpp   ML feature extraction from match candidates
│   │   └── ml_model.cpp      ML model inference for post-filter
│   ├── ffi/soff_ffi.cpp      C API shared library (soff_diff_run, soff_version)
│   ├── plugin/soff_plugin.cpp IDA plugin (export, diff, import, graph UI) ~6700 lines
│   └── ui/                   HTML diff export, import plan, line diff
├── desktop/                  Tauri desktop app
│   ├── src/                  React frontend (App, DiffPage, DiffViewer, CfgView, etc.)
│   └── src-tauri/            Rust backend (db queries, diff invoke via FFI)
├── tests/                    soff_smoke test binary
└── xmake.lua                 Build config (soff_core, soff_cli, soff_ffi, soff_ida, soff_smoke)
```

## Data Flow

```
IDA Plugin (soff_plugin.cpp)
  → read_function_feature() per function
  → extract_pseudocode_features() [Hex-Rays]
  → extract_microcode_features() [Hex-Rays]
  → SnapshotRepository::save() → .sqlite (49 columns per function)

Diff Engine (session.cpp)
  → attach primary + secondary .sqlite
  → find_equal_matches() [INTERSECT]
  → find_same_name() [name matching + ratio]
  → SqlRunner::run_all() [50 heuristics, parallel ratio]
  → run_propagation() [5 passes, iterative]
  → resolve_multimatches()
  → MlModel::filter_matches() [optional]
  → ResultRepository::save() → .soff

Desktop Viewer (desktop/)
  → open_soff → load config + stats
  → get_matches → query results table
  → DiffViewer → get_function_pseudocode/assembly from export DBs
  → CfgView → extract_cfg from export DBs
```

## Key Types

- `Address` = uint64_t
- `FunctionFeature` — 49 fields (model.hpp:39)
- `BasicBlock` — start, end, instructions[], successors[]
- `InstructionFeature` — address, disassembly, mnemonic, comments, operands
- `HeuristicDefinition` — name, category, ratio_mode, flags, min_ratio, sql
- `DiffCandidate` — primary/secondary addresses, names, 30+ match columns
- `MatchResult` — kind, addresses, names, ratio, description, line

## Diff Session Steps (session.cpp)

1. Open + attach databases
2. Detect same_processor
3. Auto-tune (disable slow if >4001 functions, relaxed ratio if >8001)
4. find_equal_matches (INTERSECT on 7 fields)
5. Stripped binary fast path (99%+ address+hash match → skip heuristics)
6. find_same_name (name + ratio >= 0.8)
7. Patchdiff fast path (90%+ name match → skip heuristics)
8. SQL heuristics (50 in order: best → partial → unreliable)
9. Propagation (5 passes, iterative until convergence)
10. resolve_multimatches
11. ML model filter (optional)
12. final_pass_unmatched
13. Save results

## Known Gaps (vs BinDiff)

- No basic-block-level matching (data exists in `blocks` + `bb_instructions`)
- No disambiguation (feature collision → first match wins)
- No call-reference propagation (BinDiff matches callees by position)
- Text ratio (bag-of-lines) instead of structural similarity
- MD Index underutilized (only as ratio boost, not as primary match key)
