# Soff

[中文文档](README_CN.md)

High-performance binary diff engine for IDA Pro. Exports function features from IDA databases and compares them using 40+ heuristics to identify matching, modified, and unmatched functions across binary versions.

![Analyze View](img/analyze.png)

## Components

| Component | Description |
|-----------|-------------|
| `soff.dll` / `.so` / `.dylib` | IDA Pro plugin (IDA 9.0+) |
| `soff_cli` | Command-line diff tool |
| `soff-desktop` | Standalone result viewer (Tauri app) |

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

After loading the plugin, a top-level **Soff** menu appears in IDA's menu bar:

| Menu Item | Description |
|-----------|-------------|
| **Export current IDB** | Export the current database's function features to a `.sqlite` file |
| **Diff SQLite databases** | Compare two exported `.sqlite` files and produce a `.soff` result |
| **Load Diff Results** | Load a previously saved `.soff` result into the chooser |
| **Save Diff Results As** | Save current diff results to a new `.soff` file |
| **Import Diff Results** | Apply a `.soff` result to the current IDB: rename functions, import types/comments/prototypes from the matched binary |
| **Local Function Diff** | Compare two functions within the same IDB. Generates an HTML diff (text, native graph, or microcode graph) and opens it in the browser |

---

#### Export current IDB

| Field | Description |
|-------|-------------|
| **Output SQLite** | Path for the exported `.sqlite` database |
| **From address** | Start address for export range (default: beginning of binary) |
| **To address** | End address for export range (default: end of binary) |

| Option | Description |
|--------|-------------|
| **Use decompiler** | Export Hex-Rays pseudocode. Enables pseudocode-based heuristics during diff. Requires Hex-Rays license |
| **Export microcode (slow)** | Export Hex-Rays microcode IR. Enables microcode-based matching but significantly increases export time |
| **Exclude library/thunk/nullsub** | Skip library functions, thunk wrappers, and empty stubs |
| **Ignore very small functions** | Skip functions with fewer than 4 instructions |

---

#### Diff SQLite databases

| Field | Description |
|-------|-------------|
| **Primary SQLite** | The original (baseline) exported database |
| **Secondary SQLite** | The modified (patched/updated) exported database |
| **Result DB** | Output path for the `.soff` result file |
| **Max rows** | Maximum number of match results to produce (default: 1000000) |
| **Timeout seconds** | Maximum time for the diff operation (default: 300s) |

| Option | Description |
|--------|-------------|
| **Enable slow heuristics** | Run computationally expensive heuristics (fuzzy hashing, graph comparison). Recommended for thorough analysis |
| **Enable unreliable heuristics** | Include low-confidence matching algorithms. May produce false positives |
| **Enable experimental heuristics** | Use experimental matching strategies still under development |

---

### Desktop Viewer

Open a `.soff` result file to browse matches interactively.

![Soff Results](img/soff.png)

![Graph View](img/graph.png)

---

### CLI

```bash
# Diff two exports
soff_cli diff primary.sqlite secondary.sqlite -o results.soff

# View summary
soff_cli info results.soff
```

## IDA Chooser Fields

Results appear in the "Soff Diff Results" chooser:

| Column | Description |
|--------|-------------|
| **Type** | Match confidence: `best`, `partial`, or `unreliable` |
| **Ratio** | Similarity score 0.0–1.0. 1.0 = identical |
| **Primary** | Function address in the primary binary |
| **Primary name** | Function name in the primary binary |
| **Secondary** | Function address in the secondary binary |
| **Secondary name** | Function name in the secondary binary |
| **Description** | The heuristic that produced this match |

### Match Types

| Type | Meaning |
|------|---------|
| `best` | High confidence. Hash-based or ratio = 1.0 |
| `partial` | Medium confidence. Structural similarity detected |
| `unreliable` | Low confidence. May be false positive |

### Heuristics (Description column)

**Best:**
`Same RVA and hash` · `Same order and hash` · `Bytes hash` · `Same cleaned assembly` · `Same cleaned pseudo-code` · `Same cleaned microcode` · `Equal assembly or pseudo-code` · `Same RVA` · `Same address, nodes, edges and mnemonics`

**Partial:**
`Same compilation unit` · `Same KOKA hash and constants` · `Same constants` · `Same rare KOKA hash` · `Same rare MD Index` · `Same MD Index and constants` · `Similar pseudo-code and names` · `Pseudo-code fuzzy hash` · `Partial pseudo-code fuzzy hash` · `Same nodes, edges, loops and strongly connected components` · `Same graph` · `Same high complexity` · `Same rare assembly instruction` · `Same rare basic block mnemonics list` · `Topological sort hash` · `Import names hash`

## Build

```bash
# Native (plugin + CLI)
xmake config --ida_plugin=y -y
xmake build -y

# Desktop app
cd desktop && bun install && bun run tauri build
```

## License

MIT
