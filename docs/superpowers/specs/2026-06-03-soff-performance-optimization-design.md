# Soff Performance Optimization Design

Date: 2026-06-03

## Scope

Optimize the full Soff project in three priority lanes:

1. Remove noticeable IDA startup/open sluggishness after installing the plugin.
2. Speed up IDA export while preserving crash-resume and export compatibility.
3. Speed up diffing by improving SQL, heuristic, propagation, and basic-block matching pipelines.

The implementation should borrow the high-recall heuristic breadth of Diaphora and the candidate reduction, structural propagation, and graph-aware matching discipline of BinDiff.

## Non-Goals

- Do not rewrite the project from scratch.
- Do not remove existing heuristics without benchmark evidence.
- Do not make microcode export a default fast path.
- Do not split the plugin into multiple DLLs unless lighter-weight changes fail to make IDA startup acceptable.

## Current Hotspots

### Plugin Startup and UI

- `src/plugin/soff_plugin.cpp` is a large monolithic IDA plugin translation unit.
- IDA startup should only register the Soff menu/actions, but any accidental construction of SQLite, Hex-Rays, repository, or diff engine objects will make plugin load visible.
- Result chooser refresh/edit paths reload entire result databases.
- Diff view actions repeatedly reload both primary and secondary snapshots from SQLite.

### Export

- `read_function_feature()` performs expensive per-instruction IDA API calls, line generation, assembly cleaning, xref walking, switch extraction, constant extraction, and hash accumulation.
- Pseudocode and microcode extraction are inherently expensive and must remain opt-in or explicitly visible to the user.
- SQLite export currently pays unnecessary overhead when many rows are inserted into instructions, basic blocks, callgraph, constants, and relation tables.
- Existing crash-resume support must remain intact.

### Diff

- `SqlHeuristicRunner::run_all()` executes many heuristics sequentially and parses many string columns per candidate.
- `deep_ratio_bonus()` and propagation passes perform N+1 SQL lookups by address, constant, callgraph, or compilation unit.
- Fixed-point iteration can rerun all heuristics after propagation, multiplying query cost.
- Basic-block matching loads block data repeatedly for many function pairs.

## Design Overview

Use a staged optimization model. Each stage adds measurement, preserves correctness with golden outputs, and keeps the next stage reversible.

1. Add telemetry and correctness guardrails.
2. Make IDA plugin initialization thin and lazy.
3. Optimize export hot paths and SQLite writing.
4. Optimize diff through in-memory caches, heuristic staging, and SQL/index improvements.
5. Optimize propagation and basic-block matching with cached structural data.

## Stage 0: Telemetry and Guardrails

### Requirements

- Add a `SOFF_PERF=1` mode that emits a compact timing tree.
- Measure plugin init, export phases, SQLite write time, each heuristic, propagation subpass, BB matching, and result save.
- Keep telemetry disabled by default and low-overhead when disabled.

### Timing Spans

- Plugin: `plugin.init`, `action.register`, `menu.attach`.
- Export: `export.total`, `export.enumerate_functions`, `export.read_function`, `export.hexrays.pseudocode`, `export.hexrays.microcode`, `export.sqlite.write`, `export.batch_commit`.
- Diff: `diff.total`, `diff.equal`, `diff.same_name`, `diff.heuristic.<name>`, `diff.propagation.<pass>`, `diff.bb_matching`, `diff.cleanup`, `diff.save`.
- SQL runner: `sql.query`, `sql.parse_candidate`, `ratio.compute`, `ratio.deep_bonus`.

### Correctness Guardrails

- Maintain golden export and diff fixtures.
- For exports, compare function counts, instruction/basic-block/callgraph/constants row counts, selected hashes, MD index values, and pseudocode/microcode presence.
- For diffs, compare best/partial/unreliable/unmatched counts, top address pairs, and any changed classification.
- Any accuracy regression above 0.5% must be reviewed before accepting the optimization.

## Stage 1: IDA Startup and Plugin Load

### Requirements

- `PLUGIN_ENTRY/init()` must only do lightweight registration.
- Heavy subsystems must be created on first action use.
- Hex-Rays initialization must stay deferred until an export or local diff action needs it.
- SQLite and repository objects must not be constructed during plugin load unless strictly necessary.

### Changes

- Audit plugin init and constructors for hidden work.
- Keep only minimal top-level Soff menu/action registration at startup.
- Lazily create export, diff, import, result chooser, graph diff, and local diff runtime objects.
- Cache loaded `.soff` results for chooser refresh/edit instead of reloading the full result database.
- Cache loaded primary/secondary snapshots for diff views and invalidate only when the result path changes.

### Optional Later Step

If startup remains slow after lazy initialization, introduce a thin loader plugin and move core runtime into a lazily loaded module. This is intentionally deferred because it adds ABI, deployment, and xmake complexity.

### Validation

- Measure plugin init time with `SOFF_PERF=1`.
- Verify IDA opens with the plugin installed and no binary loaded action performs unexpected work.
- Confirm all menu actions still work after lazy construction.

## Stage 2: Export Optimization

### Hot Loop Reduction

- Reduce repeated allocation in `read_function_feature()`.
- Reserve storage for assembly text, stripped assembly, mnemonics, instruction details, constants, and call references based on function size or instruction estimates.
- Avoid regex-heavy cleaning where deterministic token scanning is sufficient.
- Cache repeated per-function results such as function name, flags, comments, and prototype variants.

### Hash Acceleration

- Replace per-instruction decimal string multiplication in hash accumulators with a fast numeric accumulator where possible.
- Convert to stored string form once per function.
- Preserve legacy hash behavior behind a compatibility path until golden exports prove equality or intentional schema-compatible replacement.

