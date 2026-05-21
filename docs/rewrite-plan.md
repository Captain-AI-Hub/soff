# Diaphora C++ rewrite plan

## Rewrite principle

The rewrite starts with compatibility, then replaces pieces one at a time.

1. Keep a Diaphora-compatible SQLite export schema during the first milestones.
2. Build and test the diff engine outside IDA first.
3. Keep IDA SDK code behind adapter classes.
4. Port heuristics in batches, with visible parity tests for every batch.
5. Do not mix UI behavior with export, storage, or matching logic.

## Source-system map

| Python Diaphora area | Current role | C++ destination |
| --- | --- | --- |
| `diaphora_plugin.py` | IDA menu/actions/plugin lifetime | `src/plugin`, `src/ui` |
| `diaphora_ida.py` | IDA export, chooser UI, graph view, import helpers | `src/ida`, `src/ui`, `src/export` |
| `diaphora.py::CBinDiff` | schema setup, DB attach, heuristics runner, cleanup, result persistence | `src/db`, `src/diff`, `src/export` |
| `db_support/schema.py` | tables and indexes | `src/db/schema.cpp` |
| `diaphora_heuristics.py` | SQL heuristic catalog and ratio modes | `src/diff/heuristics.cpp` |
| `jkutils/*`, `codecut/*` | graph/hash/fuzzy helpers | `src/analysis`, `src/diff` |

## Milestones

### M1: Database and heuristic skeleton

Goal: build a C++ core that knows the Diaphora-compatible schema and exposes a typed heuristic catalog.

Deliverables:

- `SchemaDefinition` for export tables and indexes.
- `HeuristicDefinition` with name, category, ratio mode, flags, SQL, and minimum ratio.
- Smoke test that validates schema and heuristic registry are non-empty.
- CLI command skeleton for future `export`, `diff`, `inspect-db`.

Acceptance:

- `xmake` and `xmake run soff_smoke` pass.
- `soff_core` has no IDA SDK includes.
- First batch contains exact/best heuristics: same RVA/hash, same order/hash, function hash, bytes hash.

### M2: SQLite repository

Goal: write and read the compatible schema from C++.

Deliverables:

- SQLite runtime integrated through a small dynamic loader to avoid MSVC/MinGW import-library conflicts.
- `SnapshotRepository::create_schema`, `create_indices`, `save`, `load`, `attach_diff`.
- Result tables for saved diff sessions.
- Tests using temporary SQLite files.

Acceptance:

- A blank Diaphora-compatible DB can be created and inspected with sqlite tooling. Done for schema/index creation.
- A minimal `ProgramSnapshot` can be saved and loaded. Done in `soff_smoke`.
- Existing Diaphora-exported DB can be opened and version-checked.

### M3: Diff engine outside IDA

Goal: compare two existing Diaphora-compatible DBs without launching IDA.

Deliverables:

- Query runner for heuristic batches.
- Ratio calculators for exact, quick text, relaxed text, graph/metadata weighted comparisons.
- Cleanup pass for duplicate and conflicting matches.
- CLI `soff_cli diff old.sqlite new.sqlite`.

Acceptance:

- Produces best/partial/unreliable/unmatched result counts.
- Runs first heuristic batch against two existing exports.

### M4: IDA export MVP

Goal: export current IDB to the compatible SQLite schema.

Deliverables:

- IDA function iterator.
- Function metadata: name, address/RVA, size, flags, instruction count.
- Basic block and callgraph export.
- Native instruction export.
- Plugin action: `Diff or export`.

Acceptance:

- `soff.dll` loads in IDA 9.3.
- Exported DB passes C++ schema validation.
- Exported function count matches IDA function count.

### M5: IDA results UI

Goal: show diff results inside IDA.

Deliverables:

- Actions: show, load, save, load/import, import definitions.
- Chooser-backed result lists for best, partial, unreliable, multimatch, unmatched.
- Jump-to-primary / jump-to-secondary behavior.

Acceptance:

- Existing result DB can be opened and shown.
- User can navigate matched functions from chooser rows.

### M6: Advanced export and parity

Goal: port high-value Diaphora features after the basic pipeline works.

Deliverables:

- Pseudocode extraction and normalized pseudo hash.
- Microcode summaries where Hex-Rays is available.
- Type/structure/import/export metadata.
- Compilation unit and local affinity passes.
- Optional ML parity hook, kept separate from the core deterministic engine.

Acceptance:

- Diff quality approaches the Python implementation on shared fixture pairs.
- Slow/experimental heuristics are togglable.

## Work order for M1

1. Add typed schema and index definitions.
2. Add typed heuristic registry with ratio modes and flags.
3. Extend smoke test to assert M1 registry invariants.
4. Expand `codemap.md` with the M1 implementation map.
5. Keep IDA plugin compiling while core grows.

## Non-goals for early milestones

- No full UI clone before diff core exists.
- No Hex-Rays/microcode dependency in core library.
- No ML model port until deterministic results are stable.
- No forced schema redesign before existing Diaphora DB compatibility is proven.
