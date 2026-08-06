# IDA plugin implementation modules

`soff_plugin.cpp` owns the SDK/system includes, the anonymous namespace boundary, and the exported `PLUGIN` descriptor. The implementation is kept in focused compile-time modules so IDA callback symbols retain the original internal linkage while each workflow is independently navigable:

- `common.inc`: shared constants, data structures, validation, hashing, and utility helpers.
- `settings.inc`: persisted options, environment overrides, and IDA option dialogs.
- `export_helpers.inc`: crash markers, JSON/hash helpers, and shared feature primitives.
- `hexrays.inc`: Hex-Rays initialization, AST/pseudocode support, and decompiler failure accounting.
- `microcode.inc`: microcode tokenization and microcode feature extraction.
- `export.inc`: native function/CFG feature extraction and SQLite export.
- `result_ui.inc`: result chooser/panel models and chooser interactions.
- `graph_ui.inc`: HTML/native/microcode graph and text-diff viewers.
- `result_ui_actions.inc`: loading/saving result databases and result-view workflows.
- `import.inc`: applying selected result metadata back into the active IDB.
- `actions.inc`: export/diff/local-diff commands, graph actions, and IDC/automatic mode.
- `entry.inc`: `plugmod_t` lifecycle, action registration, and plugin initialization.

These are intentionally `.inc` files rather than independent DLL/object targets: the IDA SDK callback implementation has a large amount of shared anonymous-namespace state, and this arrangement achieves the requested module split without changing ABI, ownership, or initialization order.