### Hex-Rays Export

- Keep pseudocode export optional and visible.
- Keep microcode export as an explicit slow option.
- Cache failed decompilation/microcode attempts per function for the current export run.
- Continue clearing Hex-Rays caches periodically, but include timing so the interval can be tuned.

### SQLite Writing

- Reuse prepared statements for high-volume export tables.
- Batch insert instructions, basic blocks, block relations, callgraph rows, and constants.
- Apply performance pragmas during export while preserving durability expectations.
- Tune async writer batch size to reduce foreground waits while preserving crash-resume boundaries.

### Large IDB Degradation Strategy

- For very large databases, degrade expensive features first: microcode, detailed BB instruction payloads, low-value tiny functions, and library/thunk functions.
- Keep user-visible options and export metadata describing which features were skipped.

### Validation

- Run export before and after optimization on the same IDB.
- Compare table counts and key fields.
- Compare runtime by phase, not only total runtime.
- Target at least 2x faster no-decompiler export and measurable improvement for decompiler export outside Hex-Rays time.

## Stage 3: Diff SQL and Heuristic Pipeline

### Function Cache

- Load key `functions` fields from both databases once at session start.
- Store native numeric fields as native types to avoid repeated string parsing.
- Provide lookup by address and by name.
- Use the cache in `deep_ratio_bonus()`, same-name matching, propagation, candidate ratio computation, and BB prechecks.
- Add a memory threshold fallback for extremely large binaries.

### N+1 SQL Removal

- Replace repeated address lookups with cache lookups.
- Preload or batch constants by function.
- Preload or batch callgraph edges by function.
- Preload compilation unit membership needed by propagation.
- Memoize expensive text ratio and AST-prime comparisons by function pair.

### Heuristic Staging

- Execute cheap, high-confidence heuristics first.
- Use matched/unmatched sets to shrink candidate space before expensive heuristics.
- Run slow and unreliable heuristics only when enabled and when binary size permits.
- Add an MD index fast path before broad fuzzy heuristics, guarded by size/graph similarity checks.

### Fixed-Point Cost Control

- Track propagation deltas.
- Do not rerun all heuristics unless propagation adds a meaningful number of new matches.
- Prefer targeted reruns over full reruns.
- Cap fixed-point iteration to the lowest value that preserves golden results.

### SQLite Indexes

Add or verify covering indexes for common diff queries:

- `functions(address, bytes_hash)`
- `functions(name, nodes, instructions)`
- `functions(md_index, nodes, edges)`
- `callgraph(func_id, type, address)`
- `constants(constant, func_id)`
- Compilation unit membership joins used by slow heuristics.

### Validation

- Run golden diff pairs with and without slow/unreliable heuristics.
- Compare match counts and top pairs.
- Count SQL statements before and after; target at least a 10x reduction in repeated point queries.
- Target several-fold speedup on 10k-function class inputs.

## Stage 4: Propagation and Basic-Block Matching

### Propagation

- Convert propagation passes to consume cached functions, constants, callgraph, and compilation unit membership.
- Prioritize BinDiff-style local structural propagation: matched callers/callees, address-neighbor affinity, and compilation-unit locality.
- Keep Diaphora-style text and constants propagation as recall-oriented secondary passes.

### Basic-Block Matching

- Cache basic-block data per function address.
- Memoize basic-block match results by function pair.
- Skip BB matching when it cannot change classification, such as tiny functions or already near-perfect matches.
- Keep BB matching for ambiguous partial/unreliable candidates where structural evidence improves confidence.

### Validation

- Compare classification changes caused by BB matching before and after caching/skipping.
- Ensure skipped BB matching does not reduce golden best/partial quality.

## Error Handling

- Telemetry failures must never abort export or diff.
- Cache loading must fall back to SQL mode when memory thresholds are exceeded.
- Batch SQLite writes must report the table and batch boundary on failure.
- Export crash-resume markers must remain accurate when batch sizes change.
- Lazy plugin initialization must surface first-use errors through IDA messages and dialogs instead of failing silently.

## Test and Benchmark Plan

### Unit and Smoke Tests

- Build and run `soff_smoke`.
- Keep ratio and AST-prime tests stable.
- Add tests for cache lookups, telemetry disabled behavior, and index creation.

### Export Benchmarks

- Export one small fixture and one medium/large IDB.
- Measure no-decompiler, pseudocode, and microcode modes separately.
- Compare table counts and key fields.

### Diff Benchmarks

- Run CLI diff on existing fixture pairs and at least one larger real pair.
- Measure total diff time, per-heuristic time, propagation time, BB matching time, SQL statement count, and result counts.

### IDA Startup Benchmarks

- Measure plugin init time with `SOFF_PERF=1`.
- Verify startup does not instantiate export/diff repositories.
- Verify first-use latency is acceptable and visible only when the user invokes a Soff action.

## Rollout Plan

1. Implement telemetry and golden comparisons.
2. Implement plugin lazy initialization and UI/result snapshot caches.
3. Optimize SQLite export writes and hot-loop allocations.
4. Add diff function cache and remove N+1 SQL from `deep_ratio_bonus()`.
5. Convert propagation to caches and batch data access.
6. Add BB matching cache and skip policy.
7. Add MD index fast path and final heuristic staging refinements.

Each stage must be benchmarked before proceeding to the next stage.

## Acceptance Criteria

- Installing Soff no longer makes IDA opening noticeably slower.
- Export remains resumable and produces compatible SQLite outputs.
- Diff outputs remain stable on golden pairs.
- Diff and export telemetry identifies the remaining dominant bottlenecks.
- No high-risk plugin split is introduced unless benchmark evidence requires it.
