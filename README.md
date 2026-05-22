# Soff

High-performance binary diff engine for IDA Pro. Exports function features from IDA databases and compares them using 40+ heuristics to identify matching, modified, and unmatched functions across binary versions.

![Analyze View](img/analyze.png)

## Components

| Component | Description |
|-----------|-------------|
| `soff.dll` / `.so` / `.dylib` | IDA Pro plugin (IDA 9.0+) |
| `soff_cli` | Command-line diff tool |
| `soff-desktop` | Standalone viewer (Tauri app) |

## Installation

Copy the plugin to your IDA plugins directory:

```
# Windows
copy soff.dll "%IDADIR%\plugins\"

# Linux
cp soff.so "$IDADIR/plugins/"

# macOS
cp soff.dylib "$IDADIR/plugins/"
```

## Usage

### IDA Plugin

**Export:**

1. Open a binary in IDA, wait for auto-analysis to complete
2. `Edit → Plugins → Soff` or `Soff → Export`
3. Choose output path (`.sqlite` file)
4. Options:
   - **Use decompiler** — export Hex-Rays pseudocode (slower but enables more heuristics)
   - **Exclude library/thunk** — skip trivial wrapper functions
5. Wait for export to finish

**Diff:**

1. Export both binaries (primary and secondary) to `.sqlite` files
2. `Soff → Diff` in the primary IDA instance
3. Select the secondary `.sqlite` file
4. Results appear in the "Soff Diff Results" chooser

**Local Diff (single IDA instance):**

1. `Soff → Local Function Diff`
2. Select two exported `.sqlite` files
3. Results appear without needing both IDBs open

### CLI

```bash
# Export (headless, requires idat/idat64)
soff_cli export input.idb -o output.sqlite

# Diff two exports
soff_cli diff primary.sqlite secondary.sqlite -o results.soff

# View summary
soff_cli info results.soff
```

### Desktop Viewer

Open a `.soff` result file to browse matches interactively.

![Soff Results](img/soff.png)

![Graph View](img/graph.png)

## IDA Chooser Fields

When you run a diff in IDA, results appear in a chooser with these columns:

| Column | Description |
|--------|-------------|
| **Type** | Match confidence: `best` (high confidence), `partial` (medium), `unreliable` (low) |
| **Ratio** | Similarity score from 0.0 to 1.0. A ratio of 1.0 means identical; lower values indicate more differences |
| **Primary** | Address of the function in the primary (original) binary |
| **Primary name** | Function name in the primary binary |
| **Secondary** | Address of the matched function in the secondary (patched/updated) binary |
| **Secondary name** | Function name in the secondary binary |
| **Description** | The heuristic that produced this match (see below) |

### Match Types

| Type | Meaning |
|------|---------|
| `best` | High-confidence match. The heuristic is deterministic (hash-based) or the ratio is 1.0 |
| `partial` | Medium-confidence match. Structural similarity detected but not identical |
| `unreliable` | Low-confidence match. May be a false positive; review manually |

### Description (Heuristics)

The Description column shows which heuristic matched the function pair. Common values:

**Best (deterministic):**
- `Same RVA and hash` — identical bytes at the same relative address
- `Same order and hash` — identical bytes, same ordinal position
- `Bytes hash` — identical raw bytes (address-independent)
- `Same cleaned assembly` — identical assembly after stripping addresses/constants
- `Same cleaned pseudo-code` — identical decompiled code after normalization
- `Same cleaned microcode` — identical Hex-Rays microcode
- `Equal assembly or pseudo-code` — exact text match
- `Same RVA` — same relative virtual address (name may differ)
- `Same address, nodes, edges and mnemonics` — structural + instruction match

**Partial (heuristic):**
- `Same compilation unit` — functions from the same source file
- `Same KOKA hash and constants` — control flow + constant values match
- `Same constants` — shared magic numbers / string references
- `Same rare KOKA hash` — unique control flow pattern
- `Same rare MD Index` — unique complexity fingerprint
- `Same MD Index and constants` — complexity + constants match
- `Similar pseudo-code and names` — fuzzy decompiled code comparison
- `Pseudo-code fuzzy hash` — ssdeep-style fuzzy hash of pseudocode
- `Partial pseudo-code fuzzy hash (normal/reverse/mixed)` — partial fuzzy match
- `Same nodes, edges, loops and strongly connected components` — graph topology
- `Same low complexity, prototype and names` — small functions with matching signatures
- `Same graph` — isomorphic control flow graph
- `Same high complexity` — matching cyclomatic complexity (large functions)
- `Same rare assembly instruction` — unique opcode sequence
- `Same rare basic block mnemonics list` — unique block-level instruction pattern
- `Topological sort hash` — matching Tarjan SCC ordering
- `Import names hash` — same set of imported API calls

## Export Options

| Option | Default | Env Variable | Description |
|--------|---------|--------------|-------------|
| Use decompiler | `true` | `DIAPHORA_USE_DECOMPILER` | Export Hex-Rays pseudocode |
| Exclude library/thunk | `true` | — | Skip trivial wrappers |
| Ignore small functions | `false` | — | Skip functions with < 4 instructions |
| Resume existing | `false` | — | Continue a previously interrupted export |

## Exported Function Features

Each function is exported with 49 feature columns including:

- **Structural:** nodes, edges, indegree, outdegree, cyclomatic complexity, loops, strongly connected components
- **Content:** bytes hash, mnemonics, constants, assembly, pseudocode
- **Identity:** name, address, RVA, prototype, mangled name
- **Hashes:** function hash, KOKA hash (kgh_hash), MD Index, pseudocode hashes (3 variants), mnemonics SPP, topological sort hash
- **Decompiler:** pseudocode, pseudocode lines, clean pseudo, microcode, clean microcode

## Build

```bash
# Native (plugin + CLI)
xmake config --ida_plugin=y -y
xmake build -y

# Desktop app
cd desktop
bun install
bun run tauri build
```

## License

MIT
