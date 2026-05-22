# Soff — Binary Diff Engine

High-performance binary diffing toolkit. Export IDA databases to SQLite, diff two exports, view results in a desktop app or via MCP.

## Quick Start

```bash
# Build (requires xmake, IDA SDK for plugin)
xmake build

# CLI: diff two exports
soff_cli diff-db primary.sqlite secondary.sqlite --out result.soff

# Desktop app
cd desktop && bun install && bun run tauri dev
```

## Architecture

```
soff/
├── include/soff/         Public C++ headers
│   ├── analysis/         Feature model (FunctionFeature, ProgramSnapshot)
│   ├── core/             Error handling, hooks, version
│   ├── db/               Database + result repository interfaces
│   ├── diff/             Diff session, propagation, SQL runner, ML model
│   └── ui/               HTML diff, import plan, line diff (LCS)
├── src/
│   ├── cli/              Standalone CLI (diff-db, export stats)
│   ├── db/               SQLite repository implementation
│   ├── diff/             Diff engine (session, propagation, sql_runner)
│   └── plugin/           IDA plugin (export, UI, graph viewer)
├── desktop/              Tauri + React desktop application
│   ├── src-tauri/        Rust backend (rusqlite, similar)
│   └── src/              React frontend (TypeScript, Tailwind)
├── tests/                Smoke tests
└── xmake.lua             Build configuration
```

## Data Flow

```
IDA Pro ──[soff plugin]──> .sqlite (function features)
                                │
                                ├──[soff_cli / desktop]──> .soff (diff results)
                                │
.sqlite + .sqlite + .soff ──[desktop / MCP]──> view diffs
```
