# soff

Binary diffing engine for IDA Pro. Exports function-level features from IDA databases and computes semantic diffs between two binaries using 50+ heuristics.

## Build

Requires [xmake](https://xmake.io/) and a C++17 compiler.

```bash
xmake require -y
xmake config --ida_plugin=y
xmake build
```

Outputs:
- `soff.dll` (Windows) / `soff.so` (Linux) / `soff.dylib` (macOS) — IDA plugin
- `soff_cli` — standalone CLI tool

## Install

Copy the plugin to your IDA plugins directory:

```bash
# Windows
copy build\windows\x64\release\soff.dll "%IDADIR%\plugins\"

# Linux
cp build/linux/x86_64/release/soff.so "$IDADIR/plugins/"

# macOS
cp build/macosx/arm64/release/soff.dylib "$IDADIR/plugins/"
```

## Usage

### Export

In IDA menu: `Soff → Export current IDB`

Exports function features (assembly, pseudocode, control flow, hashes) to a SQLite database.

### Diff

In IDA menu: `Soff → Diff SQLite databases`

Select two exported databases to compare. Results show matched, partially matched, and unmatched functions with similarity ratios.

### Other menu items

- `Soff → Load Diff Results` — load a previously saved .soff result file
- `Soff → Save Diff Results As` — save current diff results
- `Soff → Import Diff Results` — import results into IDA (rename, set types)
- `Soff → Local Function Diff` — diff two functions within the same IDB

### CLI

```bash
soff_cli diff primary.sqlite secondary.sqlite -o results.soff
```

## Architecture

```
include/soff/
├── analysis/   # FunctionFeature model
├── core/       # Error types, config, thread pool
├── db/         # SQLite database layer
├── diff/       # Heuristics, ratio, propagation, session
└── ui/         # HTML diff, line diff, import plan

src/
├── plugin/     # IDA plugin (export + diff UI)
├── cli/        # Standalone CLI
├── db/         # Repository, schema
├── diff/       # SQL heuristics, ratio computation
└── ui/         # Viewers, HTML rendering
```

## Key Features

- 50 SQL-based matching heuristics (exact, partial, unreliable)
- Parallel ratio computation via thread pool
- Boost unordered_flat_map for fast hash lookups
- LCS-based line diff with 2000-line guard
- Incremental export with crash recovery
- Cross-platform (Windows, Linux, macOS)

## Dependencies

- [IDA SDK 9.3](https://hex-rays.com/ida-sdk/) (included)
- [Boost](https://www.boost.org/) (header-only: unordered, uuid)
- SQLite3 (runtime, dynamically loaded)

## License

See [IDA SDK LICENSE](ida-sdk-93-main/LICENSE) for SDK terms.
