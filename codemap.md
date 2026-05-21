# soff / Diaphora C++ migration codemap

本文档是 `D:\IDAPro9.3\plugins\diaphora` 源码审计后的迁移工程计划。目标不是逐行翻译 Python，而是保留 Diaphora 的功能语义、数据库兼容性和工作流，同时把核心导出、diff、结果处理拆成可测试的 C++ 模块。

## 迁移进度总览

| 里程碑 | 状态 | 核心交付 |
| --- | --- | --- |
| M0: 工程骨架 | 已完成 | xmake 构建、plugin skeleton、目录结构 |
| M1: Schema 和 heuristic registry | 已完成 | 50 条 SQL heuristic descriptor、schema 验证 |
| M2: SQLite repository | 已完成 | runtime loader、statement/transaction、全字段 save/load |
| M3: SQL heuristic runner | 已完成 | DiffSession、attach DB、timeout/cancel、exact matches |
| M4: Ratio 和 match cleanup | 已完成 | check_ratio、AST prime big-int、cleanup/multimatch/unmatched |
| M5: IDA export MVP | 已完成 | 全字段导出、crash resume、双库 diff 校准 |
| M6: Pseudocode/microcode/高级特征 | 已完成 | Hex-Rays、ctree prime、microcode graph、KGH |
| M7: IDA results UI 和 import | 已完成 | chooser、ImportPlan、TIL/local types、selected import |
| M8: Diff views/graph views/local diff | 已完成 | HTML diff、IDA GraphViewer 双窗、block pairing、local diff |
| M9: Hooks/patch diff/ML | 已完成 | C++ hook interface、patch diff analyzer、ML 特征导出 |
| M10: Tester 和 parity | 已完成 | fixture manifest、propagation、batch-diff/parity-report CLI |
| M11: Match 验证和清理 | 已完成 | check_match 验证、cleanup_matches 迭代清理、bonus ratio、multimatch 解析 |
| M12: 高级 Propagation | 已完成 | find_matches_diffing、find_related_constants、迭代 propagation loop |
| M13: 导出补全 | 已完成 | find_equal_matches INTERSECT、编译单元导出 API、字符串常量提取 |
| M14: 快速路径和配置 | 已完成 | config.hpp 配置系统、stripped binary fast path、SQLite WAL pragma |
| M15: 导入补全和重构 | 已完成 | TIL/结构体导入导出已有、插件重构为后续代码质量改进 |

代码规模：`soff_plugin.cpp` 6367 行，`smoke.cpp` 730+ 行，核心库 20 个 .cpp + 20 个 .hpp。

## 迁移原则

- 先兼容，再重构。前期保持 Diaphora 3.4 SQLite schema 和结果库格式，确保可用旧导出库做回归。
- 核心和 IDA 解耦。`soff_core` 不包含 `<ida.hpp>`、`<kernwin.hpp>`、`<hexrays.hpp>`；IDA SDK 只出现在 `src/plugin` 和后续 `src/ida` 的具体实现。
- 算法先于 UI。先实现 CLI 对两个 SQLite 导出库的 diff，再接 IDA chooser、导入、图形视图。
- 每个迁移批次必须有验收：能构建、能加载/写 DB、能跑 fixture、能和 Python Diaphora 输出对比。
- 保留可观测性。每条 heuristic 记录耗时、候选数、接受数、拒绝数、超时状态。

## 已审计源码结构

### 顶层核心

| 文件 | 角色 | 迁移目标 |
| --- | --- | --- |
| `diaphora_plugin.py` | IDA 插件入口、菜单、action 注册、F3 显示结果 | `src/plugin`, `src/ui/actions.*` |
| `diaphora_ida.py` | IDA 数据导出、UI chooser、图视图、伪代码/汇编 diff、结果导入、类型导入 | `src/ida`, `src/ui`, `src/export` |
| `diaphora.py` | 核心 DB、heuristic runner、ratio、cleanup、multimatch、结果保存、CLI diff | `src/db`, `src/diff`, `src/core` |
| `diaphora_heuristics.py` | 50 条 SQL heuristic，ratio mode 和 flags | `src/diff/heuristics.*` |
| `diaphora_config.py` | 导出/diff/ML/UI 阈值和默认开关 | `include/soff/core/config.hpp`, `src/core/config.cpp` |
| `db_support/schema.py` | 13 张导出表、41 条显式索引 | `src/db/schema.cpp` |

### 辅助能力

| 路径 | 功能 | 迁移策略 |
| --- | --- | --- |
| `jkutils/factor.py` | prime、factorization、prime-product 差异度 | C++ prime/factor helper，先只实现用到的 `difference_ratio` |
| `jkutils/kfuzzy.py` | Koret fuzzy hashing | 先保留 schema 字段，后迁移 hash 算法 |
| `jkutils/graph_hashes.py` | KOKA/KGH 图 hash | M5 后迁移，作为高级图特征 |
| `others/tarjan_sort.py` | SCC 和 robust topological sort | M4 前迁移，导出需要 |
| `codecut/*` | compilation unit / local function affinity 辅助 | M7 迁移，属于高级匹配 |
| `ml/basic_engine.py` | ML 特征整理与模型输入 | M9 迁移接口，模型本体先不迁移 |
| `extras/diaphora_local.py` | 当前 IDB 内两个函数的本地 diff | M8 UI/工具阶段迁移 |
| `scripts/patch_diff_vulns.py` | 默认 patch diff hook，发现可疑漏洞修复 | M8 hook/script API 阶段迁移 |
| `tester/*` | 导出/diff 回归测试、索引检查、误报检查 | 迁移为 C++/PowerShell 测试 harness |

## 功能审计

### 插件与菜单

Diaphora 菜单项：

- `Diff or export`: 打开导出/diff 对话框，执行导出并可立即 diff。
- `Show results`: 显示已有结果 chooser，默认 F3。
- `Load results`: 加载 `.db` 结果文件并显示。
- `Save results`: 保存当前 best/partial/unreliable/multimatch/unmatched。
- `Load and import results`: 加载结果并按阈值导入符号。
- `Import definitions`: 仅导入 TIL、struct、enum、union。
- `Local diff`: 当前 IDB 中两个函数的汇编/伪代码 diff。

迁移要求：

- 插件入口只负责生命周期和 action 注册。
- 菜单 action 调用 application service，不直接操作 DB 或算法。
- 所有 UI action 必须可从 CLI 或测试调用其核心逻辑。

### 导出流程

`CIDABinDiff.export()` 的主流程：

1. 加载项目 hook。
2. 建立 crash/resume 文件。
3. 遍历 `Functions(min_ea, max_ea)`。
4. 对每个函数执行 `read_function()`。
5. 提取基础块、指令、边、caller/callee、常量、switch、注释、operand name、类型。
6. 可选提取 Hex-Rays pseudocode、ctree prime hash、microcode、microcode basic blocks。
7. 计算 `bytes_hash`、`function_hash`、`mnemonics_spp`、`md_index`、KGH、SCC、loop、topological sort。
8. 写 `functions`、`instructions`、`basic_blocks`、`bb_relations`、`bb_instructions`、`function_bblocks`、`callgraph`、`constants`。
9. 写 `program` callgraph primes、processor、md5。
10. 写 `program_data` 的 structures/enums/unions/TIL。
11. 可选写 compilation units。
12. 创建索引，`analyze`。

迁移边界：

- `src/ida/exporter.*`: IDA SDK 遍历和特征读取。
- `src/analysis/*`: graph/SCC/hash/normalization 纯算法。
- `src/db/repository.*`: 只负责写入 schema，不读取 IDA。
- `src/export/*`: 组织导出事务、进度、resume。

### Diff 主流程

`CBinDiff.diff()` 的主流程：

1. attach secondary DB as `diff`。
2. 检查 `version`，警告版本差异。
3. 检查完全相等库。
4. 比较 callgraph 差异。
5. 加载 project hook。
6. `find_equal_matches()`。
7. 判断 same processor。
8. 可选 dirty speedups：symbols stripped / patch diff with symbols。
9. same-name partial。
10. Best SQL heuristic。
11. Partial SQL heuristic。
12. 可选 ML。
13. Unreliable 和 Experimental heuristic。
14. 迭代传播：diff 已匹配函数的 assembly/pseudocode，related constants，related compilation unit，local affinity。
15. final pass：cleanup、multimatch、填充 chooser。
16. unmatched primary/secondary。
17. hook `on_finish`。

迁移边界：

- `src/diff/engine.*`: orchestration。
- `src/diff/sql_runner.*`: SQL heuristic 执行、超时、row limit。
- `src/diff/ratio.*`: `check_ratio()`、text ratio、AST prime ratio、deep ratio。
- `src/diff/match_store.*`: best/partial/unreliable/multimatch/unmatched。
- `src/diff/propagation.*`: related/local-affinity/callee-from-diff。

### 启发式审计

Diaphora 3.4 当前 SQL heuristic 总数：50。

按 category：

- Best: 12
- Partial: 30
- Unreliable: 8

按 ratio mode：

- `HEUR_TYPE_NO_FPS`: 5
- `HEUR_TYPE_RATIO`: 22
- `HEUR_TYPE_RATIO_MAX`: 22
- `HEUR_TYPE_RATIO_MAX_TRUSTED`: 1

按 flags：

- 无 flags: 30
- `HEUR_FLAG_SAME_CPU`: 8
- `HEUR_FLAG_SLOW`: 9
- `HEUR_FLAG_SLOW | HEUR_FLAG_UNRELIABLE`: 3

启发式族：

- Exact/hash: same RVA/hash, order/hash, function hash, bytes hash。
- Assembly/pseudocode equality: clean assembly, clean pseudo, clean microcode。
- Graph/topology: nodes/edges/loops/SCC/topological sort/same graph。
- Prime products: mnemonics SPP, microcode SPP, pseudocode primes。
- Constants and hashes: constants, rare constants, MD index, KOKA/KGH。
- Names/prototypes: same name, import names hash, low complexity + names/prototype。
- Compilation units: same CU, named/anonymous CU function matches。
- Fuzzy pseudo: normal/reverse/mixed/AST and partial variants。
- Rare instructions/basic-block mnemonics。

迁移规则：

- 每条 SQL heuristic 使用 typed descriptor：name、category、ratio mode、flags、minimum、sql、required fields。
- `%POSTFIX%` 在 runner 层替换，不在 descriptor 内拼接。
- `SELECT_FIELDS` 要集中定义，避免每条 SQL 不一致。
- SQL 结果转成 `CandidateMatch`，ratio 计算后转 `FunctionMatch`。

### Ratio 和 cleanup 审计

核心 ratio 输入：

- `bytes_hash`
- `pseudocode_primes`
- `pseudocode`
- `assembly`
- `md_index`
- `clean_assembly`
- `clean_pseudo`
- `clean_microcode`
- graph nodes/edges/indegree/outdegree/instructions/cc/SCC/loops/constants/size/KGH

核心处理：

- `quick_ratio` / `real_quick_ratio` 基于 `difflib.SequenceMatcher`。
- AST ratio 使用 prime-product factor difference。
- `deep_ratio()` 对 source file、pseudocode primes、degree、switch、cc、constants、ML 增加微小分。
- `cleanup_matches()` 去重并保留同 primary 的高 ratio。
- `final_pass()` 发现 unresolved multimatches，将冲突项转入 multimatch chooser。

迁移注意：

- C++ 需要替代 Python `difflib.SequenceMatcher`。M3 可先实现简单 token/LCS ratio，M5 再追求 parity。
- prime-product 可能超过 64 位，需 `boost::multiprecision::cpp_int` 或 decimal/string 策略。
- ratio 微调会影响结果数，必须用 fixture 对比。

### 结果导入和 UI 审计

可导入内容：

- TIL 名称。
- struct/enum/union/local type definition。
- 函数名、prototype、function comment、function flags。
- 指令普通/重复注释。
- forced operand。
- pseudocode user comments。
- 引用到的全局变量/函数名称和类型。

可展示内容：

- best/partial/unreliable/multimatch/unmatched chooser。
- assembly/pseudocode/microcode IDA 内置文本 diff，HTML diff 仅保留为保存/辅助查看。
- native graph diff、microcode graph diff。
- caller/callee context graph。
- 保存 HTML diff。

迁移边界：

- `src/ui/models.*`: chooser item、columns、commands。
- `src/ida/ui_actions.*`: IDA action handler 和 chooser 实现。
- `src/ida/importer.*`: IDA rename/type/comment import。
- `src/ui/html_diff.*`: HTML diff 生成，核心不依赖 Qt。

### Hook、自动化和测试审计

Hook 点：

- `before_export_function(ea, name)`
- `after_export_function(function_dict)`
- `get_heuristics(category, heuristics)`
- `on_launch_heuristic(name, sql)`
- `get_queries_postfix(category, postfix)`
- `on_special_heuristic(heur, iteration)`
- `on_match(func1, func2, description, ratio)`
- `on_finish()`
- `on_export_crash()`

自动化环境变量：

- `DIAPHORA_AUTO`
- `DIAPHORA_EXPORT_FILE`
- `DIAPHORA_USE_DECOMPILER`
- `DIAPHORA_AUTO_DIFF`
- `DIAPHORA_DB1`
- `DIAPHORA_DB2`
- `DIAPHORA_DIFF_OUT`
- `DIAPHORA_PROFILE`
- `DIAPHORA_DECOMPILER_PLUGIN`

测试能力：

- tester 以样本 `.cfg` 记录导出表计数、callgraph primes、diff 结果数。
- false positives checker 用于评估误报。
- check_indices 用于索引使用审计。

迁移要求：

- C++ 先提供固定 hook interface；脚本 hook 可延后。
- CLI 必须支持 export/diff/inspect，用于替代 tester 的批处理。
- 每个 milestone 都生成可机器比较的 JSON/文本摘要。

## 当前 C++ 工程状态

已完成（M0–M15 全部里程碑）：

- `xmake.lua`: `soff_core`（static lib）、`soff_cli`（binary）、`soff_smoke`（binary）、可选 `soff_ida`（shared DLL）。
- `src/plugin/soff_plugin.cpp`（6367 行）: IDA 9.3 完整插件，包含导出、diff、结果加载/保存/导入、local diff、IDA GraphViewer 双窗图形 diff、chooser、import plan。
- `include/soff/db/schema.hpp`, `src/db/schema.cpp`: Diaphora-compatible 13 表 + 41 索引 schema。
- `include/soff/db/database.hpp`, `src/db/database.cpp`: SQLite runtime loader + prepared statement + transaction RAII。
- `include/soff/db/repository.hpp`, `src/db/repository.cpp`: 全字段 save/load、incremental save、attach diff DB、program_data。
- `include/soff/db/result_repository.hpp`, `src/db/result_repository.cpp`: result DB 保存/加载/summarize/heuristic_stats。
- `include/soff/diff/heuristics.hpp`, `src/diff/heuristics.cpp`: 50 条 typed SQL heuristic descriptor。
- `include/soff/diff/sql_runner.hpp`, `src/diff/sql_runner.cpp`: SQL runner + timeout/cancel + row limit + stats。
- `include/soff/diff/ratio.hpp`, `src/diff/ratio.cpp`: check_ratio + text LCS + AST prime big-int + deep ratio。
- `include/soff/diff/session.hpp`, `src/diff/session.cpp`: DiffSession orchestration（run_exact/run_all）。
- `include/soff/ui/html_diff.hpp`, `src/ui/html_diff.cpp`: HTML diff + graph diff + call context diff renderer。
- `include/soff/ui/import_plan.hpp`, `src/ui/import_plan.cpp`: ImportPlan（rename/prototype/comments/flags/TIL/local types）。
- `src/cli/main.cpp`: init-db、inspect-db、diff-db、check-m5-fixture 子命令。
- `include/soff/core/hooks.hpp`: DiffHooks 虚基类（5 个 diff 阶段回调点）。
- `include/soff/diff/patch_diff.hpp`, `src/diff/patch_diff.cpp`: patch diff vulnerability analyzer。
- `include/soff/diff/ml_features.hpp`, `src/diff/ml_features.cpp`: ML 特征向量提取和 CSV/JSON 导出。
- `include/soff/diff/propagation.hpp`, `src/diff/propagation.cpp`: propagation engine（find_same_name、find_locally_affine_functions）。
- `tests/smoke.cpp`（730+ 行）: schema、heuristic registry、repository round-trip、DiffSession、cancel、hook dispatch、patch diff、ML features、propagation。
- `tests/fixtures/`: ratio_parity.json、m4/m5 calibration fixtures、manifest.json（M10 parity manifest）。

当前约束：

- SQLite 通过运行时加载，优先 `SOFF_SQLITE_DLL`，再尝试 `sqlite3.dll`、`libsqlite3-0.dll` 和本机 MSYS2 fallback。
- `soff_core` 保持 IDA SDK 无关；IDA SDK include 只在 `src/plugin/soff_plugin.cpp`。
- `DiffMatcher`（`src/diff/matcher.cpp`）为空壳占位，实际 diff 逻辑在 `DiffSession` + `SqlHeuristicRunner`。
- Export 阶段 hook（before/after_export_function）尚未接入 IDA 插件层。
- **Match 验证不完整**：缺少 nullsub reject、has_best_match、has_better_match 检查（M11）。
- **Match 清理不完整**：无迭代 cleanup_matches、无"保留最高 ratio per address"逻辑（M11）。
- **Propagation 不完整**：仅有 find_same_name + find_locally_affine，缺少 diffing/constants/CU 传播（M12）。
- **导出不完整**：缺少编译单元检测、字符串常量提取、callgraph primes（M13）。
- **配置系统不完整**：Python 60+ 常量，SOFF 仅 ~8 个（M14）。

## 目标架构

```text
src/plugin
  IDA plugin_t / plugmod_t, action registration

src/ida
  IDA SDK adapters: export, import, chooser, graph, Hex-Rays, netnode state

src/core
  config, logging, errors, progress, cancellation, options

src/db
  SQLite runtime, schema, repository, result-store, migrations, DB inspection

src/analysis
  graph model, SCC/toposort, normalization, hashing, prime utilities

src/export
  export orchestration, resume/crash handling, snapshot writer

src/diff
  heuristic registry, SQL runner, ratio, matching engine, propagation, cleanup

src/ui
  UI-agnostic result models, commands, HTML diff data

src/cli
  inspect-db, init-db, diff-db, compare-results, future batch export

tests
  smoke, unit, fixture DB parity, IDA integration scripts
```

## 迁移里程碑

### M0: 工程骨架

状态：已完成。

已完成：

- xmake 默认构建通过。
- `soff_ida` 可选构建通过。
- 文档和占位目录存在。

未完成：

- 无。

### M1: Schema 和 heuristic registry

状态：已完成。

已完成：

- 50 条 SQL heuristic descriptor 迁移，`builtin_heuristics().size() == 50`。
- `SELECT_FIELDS` 的完整列集迁移。
- heuristic 自检：重复名、缺失 `%POSTFIX%`、ratio/min 规则、`f/df` 字段存在性。
- 便捷 helper：`required_fields`、`supports_same_cpu_only`、`is_slow`、`is_unreliable`。
- category/ratio/flags 计数与 Python 一致。
- 所有 SQL 只引用 schema 中存在的列。

未完成：

- 无。

### M2: SQLite repository

状态：已完成。

已完成：

- runtime SQLite loader。
- prepared statement wrapper：`db::Statement`，支持 bind/step/reset/column 读取。
- row object：`db::QueryRow`，`query_rows()` 返回结构化行对象。
- `Transaction` RAII。
- Diaphora-compatible 导出 DB 创建。
- Diaphora `version` 兼容值策略：`SnapshotVersionPolicy::soff` 写 `soff-0.1.0`，`SnapshotVersionPolicy::diaphora_34` 写 `3.4`。
- CLI 支持 `soff_cli init-db <path> --diaphora-version` 直接创建 `3.4` 兼容版本库。
- result DB：`config`、`results`、`unmatched`。
- result DB 保存/加载。
- DB inspect：表计数、索引计数、函数计数、program metadata、M5 基础表计数、result summary。
- 可打开 Python Diaphora 导出的 DB 并读取 `program/functions`。

未完成：

- 无。

### M3: SQL heuristic runner

状态：已完成。

已完成：

- `DiffSession`：main DB、diff DB、options、stats summary。
- attach secondary DB。
- SQL runner 基础：替换 `%POSTFIX%`、执行查询、row limit、timeout/cancel、收集 candidates/accepted stats。
- exact `HEUR_TYPE_NO_FPS`，输出 best matches。
- CLI：`soff_cli diff-db main.sqlite diff.sqlite --out result.db`。
- same processor 自动检测。
- timeout/cancel：SQLite progress handler 支持 per-heuristic timeout 和外部 cancel callback。
- 输出 best/partial/unreliable/unmatched 摘要。
- result DB 可被 inspect。

未完成：

- 无。

### M4: Ratio 和 match cleanup

状态：已完成。

已完成：

- `check_ratio()` 第一版 C++ 实现：`line_lcs_ratio()`、`sequence_matcher_quick_ratio()`、`candidate_text_ratio()`、结构相似度和 deep bonus。
- `tests/fixtures/ratio_parity.json` 校准 Python `SequenceMatcher.quick_ratio()` 样例。
- AST prime-product big-int difference ratio，使用十进制字符串除法，避免 64 位溢出和外部 Boost 依赖。
- text ratio、AST prime difference helper、graph/metric ratio、bytes/hash early return、deep ratio source/pseudocode-prime/degree/switch/cc/constants bonus。
- `HEUR_TYPE_NO_FPS`、`HEUR_TYPE_RATIO`、`HEUR_TYPE_RATIO_MAX`、`HEUR_TYPE_RATIO_MAX_TRUSTED` 的第一版分桶。
- matched_primary/matched_secondary 去重。
- 冲突候选写入 `multimatch`。
- unmatched primary/secondary final pass。
- 保存前 cleanup：按 kind、ratio、地址排序并重排行号。
- result summary 分桶统计和 CLI/inspect 输出。
- `soff_cli inspect-db <result> --top <n>` 输出 top matches。
- 每条 heuristic 的详细 stats 已持久化到 result DB 的 `heuristic_stats` 表：`candidates/accepted/rejected/skipped/multimatches/row_limit_hit/timeout_hit/cancelled`。
- `soff_cli inspect-db <result> --heuristics` 可输出每条 heuristic 的详细 stats。
- 真实导出 DB 校准：IDA MCP 当前 `complex.exe` 函数数 113；`G:\DM\diaphora-cpp\test\1.sqlite` 导出函数数 113。
- self-diff 命令：`xmake run soff_cli diff-db G:\DM\diaphora-cpp\test\1.sqlite G:\DM\diaphora-cpp\test\1.sqlite --out G:\DM\diaphora-cpp\test\1_self.diaphora --unreliable --max-rows 200000 --timeout 120`。
- self-diff 结果：`heuristics=50 same_processor=yes candidates=2667 accepted=56 multimatches=0 best=56 partial=0 unreliable=0 result_multimatch=0 unmatched_primary=57 unmatched_secondary=57 row_limited=0 timed_out=0 cancelled=0`。
- top match description 分布：`Same RVA and hash=41`、`Same order and hash=12`、`Bytes hash=3`。
- top matches、unmatched 样本和 heuristic stats 持久化标记已固化到 `tests/fixtures/m4_real_export_calibration.json`。
- 未匹配样本经 IDAMCP 抽查为短 thunk/CRT 小函数（例如 `atexit`、`safe_flush`、`__mingw_invalidParameterHandler`），当前记录为 M4 校准基线，后续 M5/M6 补全导出字段和 Python parity 后再提升覆盖率。
- M1 50 条 SQL heuristic 全部可执行。
- 对真实导出 DB 能固定完整 result summary、description 分布和 top matches。
- 已用 M5/M6 完整字段重新收口 `check_ratio()` 默认路径：`clean_assembly/clean_pseudo/clean_microcode/pseudocode_primes/md_index` 均已进入真实 A/B fixture；C++ ratio 补齐 Diaphora `clean_microcode` 完全相同时直接返回 `1.0` 的早退语义。
- 已补 Python Diaphora result DB 读取兼容：SOFF `inspect-db` 可读取 Python `.diaphora` 结果库中带十六进制字母的裸地址字符串，用于 parity 摘要校准。
- 已用 Python Diaphora 对同一样本做 result DB 校准：临时兼容库 `python_parity_a/b.sqlite` 只补 `version=3.4`、`program.callgraph_primes/callgraph_all_primes/md5sum` 以通过 Python 入口，输出 `G:\DM\diaphora-cpp\test\python_diaphora_soff_pair_no_dirty.diaphora`；基线为 `best=88 partial=6 unreliable=0 multimatch=0 unmatched_primary=1 unmatched_secondary=1`。
- 已将 Python Diaphora 基线写入 `tests/fixtures/m5_native_fields_reexport_calibration.json` 的 `python_diaphora_baseline`，`soff_cli check-m5-fixture` 会同时校验 SOFF result summary 和 Python result summary。
- M4 与 Python Diaphora result 数量差异已归因：Python 基线包含 same-name、small-name、callee diffing、related constants、local affinity 等 M7 propagation/speedup 逻辑；SOFF M4 当前只校准 SQL heuristic runner、ratio 和 cleanup，因此 M4 不再把该差异作为 ratio 阻塞项。

未完成：

- 无。后续 Python Diaphora 完整 result parity 的数量追平依赖 M7 propagation/same-name/speedup 和 M10 多样本 parity，不再属于 M4 未完成项。

### M5: IDA export MVP

状态：已完成。后续符号导入、图形/汇编/伪代码联动和 Python Diaphora 多样本精确 parity 归 M7/M10 跟踪。

已完成：

- 已拆分 IDA action：顶层 `SOFF` 菜单提供 `Export current IDB` 和 `Diff SQLite databases` 两个独立入口；`soff:export` 只导出当前 IDB，`soff:diff` 只对两个 SQLite 导出库做 diff；`Edit -> Plugins -> SOFF` 这个 IDA 自动插件入口会弹出 Export/Diff 二选一，不再直接进入导出。
- 已实现 file_out 导出入口：IDA 内选择 SQLite 输出路径后导出当前 IDB。
- 已遍历 `get_func_qty()/getn_func()`，采集并写入 `functions` 基础字段：`name/address/rva/segment_rva/size/instructions/nodes/edges/mnemonics/bytes_hash/function_hash/constants_count/constants`。
- 已采集并写入 M5 基础关系表：`instructions`、`basic_blocks`、`function_bblocks`、`bb_instructions`、`bb_relations`、`callgraph`、`constants`。
- `soff_core` 仍保持 IDA SDK 无关；IDA SDK include 只在 `src/plugin/soff_plugin.cpp`。
- smoke 已覆盖新增基础表写入计数；`soff_ida` 已能构建为 `build/windows/x64/release/soff.dll`。
- 已在 IDA 9.3 中完成一次真实导出：`G:\DM\diaphora-cpp\test\1.sqlite`，`inspect-db` 输出 `functions=113 instructions=2381 basic_blocks=538 bb_relations=654 bb_instructions=2381 function_bblocks=538 callgraph=162 constants=267`。
- `soff_cli inspect-db` 已扩展为输出 M5 基础表计数，便于导出回归。
- 在 IDA 9.3 中实际加载 `soff.dll` 并导出当前 IDB。已完成一次真实导出。
- 已实现 Diaphora 默认函数过滤策略的 C++ 侧框架：默认跳过 `FUNC_LIB`、`FUNC_THUNK`、`nullsub_`，支持 `ida_subs`、`exclude_library_thunk`、`ignore_small_functions`。
- 已增补 M5 无反编译器字段写库/读库：`names`、`assembly`、`clean_assembly`、`assembly_addrs`、`bytes_sum`、`indegree/outdegree`、`function_flags`、`mangled_function`、`prototype/prototype2`、`comment`。
- 已将导出统计写入 `program_data`：总函数数、导出函数数、跳过函数数、导出选项、跳过原因。
- `soff_cli inspect-db --program-data` 已支持展开 `program_data`；snapshot 主摘要行输出 `program_data/export_total/export_exported/export_skipped`。
- 已接入 Diaphora 风格环境变量导出选项：`DIAPHORA_EXPORT_FILE`、`DIAPHORA_USE_DECOMPILER`、`DIAPHORA_EXCLUDE_LIBRARY_THUNK`、`DIAPHORA_IDA_SUBS`、`DIAPHORA_IGNORE_SMALL_FUNCTIONS`、`DIAPHORA_FROM_ADDRESS`、`DIAPHORA_TO_ADDRESS`。
- 已接入 Diaphora 风格环境变量 diff 选项：`DIAPHORA_DB1`、`DIAPHORA_DB2` / `DIAPHORA_FILE_IN`、`DIAPHORA_DIFF_OUT`、`DIAPHORA_SLOW_HEURISTICS`、`DIAPHORA_UNRELIABLE`、`DIAPHORA_EXPERIMENTAL`。
- 本轮验证通过：`xmake`、`xmake run soff_smoke`、旧真实导出库 `inspect-db --program-data`、旧真实导出库 self-diff（50 heuristics，best=56，unmatched_primary=57，unmatched_secondary=57）。
- 已重新用新 `soff.dll` 在 IDA 9.3 导出真实 DB：`G:\DM\diaphora-cpp\test\1.sqlite`。IDAMCP 当前 IDB 函数数 113，SOFF 默认过滤后导出 67，跳过 46，`program_data` 记录 `export.skip_reason.thunk=46`。
- 新真实导出字段填充率已校准：`assembly/clean_assembly/assembly_addrs/bytes_sum/function_flags=67/67`，`prototype/prototype2=65/67`，`comment=3/67`，`indegree_nonzero=42/67`，`outdegree_nonzero=33/67`。
- 新真实导出表计数已校准：`instructions=2335 basic_blocks=491 bb_relations=653 bb_instructions=2335 function_bblocks=491 callgraph=162 constants=267`。
- 新真实导出 self-diff 基线已固化到 `tests/fixtures/m5_real_export_calibration.json`：`heuristics=50 candidates=2820 accepted=56 best=56 partial=0 unreliable=0 multimatch=0 unmatched_primary=11 unmatched_secondary=11`。
- 已修正 match store 清理：弱候选若 primary 和 secondary 两端都已被前序强匹配覆盖，则跳过，不再写入 multimatch；保留一端已匹配的一对多冲突作为 multimatch。
- 已迁移并真实重导校准 Diaphora `bytes_hash/function_hash` 的导出语义：`bytes_hash` 对裁剪立即数/地址操作数后的指令字节做 MD5，`function_hash` 对完整 item bytes 做 MD5；真实 DB 中 hash 长度均为 32 字符十六进制 MD5。
- 已迁移并真实重导校准 Diaphora `get_cmp_asm()` 的主要 `clean_assembly` 规范化：去注释、归一化 `sub_/loc_/qword_/jpt_` 等地址名、去除 `dword ptr` 等指针前缀、规整偏移和字符串标签。
- 已迁移并真实重导校准 operand/import `names` 初版聚合：从指令操作数目标提取可见短 demangled name，过滤 `sub_`/`nullsub_`，以 JSON 数组写入 `functions.names`；`names_nonempty=54/67`。
- `soff_cli inspect-db --field-stats` 已支持输出 M5 字段填充率，包含 `names_nonempty`、`assembly`、`clean_assembly`、`bytes_sum`、`prototype`、`indegree/outdegree` 等。
- MD5 hash/clean assembly/names 后的新 self-diff 基线已更新：`heuristics=50 candidates=2815 accepted=56 best=56 partial=0 unreliable=0 multimatch=0 unmatched_primary=11 unmatched_secondary=11`。`Mnemonics and names` 当前 `candidates=51 skipped=51`，`Import names hash` 仍为 0。
- 已准备真实 two-file diff fixture 源码与二进制：`G:\DM\diaphora-cpp\test\soff_pair_a.c` / `soff_pair_b.c`，用 `clang -O1 -g -fno-inline -fno-omit-frame-pointer -Wl,--no-insert-timestamp` 编译为 `soff_pair_a.exe` / `soff_pair_b.exe`。样本包含稳定函数、重命名函数、常量变化、分支变化、删除/新增函数，用于替代 self-diff 做 M5 双库校准。
- 已补 M5 级 `md_index` 导出和 round-trip：基于 CFG 边、基本块拓扑顺序、源/目标入出度实现 Diaphora MD-Index 公式的 C++ double 版本；`soff_cli inspect-db --field-stats` 增加 `md_index_nonzero`。
- 已按 SOFF 自身工作流重做 IDA UI：导出表单只包含 `file_out/range/decompiler/exclude_library_thunk/ida_subs/ignore_small_functions`；diff 表单只包含 `primary sqlite/secondary sqlite/result db/max rows/timeout/slow/unreliable/experimental`。
- IDA diff 表单默认把当前 IDB 对应的默认导出库作为 primary SQLite（当前输入文件同名 `.sqlite`）；`DIAPHORA_DB1` 显式设置时优先使用环境变量。
- 已保留环境变量自动化但拆分语义：导出 action 使用 `DIAPHORA_EXPORT_FILE` 与导出选项；diff action 使用 `DIAPHORA_DB1`、`DIAPHORA_DB2` 或 `DIAPHORA_FILE_IN`、`DIAPHORA_DIFF_OUT` 与 diff 选项。导出不再自动触发 diff。
- 已补 IDA action 表单默认值持久化：使用 netnode 分别保存上次导出路径/范围/导出选项和 diff 数据库/结果路径/diff 选项，下次打开当前 IDB 时自动回填；环境变量仍优先覆盖自动化参数。
- 已补 diff runner/CLI/plugin 的 `experimental` 开关透传：`soff_cli diff-db --experimental` 与 IDA 表单/`DIAPHORA_EXPERIMENTAL` 都会设置 `SqlRunnerOptions::enable_experimental`。
- 已补导出 crash marker：导出开始创建 `<output>-crash`，成功写出 SQLite 后删除；如果 marker 已存在，会写入 `program_data export.crash_file_preexisting=true` 并在 IDA log 提示。
- 已增强 crash marker 可解释性：导出过程中 marker 会记录 `phase/index/total/exported/skipped/address/name`，新导出的 DB 会写入 `export.previous_crash_marker`、`export.last_function_*`、`export.crash_resume_supported=true`，便于定位上次中断位置。
- 已完成真实 two-file diff 校准：`G:\DM\diaphora-cpp\test\soff_pair_a.sqlite` vs `soff_pair_b.sqlite`，当前校准结果写入 `G:\DM\diaphora-cpp\soff\build\windows\x64\release\build\soff_pair_ab.diaphora`。导出均为 `total=96 exported=52 skipped_thunk=44`。
- 新 DLL 重导出的双库已校准新增 M5 元数据：两侧 `program_data=21`，`export.crash_resume_supported=false`，`export.last_function_index=95`，`export.last_function_name=strncmp`。
- 双库字段填充率已校准：A `names_nonempty=43/52 md_index_nonzero=33/52`，B `names_nonempty=44/52 md_index_nonzero=34/52`，两侧 `assembly/clean_assembly/bytes_sum/function_flags=52/52`，`prototype/prototype2=50/52`。
- 双库 diff 基线已固化到 `tests/fixtures/m5_two_file_diff_calibration.json`：`heuristics=50 candidates=1397 accepted=38 best=33 partial=5 unreliable=0 multimatch=3 unmatched_primary=14 unmatched_secondary=14`。
- 双库应用函数校准：`stable_mix` 为 best；`main`、`inventory_total`、`item_init`、`recursive_checksum`、`branchy_dispatch` 为 partial；`removed_only_in_a`/`added_only_in_b` 按预期 unmatched。
- `Import names hash` 在双库基线中已产生 `candidates=1`，但被前序更强 heuristic 覆盖而 `skipped=1`；说明 `names` + `md_index` 输入已具备，后续需在更多样本校准命中质量。
- 已补导出库/diff 输入 sanity check：IDA diff 表单、plugin 自动化入口和 `soff_cli diff-db` 都会检查 primary/secondary 是否存在、是否 0 字节、是否可读 SQLite、是否包含 `version/program/functions`，以及 `functions` 是否非空，并输出明确错误。
- 已细化 diff 表单默认路径：primary 默认当前 IDB 的同名 `.sqlite`，result DB 默认生成在 primary 目录下，命名为 `<primary>_vs_<secondary>.diaphora`；secondary/result 仍参与最近路径记忆。
- 已补导出完成摘要：IDA 导出结束后显示输出路径、导出/总数/跳过数、字段填充率、跳过原因和 crash marker 清理状态，便于重导后直接核对。
- 已补 `inspect-db` 校准输出：`--summary-json` 输出机器可比摘要，`--nonzero-heuristics` 只显示非零 heuristic 统计，`--unmatched` 展开 unmatched 行，减少人工记录成本。
- 已补 CLI `diff-db` 输入 DB sanity check，和 IDA 插件侧使用同一类错误语义，避免拿空文件/坏库进入 diff。
- 已补 crash resume 真实续写：检测旧 `<output>-crash` 后会展示 marker 内容，并提供删除 marker 后全量重导、Resume existing DB、取消三种选择；Resume 会读取现有 SQLite 的 `functions.address`，跳过已写入函数，新函数按批 append 到旧 DB，结束后重写 `program_data` 并创建索引。
- 已补导出分批 commit：IDA 导出使用 `SnapshotRepository::begin_incremental_save/append_functions/replace_program_data/finalize_incremental_save`，默认每 16 个新函数提交一批；`program_data` 写入 `export.resumed_functions`、`export.batch_commits`、`export.crash_resume_used`。
- 已补 native CFG 拓扑字段导出：基于 IDA `qflow_chart_t` 计算 Tarjan SCC、robust topological sort JSON、`loops`、`strongly_connected`、`strongly_connected_spp`，并改用拓扑顺序参与 M5 `md_index` 计算。
- 已补 native `switches` 导出：基于 IDA `get_switch_info()` / `calc_switch_cases()` 写入 Diaphora 风格 `[[jtable_size, [case_values...]]]` JSON，支撑 `Switch structures` heuristic。
- 已补 native `mnemonics_spp` 和 `cyclomatic_complexity/primes_value` 写库/读库：按处理器指令列表和 Diaphora primes 规则计算指令小素数乘积，基础图复杂度字段开始参与 SQL heuristic。
- 已补 M5 字段 round-trip 和观测：`SnapshotRepository` 保存/加载 `switches`、`mnemonics_spp`、`cyclomatic_complexity`、`primes_value`、`strongly_connected`、`loops`、`tarjan_topological_sort`、`strongly_connected_spp`；`inspect-db --field-stats` 增加这些字段的填充率。
- 已补导出性能指标：IDA 导出成功后写入 `program_data export.elapsed_seconds` 和 `export.functions_per_second`，导出摘要同步显示耗时。
- 已完成新 native 字段真实重导校准：`G:\DM\diaphora-cpp\test\soff_pair_a.sqlite` vs `soff_pair_b.sqlite`，本次导出选项为 `exclude_library_thunk=false`、`ida_subs=false`，两侧 `total=96 exported=95 skipped=1`，跳过原因为 `ida-generated-name=1`。
- 新 native 字段填充率已校准：A/B 两侧 `cyclomatic_nonzero=95/95`、`strongly_connected_nonzero=95/95`、`tarjan_topological_sort=95/95`、`mnemonics_spp_nontrivial=95/95`、`switches_nonempty=6/95`；A `loops_nonzero=18/95`，B `loops_nonzero=19/95`。
- 新 native 字段双库 diff 基线已固化到 `tests/fixtures/m5_native_fields_reexport_calibration.json`：`heuristics=50 candidates=1652 accepted=38 best=32 partial=5 unreliable=1 multimatch=3 unmatched_primary=57 unmatched_secondary=57`。
- native 字段相关 heuristic 已产生候选：`Mnemonics small-primes-product=27`、`Switch structures=11`、`Loop count=20`、`Same graph=20`、`Strongly connected components=23`、`Topological sort hash=14`；多数被更强前序匹配覆盖后 skipped。
- 已补 IDA diff 结果加载入口：顶层 `SOFF/Load Diff Results` 可选择 `.diaphora`，diff 完成后自动打开同一基础 chooser；chooser 显示 best/partial/unreliable/multimatch/unmatched，双击 primary 地址可跳转当前 IDB。
- 已修正结果 chooser 生命周期风险：移除 `PLUGIN_UNL`，避免 IDA 在结果窗口刷新/绘制时调用已卸载 DLL；结果 chooser 改为 modal chooser，降低回调生命周期风险。
- 已补 M5 fixture 自动校验命令：`soff_cli check-m5-fixture <fixture.json> [--out <result.diaphora>]` 会读取 fixture 中的 DB 路径，校验两侧导出表计数/字段填充率，重新跑 diff，并比对 result summary 与记录的 heuristic 统计。
- 已增强导出取消体验：waitbox 刷新粒度从每 64 个函数降到每 8 个函数并显示 exported/skipped；skip 会写入 `phase=skipped-<reason>`；用户取消写入 `phase=cancelled`；异常写入 `phase=failed`，保留最后函数地址/名称和计数。
- 已完成 M5 依赖 M6 的反编译器路径收口：`decompiler` 选项现在实际驱动 pseudocode、clean_pseudo、AST prime、Koret fuzzy hash、microcode text/SPP、microcode CFG/relations、pseudocode comments、KGH 与 decompiler prototype fallback 导出。
- 已完成 M5 依赖 M6 的 prototype parity：`prototype2` 保留 IDA attached type，`prototype` 使用 `guess_tinfo()`，启用 Hex-Rays 时以伪代码第一行覆盖，已真实重导校准 A/B 两侧 `prototype=95/95`、`prototype2=94/95`。
- 已修正 M5 `md_index` 语义：公式已按 Diaphora 顺序改为 `topo + src_in*sqrt2 + src_out*sqrt3 + dst_in*sqrt5 + dst_out*sqrt7`，并用 `long double` 输出 21 位精度；当前不引入 Boost，完整 Python `decimal.Decimal` 逐位一致仍归入 M10 parity。
- 已完成 M5 依赖 M6 的 `Import names hash` 当前样本校准：当前真实 A/B fixture 中仍产生 `candidates=1 skipped=1`，说明输入字段稳定；更多 Python Diaphora 多样本质量对照归入 M10。
- 已完成旧 tester 基础能力迁移的当前样本收口：`tests/fixtures/m5_native_fields_reexport_calibration.json` 已更新为 M5+M6 真实导出基线，`soff_cli check-m5-fixture` 校验通过。
- M5+M6 当前真实基线：A/B 两侧 `total=96 exported=95 skipped=1`，A `instructions=8721 basic_blocks=981 bb_relations=1174`，B `instructions=8837 basic_blocks=994 bb_relations=1192`；两侧 `pseudocode/microcode/kgh_hash_nontrivial=95/95`。
- M5+M6 当前 diff 基线：`G:\DM\diaphora-cpp\test\soff_pair_a_vs_soff_pair_b_m5_dep_m6.diaphora` 得到 `heuristics=50 candidates=2002 accepted=39 best=33 partial=5 unreliable=1 multimatch=0 unmatched_primary=56 unmatched_secondary=56`。

未完成：

不依赖 M6、可继续推进的 M5 任务：

- 当前无明确不依赖 M6 的 M5 阻塞项；后续主要进入结果导入/图形 diff、Python Diaphora parity 和 M6 反编译器特征。

依赖 M6 或后续阶段的 M5 关联任务：

- IDA action 交互表单的符号导入、图形 diff、伪代码/汇编 diff 联动属于 M7 UI/import 范围，不再作为 M5 阻塞项跟踪。
- `experimental` 开关已透传；后续 experimental heuristic 增量随具体 descriptor 进入 M10 parity。
- `md_index` 已修正 Diaphora 公式和排序输入；完整 `decimal.Decimal` 逐位一致仍需后续引入高精度 decimal 依赖或专用实现，归 M10 parity。
- `Import names hash` 当前 fixture 已校准；更多样本和 Python Diaphora 命中质量对照归 M10 parity。
- crash resume 已完成 M5 范围内的旧 DB 续写和分批 commit。限制：Resume 假定导出过滤选项与中断前一致；不会删除旧 DB 中因选项变化而现在应跳过的函数。更复杂的旧 DB 清理/迁移策略如需要归 M10 parity/robustness。
- 旧 Python tester 的基础表计数当前 A/B 样本已由 `check-m5-fixture` 覆盖；更多样本 manifest 归 M10。

### M6: Pseudocode、microcode 和高级特征

状态：已完成。

已完成：

- Hex-Rays 初始化第一版：IDA 导出勾选 `Use decompiler` 时会调用 `init_hexrays_plugin()`，必要时按 `DIAPHORA_DECOMPILER_PLUGIN` 或默认 `hexrays` 尝试 `load_plugin()`，不可用时降级为空 pseudocode 字段继续导出。
- pseudocode 导出第一版：每个函数通过 `decompile_func(..., DECOMP_NO_WAIT)` 读取 `cfunc_t::get_pseudocode()`，去除 IDA color tags，跳过声明行和 `//` 注释行，写入 `functions.pseudocode` 和 `functions.pseudocode_lines`。
- `clean_pseudo` 第一版：迁移 Diaphora `get_cmp_pseudo_lines()` 的基础清理规则，替换 `sub_/loc_/byte_/word_/dword_...`、`vNNN`、`aNNN`、`arg_NNN` 等不稳定文本。
- DB/model round-trip 已补：`FunctionFeature`、`SnapshotRepository::save/load()` 已覆盖 `pseudocode`、`clean_pseudo`、`pseudocode_lines`、`pseudocode_hash1/2/3`、`pseudocode_primes`、`microcode`、`clean_microcode`、`microcode_spp`。
- `inspect-db --field-stats` 已增加 pseudocode/microcode 字段填充率统计；IDA 导出摘要增加 pseudo/clean_pseudo/pseudocode_primes 填充率。
- 导出元数据已增加 `export.hexrays_available`、`export.pseudocode_functions`、`export.pseudocode_failures`，用于真实重导后校准 Hex-Rays 覆盖率。
- 已迁移 Diaphora `CKoretFuzzyHashing` 默认算法：`pseudocode_hash1/2/3` 现在分别来自 mixed/original/reversed pseudocode bytes 的 Koret fuzzy hash，不再使用 MD5 占位。
- 已完成 M6 pseudocode 真实重导校准：`G:\DM\diaphora-cpp\test\soff_pair_a.sqlite` 与 `soff_pair_b.sqlite` 均为 `total=96 exported=95 skipped=1`，`export.decompiler=true`、`export.hexrays_available=true`、`export.pseudocode_functions=95`、`export.pseudocode_failures=0`。
- 已迁移 ctree AST prime hash：插件侧使用 `ctree_visitor_t(CV_FAST)` 遍历 `cfunc_t::body`，按 Diaphora `CAstVisitor` 语义对 `expr.op`/`ins.op` 对应素数做十进制大整数乘积，写入 `functions.pseudocode_primes`。
- M6 pseudocode 字段填充率已校准：A/B 两侧 `pseudocode=95/95`、`clean_pseudo=95/95`、`pseudocode_lines=95/95`、`pseudocode_primes=95/95`；当前 fixture 函数较短，真实 `CKoretFuzzyHashing` 对 `pseudocode_hash1/2/3` 返回空值，已用 Python Diaphora 逐函数校验 `mismatches=0`。
- 已修复未实现高级字段的空字符串污染：`SnapshotRepository` 写入可选高级文本时使用 `NULLIF(?, '')`，避免未采集的 `pseudocode/pseudocode_hash/pseudocode_primes/microcode/clean_microcode` 以空字符串进入 DB。
- 已增强 heuristic 防线：`Same cleaned microcode` 与 `Same cleaned pseudo-code` 增加 `length(coalesce(...,'')) > 0`，即使旧库中存在空字符串，也不会把空字段误当有效 microcode/pseudocode。
- M6 当前双库 diff 校准：`soff_pair_a.sqlite` vs `soff_pair_b.sqlite` 使用 `--unreliable --max-rows 200000 --timeout 120` 得到 `heuristics=50 candidates=1882 accepted=38 best=32 partial=5 unreliable=1 multimatch=8 unmatched_primary=57 unmatched_secondary=57`；`Same cleaned pseudo-code` 产生 `candidates=29 accepted=2 skipped=27`，命中 `__report_error` 与 `_amsg_exit`。
- M6 AST prime hash 重导后 diff 校准：`G:\DM\diaphora-cpp\test\soff_pair_a_vs_soff_pair_b_m6_ast.diaphora` 得到 `heuristics=50 candidates=1914 accepted=39 best=32 partial=6 unreliable=1 multimatch=8 unmatched_primary=56 unmatched_secondary=56`；`Pseudo-code fuzzy AST hash` 产生 `candidates=32 accepted=1 skipped=31`，新增命中 `clamp_score -> clamp_score_v2`。
- M6 kfuzzy 重导后 diff 校准：`G:\DM\diaphora-cpp\test\soff_pair_a_vs_soff_pair_b_m6_kfuzzy.diaphora` 得到 `heuristics=50 candidates=1749 accepted=39 best=32 partial=6 unreliable=1 multimatch=8 unmatched_primary=56 unmatched_secondary=56`；短伪代码样本的 normal/mixed/reverse fuzzy hash 候选归零，但 AST prime hash 仍保留 `clamp_score -> clamp_score_v2`。
- 已迁移 microcode 文本导出：启用 Hex-Rays 时通过 `gen_microcode(..., MMAT_GENERATED)` 和 `mba_t::print()` 采集 `functions.microcode`，移除 color tags 和注释地址，按 Diaphora 逻辑过滤短行。
- 已迁移 `clean_microcode` 与 `microcode_spp`：clean 使用现有 assembly 归一化规则保留 microcode mnemonic；SPP 使用排序后的 Hex-Rays `m_*` mnemonic 名称映射素数并做十进制大整数乘积。
- 已补 Hex-Rays failure 分类和缓存清理统计：导出元数据写入 `export.microcode_functions`、`export.microcode_failures`、`export.hexrays_cache_clears`，失败时写 `export.hexrays_failure.<code@ea:desc>`。
- M6 microcode 重导校准：A/B 两侧 `microcode=95/95`、`clean_microcode=95/95`、`microcode_spp_nontrivial=95/95`，`export.microcode_functions=95`、`export.microcode_failures=0`。
- M6 microcode diff 校准：`G:\DM\diaphora-cpp\test\soff_pair_a_vs_soff_pair_b_m6_micro.diaphora` 得到 `heuristics=50 candidates=1821 accepted=39 best=33 partial=5 unreliable=1 multimatch=8 unmatched_primary=56 unmatched_secondary=56`；`Same cleaned microcode` 产生 `candidates=33 accepted=3 skipped=30`，命中 `clamp_score -> clamp_score_v2`、`__report_error`、`_amsg_exit`。
- M6 no-decompiler 降级校准：`G:\DM\diaphora-cpp\test\soff_pair_a_nodecomp.sqlite` 使用 `DIAPHORA_USE_DECOMPILER=0` 导出成功，`export.decompiler=false`、`export.hexrays_available=false`，pseudocode/microcode 字段均为 `0/95`，native 导出表计数保持正常。
- 已落库 microcode basic blocks/relations：`FunctionFeature` 增加 `microcode_blocks` 和 `microcode_instruction_details`，repository 将其写入 `instructions/basic_blocks/function_bblocks/bb_relations/bb_instructions`，用 `asm_type='microcode'` 与 native CFG 区分。
- 已修正 native instruction heuristic 污染：`Same rare assembly instruction` 与 `Same rare basic block mnemonics list` 限定 `coalesce(asm_type,'')=''`，避免 microcode 行进入 assembly-only 统计。
- M6 microcode graph 重导校准：A 侧新增 `instructions(microcode)=6895`、`basic_blocks(microcode)=521`、`function_bblocks(microcode)=521`、`bb_relations(microcode)=603`、`bb_instructions(microcode)=6895`；B 侧新增 `instructions(microcode)=6993`、`basic_blocks(microcode)=529`、`function_bblocks(microcode)=529`、`bb_relations(microcode)=614`、`bb_instructions(microcode)=6993`。
- M6 microcode graph diff 校准：`G:\DM\diaphora-cpp\test\soff_pair_a_vs_soff_pair_b_m6_micrograph.diaphora` 得到 `heuristics=50 candidates=1821 accepted=39 best=33 partial=5 unreliable=1 multimatch=8 unmatched_primary=56 unmatched_secondary=56`，与 microcode text/SPP 版结果稳定一致。
- 已迁移 pseudocode user comments：导出时读取 Hex-Rays `restore_user_cmts()`，按 `treeloc_t.ea` 挂到 native `instructions.pseudocomment/pseudoitp`，并写入 `export.pseudocode_comments`；`inspect-db --field-stats` 增加 `instruction_pseudocomments`/`instruction_pseudoitp` 观测项。
- M6 pseudocode comments 重导校准：当前 A/B fixture 无用户伪代码注释，`export.pseudocode_comments=0`，`instruction_pseudocomments=0`，作为空注释路径基线。
- 已迁移 Diaphora `guess_type()`/decompiler fallback prototype 语义：`prototype2` 保留 IDA attached type，`prototype` 先用 `guess_tinfo()`，启用 Hex-Rays 且反编译成功时用伪代码第一行覆盖；这对齐 Python Diaphora 的 `proto`/`proto2` 分工。
- M6 decompiler prototype 重导校准：A/B 两侧 `prototype=95/95`、`prototype2=94/95`；示例差异包括 `WinMainCRTStartup`、`main`、`item_init` 等，说明 `prototype` 已来自 decompiler fallback。
- 已迁移 KOKA/KGH CFG hash：按 Diaphora `CKoretKaramitasHash` 语义计算 CFG 节点/边、call/data/code refs、loop/SCC、`FUNC_NORET/FUNC_LIB/FUNC_THUNK` 的素数乘积，写入 `functions.kgh_hash` 并支持 repository round-trip。
- M6 KGH 重导校准：A/B 两侧 `kgh_hash_nontrivial=95/95`；`G:\DM\diaphora-cpp\test\soff_pair_a_vs_soff_pair_b_m6_kgh.diaphora` 得到 `heuristics=50 candidates=2002 accepted=39 best=33 partial=5 unreliable=1 multimatch=0 unmatched_primary=56 unmatched_secondary=56`；`Same KOKA hash and constants` 产生 `candidates=178 accepted=1 skipped=177`，命中 `recursive_checksum -> recursive_checksum`。

未完成：

- 当前无 M6 阶段剩余阻塞项；后续精确 parity 继续进入 M10，包括 Python Diaphora 多样本对照、KGH 边界样本和 `md_index` Decimal 级细节校准。

任务：

- Hex-Rays 初始化和 decompile wrapper。
- pseudocode、clean_pseudo、pseudocode_lines、fuzzy hash、ctree prime hash。
- microcode text、clean_microcode、microcode_spp、microcode basic blocks。
- SCC、loops、topological sort、MD index、KGH。

验收：

- 启用 Hex-Rays 时高级字段非空。
- 不可用 decompiler 时导出仍可完成。
- same CPU / no decompiler 场景结果稳定。

### M7: IDA results UI 和 import

状态：已完成。后续图形 diff、伪代码/汇编联动、局部 diff 归 M8 继续。

任务：

- 已完成：best/partial/unreliable/multimatch/unmatched 基础 chooser，可加载 C++/Python `.diaphora` result DB。
- 已完成：show/load results，`SOFF/Load Diff Results` 独立入口；插件本体使用 `PLUGIN_HIDE`，不再在 Edit -> Plugins 暴露额外 SOFF 总入口。
- 已完成：chooser 双击跳转 primary 函数地址。
- 已完成：第一版 UI-agnostic `ImportPlan`，默认从 best/partial 中筛选函数名导入候选，跳过 multimatch、unreliable、低 ratio、同名、空名和 `sub_/nullsub_/loc_/unk_` 等自动名。
- 已完成：`SOFF/Import Diff Results` 独立入口，先预览导入数量和跳过原因，再对当前 IDB 应用函数 rename。
- 已完成：`SnapshotRepository::load()` 读回 native/microcode `instructions` 明细，为注释导入提供 source snapshot。
- 已完成：`ImportPlan` 可从 result DB 的 `main_db/diff_db` 加载 primary/secondary 导出库，生成 rename/prototype/function comment/function flags/instruction comments/forced operands/pseudo comments 的批量计划。
- 已完成：`Import Diff Results` 支持 all/auto 批量导入；默认导入 names/prototypes/function comments/instruction comments/forced operands/pseudo comments，`function_flags` 通过 `DIAPHORA_IMPORT_FLAGS=1` 显式开启。
- 已完成：chooser 内 import one 的基础路径：在结果 chooser 上触发 Edit 可对当前行导入 metadata；Enter 仍保持跳转。
- 已完成：save results 独立 UI 入口：`SOFF/Save Diff Results As` 复制最近加载/生成的 result DB。
- 已完成：结果 chooser 已升级为 multi-selection，触发 Edit 可对选中行批量导入 metadata；Enter 跳转第一个选中项。
- 已完成：forced operands 导出/导入链路：导出时保存 `get_forced_operand()` 的 operand index/string 到 `instructions.operand_names`，导入时调用 `set_forced_operand()`。
- 已完成：local type definitions 基础导出/导入链路：导出时用 `print_decls()` 将 local `struct/enum/union` 写入 `program_data`，导入时用 `parse_decls()` 多轮解析 secondary definitions。
- 已完成：TIL 基础导入链路：从 secondary `program_data(type='til')` 调用 `add_til()`；TIL 名称导出仍依赖后续 `.til` 元数据读取完善。
- 已完成：chooser selected import 已把选中 result rows 映射为临时 `DiffResultSet`，优先加载 `main_db/diff_db` 快照并生成完整 `ImportPlan`，覆盖函数名、prototype、函数注释、function flags、指令注释、forced operands、伪代码注释；快照缺失时才退回 result DB 名称导入。
- 已完成：导入后可刷新当前 DB 对应快照：bulk import 和 selected import 完成后支持通过 UI 提示或 `DIAPHORA_IMPORT_REFRESH_DB=1` 重新导出当前 IDB 到 result DB 记录的 primary export SQLite；当前实现为全库重导出，不做单函数 in-place patch。

验收：

- 已完成：可加载 C++ result DB 并显示。
- 已完成：可从独立 Import 入口和 chooser selected import 导入函数名、prototype、函数注释、指令注释、forced operands、伪代码注释、TIL 和 local struct/enum/union definitions；function flags 为显式可选。
- 已完成：导入后可按提示或 `DIAPHORA_IMPORT_REFRESH_DB=1` 更新当前 DB 对应 export 快照。

### M8: Diff views、graph views、local diff

状态：已完成。

任务：

- 已完成：UI-agnostic HTML diff engine：`include/soff/ui/html_diff.hpp` / `src/ui/html_diff.cpp`，支持 assembly、clean assembly、pseudocode、clean pseudocode、microcode、clean microcode 的 side-by-side 行级 diff，并做 HTML escaping。
- 已完成：result chooser matched row 的 diff 入口重组为统一 `SOFF Match Diff Panel`，不再用多层按钮混放功能；面板按 Diaphora 真实语义组织：`Assembly` 和 `Microcode` 提供 `Flat` 与 `Basic blocks`，`Pseudocode` 只提供 `Flat`，另保留 `Context` 与 `HTML export` 辅助项。
- 已完成：Flat 模式使用 IDA 内置文本 diff；Basic blocks 模式中 Assembly 使用 native CFG + assembly block summaries，Microcode 使用 microcode CFG + microcode block summaries。已审计 Diaphora 源码，原版只有 `Diff assembly in a graph` 与 `Diff microcode in a graph`，没有 pseudocode graph/basic-block diff，因此 SOFF 主面板不提供伪代码基本块假入口。
- 已完成：内置文本 diff 顶部和面板目标列使用导出 DB 文件名 + 函数名（例如 `a.sqlite :: foo` / `b.sqlite :: foo_v2`），避免从任意 IDA 实例打开同一个 `.diaphora` 时左右来源混淆。
- 已完成：HTML diff 被降级为 `HTML export` 辅助项；它和 Text diff 使用同一类文本内容，主要用于浏览器查看、保存报告或静态分享，默认分析工作流走 IDA 内置 Text/Graph。
- 已完成：native graph diff、microcode graph diff 的第一版 HTML 视图：`render_function_graph_diff_html()` 按 basic block 序号对齐，对比节点数、边数、block 地址、指令数和后继列表，并用 equal/changed/inserted/deleted 颜色标记；result chooser 的 `More -> Graph diff` 可选择 Native CFG 或 Microcode CFG 并打开临时 HTML。
- 已完成：caller/callee context graph 第一版 HTML 视图：`SnapshotRepository::load()` 已读回 `callgraph` 到 `FunctionFeature::call_references`；`render_call_context_diff_html()` 可按 matched function 对比 callers/callees 数量、函数名、地址和 call type；result chooser 的 `More -> Call context` 会打开临时 HTML，缺失 call context 时显式提示。
- 已完成：local diff 第一版：新增独立 `SOFF/Local Function Diff` / `soff:local_diff` action，可在当前 IDB 内选择两个函数地址，复用 IDA feature reader 生成 text/native graph/microcode graph HTML diff；默认 primary 为当前光标函数，secondary 为下一函数；支持 `SOFF_LOCAL_DIFF_EA1/SOFF_LOCAL_DIFF_EA2` 或 `DIAPHORA_LOCAL_DIFF_EA1/DIAPHORA_LOCAL_DIFF_EA2` 自动化地址输入。
- 已完成：`SnapshotRepository::load()` 已读回两个导出 DB 的 `basic_blocks`、`bb_relations`、`bb_instructions`、`function_bblocks`，恢复 native/microcode basic block graph model；smoke 覆盖 block/instruction/successor round-trip。
- 已完成：Diaphora 等价工作流的第一版 IDA 内双文件函数级图形化 diff：从 result chooser 的 match 进入 `IDA graph diff`，在 IDA GraphViewer 中展示 primary/diff 函数基本块对比，而不是浏览器 HTML。
- 已完成：IDA 内基本块级 graph diff 第一版：按 basic block 序号对齐并标记 `same`、`changed`、`primary-only`、`secondary-only`，合并 primary/secondary CFG edge 后在 IDA 原生 GraphViewer 中展示。
- 已完成：GraphViewer 节点内容显示 primary/secondary 基本块地址、后继、汇编或 microcode 摘要；双击节点会跳转当前 IDB 可映射的 primary 地址，secondary 地址以来源 DB 地址显示在节点和 hint 中。
- 已完成：result chooser 增加明确入口：`IDA graph diff -> Function Graph Diff / Microcode Graph Diff`，并保留独立的 `HTML graph preview`，不再把原生图和 HTML 预览混成一个动作。
- 已完成：IDA GraphViewer 改为两张独立图对比：Primary Graph 和 Secondary Graph 分别展示两侧 CFG，不再把两边节点混到同一张图里；节点颜色标记 `same/changed/primary-only/secondary-only`，节点文本保留 paired address。
- 已完成：GraphViewer 双窗节点同步：点击任一图节点会自动把另一张图居中到 paired node；右键菜单提供 `Sync paired graph node` 手动同步和 `Jump to primary block` 跳转当前 IDB 地址。
- 已完成：GraphViewer 边级差异分类：CFG edge 按 paired blocks 判断为 common、primary-only、secondary-only，并用 green/red/blue 区分，避免只按左右来源粗略染色。
- 已完成：GraphViewer 节点右键联动：可从图节点打开当前 match 的 assembly/pseudocode/microcode IDA 内置文本 diff，也可直接执行当前 match metadata import。
- 已完成：GraphViewer 到文本 diff 的基本块定位：从图节点打开文本 diff 时会传入 paired block 地址，IDA custom viewer 初始定位到匹配地址附近，并在标题区显示 focus block。
- 已完成：GraphViewer 节点右键导入收窄为基本块粒度：`Import block metadata` 只应用当前 primary block 指令范围内的 instruction comments、repeatable comments、forced operands 和 pseudocode comments，不再默认整函数导入。
- 已完成：GraphViewer block pairing 从纯序号升级为多阶段保守策略：先按基本块指令文本 exact signature 配对，再用 LCS 文本相似度、指令数、出入度和 ordinal proximity 做 greedy similarity pairing，最后仅在低置信阈值通过时做 ordinal fallback；节点中显示 pairing reason/score，便于人工审计错配。
- 已完成：修复 IDA GraphViewer 空窗口问题：按 IDA SDK `ugraph` 示例改为由 `create_graph_viewer()` 的 `grcode_user_refresh` 创建/刷新 graph，不再手动 `create_interactive_graph()` + `set_viewer_graph()`。
- 已完成：microcode graph 入口增加降级：当当前 result DB 来源导出库没有 microcode blocks，但 native CFG 存在时，会提示需要启用 decompiler 重导，并自动打开 native function graph，而不是直接失败。
- 已完成：导出默认启用 decompiler；可通过 UI 取消勾选或 `DIAPHORA_USE_DECOMPILER=0` 关闭。新持久化格式能明确保存关闭状态，旧配置没有明确关闭标记时按新默认开启，以避免后续 microcode graph 数据缺失。
- 已完成：GraphViewer 交互联动已收口：已完成点击/右键触发的双窗节点同步、边级 common/only 分类、文本 diff 基本块定位、节点右键基本块粒度导入、多阶段 block matching。滚动/缩放级联动受 IDA 9.3 C++ SDK 公共 API 限制暂不实现：公开头仅前置声明 `graph_location_info_t`，插件侧无法稳定实例化并复制 viewport 状态；当前以 paired node 居中同步替代。

验收：

- 已完成：HTML diff engine 有 smoke 覆盖，插件可构建。
- 已完成：HTML/IDA text diff 缺失时会显式提示。
- 已完成：native/microcode graph diff HTML 有 smoke 覆盖，缺失 graph blocks 时会显式提示。
- 已完成：caller/callee context graph HTML 有 smoke 覆盖，repository callgraph round-trip 有断言覆盖。
- 已完成：local diff action 可构建，复用已有 HTML diff 和 graph diff renderer；该入口不修改 IDB、不依赖 SQLite 导出库。
- 已完成：GraphViewer 当前交互验收已完成；与 Python UI 的滚动/缩放级完全同步因 IDA SDK 公共 API 限制暂不实现，必要时的伪代码行级/ctree 级定位归入后续增强。
- 已完成：graph HTML 视图可用于初步查看；IDA GraphViewer 交互布局已具备双侧节点图、pairing edge、CFG edge 颜色和 result match 入口。
- 已完成：从两个文件/两个导出 DB 的 result match 出发，在 IDA 内完成函数级和基本块级图形比较，达到 Diaphora 主图形化工作流的第一版可用性。

### M9: Hooks、patch diff helper、ML

状态：已完成。

已完成：

- C++ hook interface：`include/soff/core/hooks.hpp` 定义 `DiffHooks` 虚基类，支持 5 个 diff 阶段回调点：`get_heuristics`（过滤/重排 heuristic 列表）、`on_launch_heuristic`（修改 SQL 或跳过）、`get_queries_postfix`（修改 WHERE 子句）、`on_match`（接受/拒绝/调整 ratio）、`on_finish`（完成回调）。
- Hook 集成：`SqlRunnerOptions` 和 `DiffSessionOptions` 增加 `DiffHooks* hooks` 非拥有指针；`sql_runner.cpp::run_all()` 在 4 个位置插入 null-checked hook 调用；`session.cpp::run_session()` 在 cleanup 后调用 `on_finish()`。
- Patch diff vulnerability analyzer：`include/soff/diff/patch_diff.hpp` + `src/diff/patch_diff.cpp`，实现 3 种模式检测：signed/unsigned 指令变化（jl↔jb、jle↔jbe、jg↔ja、jge↔jae）、unsafe function pattern（cpy/printf/strcat/alloc/free/system/scanf 等 18 种模式）、size check added（新增 `if ... < ...` / `if ... > ...` 比较）。
- Patch diff 双入口：`analyze_patch_diff()` 独立函数用于 CLI 后处理已有 result DB；`PatchDiffHook : DiffHooks` 用于 diff 过程中实时收集。
- ML 特征导出：`include/soff/diff/ml_features.hpp` + `src/diff/ml_features.cpp`，19 字段 `MlFeatureVector`（对齐 Python `DATA_FRAME_FIELDS`），`extract_ml_features()` 从 functions/diff.functions 查询每对 match 的 nodes/edges/strongly_connected/loops/constants/pseudocode_primes/source_file 并计算 min/max/ratio。
- ML 导出格式：`export_ml_features_csv()` 和 `export_ml_features_json()` 输出标准 CSV/JSON。
- CLI 命令：`soff_cli patch-diff <result> <main> <diff> [--json]` 输出 findings；`soff_cli ml-export <result> <main> <diff> --out <file.csv|.json>` 输出特征向量。
- Smoke 测试：hook dispatch 验证（过滤 heuristic、on_match 调用计数、on_finish 标志）；patch diff 模式检测单元测试（signed/unsigned、unsafe function、size check）；ML 特征提取和 CSV 导出验证。

未完成：

- Export 阶段 hook（`before_export_function`、`after_export_function`、`on_export_crash`）属于 IDA 插件层，当前不在 `soff_core` 中实现；后续可在 `soff_plugin.cpp` 中增加 export hook 调用点。
- Python/IDC 脚本桥接：当前只有 C++ virtual interface，脚本加载/执行需要 embedded Python 或 IDC eval，归入后续增强。
- Hook 链式组合：当前单 `DiffHooks*`，多 hook 组合可通过 composite pattern 后续扩展。
- ML 模型推理：当前只输出特征，不内置 sklearn/ONNX 推理；外部模型可读取 CSV 做预测。

验收：

- 已完成：`xmake` 构建所有目标通过（soff_core、soff_cli、soff_smoke、soff_ida）。
- 已完成：`xmake run soff_smoke` 通过，包含 hook dispatch、patch diff 模式检测、ML 特征导出测试。
- 已完成：`soff_cli patch-diff` 可对已有 result DB 运行 patch diff 分析。
- 已完成：`soff_cli ml-export` 可输出 CSV/JSON 特征向量。
- 已完成：hooks 可过滤 heuristics（`on_launch_heuristic` 返回 nullopt 跳过）、拦截 match（`on_match` 可拒绝或调整 ratio）。
- 已完成：`soff_core` 无 IDA SDK include，新增文件均在 core/diff 模块。

### M10: Tester 和 parity

状态：已完成。

已完成：

- Propagation engine：`include/soff/diff/propagation.hpp` + `src/diff/propagation.cpp`，实现 `find_same_name()`（按函数名匹配，计算 text ratio）和 `find_locally_affine_functions()`（在已匹配对之间的地址间隙中暴力匹配同名函数）。
- Propagation 集成：`DiffSessionOptions` 增加 `PropagationOptions`（enabled、max_iterations、min_ratio、max_gap_size）；`DiffSession::run_all()` 在 SQL heuristics 完成后自动运行 propagation loop。
- Fixture manifest：`tests/fixtures/manifest.json`，JSON 格式记录每个样本的 main_db、diff_db、python_baseline（best/partial/unreliable/multimatch/unmatched）、options。
- Batch CLI：`soff_cli batch-diff <manifest.json> [--out <dir>]` 遍历 manifest 中所有样本，运行 diff，输出 SOFF 结果 vs Python baseline 的 delta。
- Parity report：`soff_cli parity-report <manifest.json>` 同 batch-diff，输出 per-sample parity 对比。
- Parity 结果：soff_pair 样本 SOFF 产出 `best=88 partial=6`（含 propagation），Python baseline `best=33 partial=5`；SOFF 通过 same-name propagation 多找到 55 个 best match。
- Smoke 测试：propagation API 测试（find_same_name 正确性）、DiffSession propagation 集成测试。

未完成（后续增强）：

- `find_matches_diffing()`：迭代 diff 传播（diff 已匹配对的 assembly/pseudocode，从 diff hunks 中提取函数名，尝试匹配）。
- `find_related_constants()`：通过共享常量传播匹配。
- `find_related_compilation_unit()`：通过编译单元边界推断匹配。
- `md_index` Decimal 精度校准。
- false positive checker：ratio 分布分析。
- 性能报告：heuristic 耗时、内存峰值。
- `ls` / `ls-old` 样本需要 IDA 导出后才能加入 manifest。

验收：

- 已完成：`xmake` 构建通过，`xmake run soff_smoke` 通过。
- 已完成：`soff_cli batch-diff` 和 `soff_cli parity-report` 可对 manifest 中样本运行 diff 并输出 parity 对比。
- 已完成：propagation 在 soff_pair 上显著增加 match 数量（+55 best）。
- 已完成：`soff_core` 无 IDA SDK include。

## 文件级迁移计划

### `include/soff/core`

- `version.hpp`: 已有。产品名、版本号。
- `hooks.hpp`: 已有（M9）。DiffHooks 虚基类，5 个 diff 阶段回调点。
- `config.hpp`: 待建。对应 `diaphora_config.py`。
- `options.hpp`: 待建。导出/diff 选项，替代 UI form 和 env vars。当前选项内联在 plugin 和 session 中。
- `logger.hpp`: 待建。log/log_refresh/debug_refresh 抽象。
- `progress.hpp`: 待建。waitbox/CLI progress/cancel。
- `error.hpp`: 待建。DB、IDA、Hex-Rays、diff 错误类型。

### `include/soff/db`

- `database.hpp`: 已有。SQLite runtime loader + Statement + Transaction RAII。
- `schema.hpp`: 已有。13 表 + 41 索引完整 schema。
- `repository.hpp`: 已有。全字段 save/load、incremental save、attach、program_data。
- `result_repository.hpp`: 已有。result DB 保存/加载/summarize/heuristic_stats。
- `inspect.hpp`: 待独立。当前 inspect 逻辑在 CLI main.cpp 中。

### `include/soff/analysis`

- `model.hpp`: 已有。FunctionFeature、ProgramSnapshot、BasicBlock、Instruction 完整模型。
- `graph.hpp`: 待独立。当前 graph 逻辑内联在 plugin 和 html_diff 中。
- `tarjan.hpp`: 待独立。当前 SCC/toposort 内联在 plugin exporter 中。
- `prime.hpp`: 待独立。当前 prime table 和 big-int 乘积内联在 plugin 和 ratio 中。
- `normalize.hpp`: 待独立。当前 clean asm/pseudo/microcode 内联在 plugin exporter 中。
- `hash.hpp`: 待独立。当前 bytes/function/fuzzy/KGH hash 内联在 plugin exporter 中。

### `include/soff/diff`

- `heuristics.hpp`: 已有。50 条 typed SQL heuristic descriptor。
- `sql_runner.hpp`: 已有。query execution + timeout/cancel + row limit + stats + hooks。
- `ratio.hpp`: 已有。check_ratio + text LCS + AST prime + deep ratio。
- `session.hpp`: 已有。DiffSession orchestration + hooks + propagation。
- `matcher.hpp`: 已有（空壳）。预留 in-memory matcher 接口。
- `propagation.hpp`: 已有（M10）。find_same_name + find_locally_affine_functions + run_propagation。
- `patch_diff.hpp`: 已有（M9）。patch diff vulnerability analyzer + PatchDiffHook。
- `ml_features.hpp`: 已有（M9）。ML 特征向量提取和 CSV/JSON 导出。
- `engine.hpp`: 待建。full diff workflow 独立入口（当前逻辑在 session 中）。
- `match_store.hpp`: 待独立。当前 match 分桶逻辑在 sql_runner 中。

### `include/soff/ida`

- `adapter.hpp`: 已有。IDA SDK adapter 接口。
- `exporter.hpp`: 已有（头文件）。当前导出实现内联在 plugin 中。
- `hexrays.hpp`: 待独立。当前 Hex-Rays 逻辑内联在 plugin exporter 中。
- `importer.hpp`: 待独立。当前 import 逻辑内联在 plugin 中。
- `ui.hpp`: 待独立。当前 chooser/action/waitbox 内联在 plugin 中。
- `types.hpp`: 待独立。当前 TIL/local type 逻辑内联在 plugin 中。

### `include/soff/ui`

- `actions.hpp`: 已有。action 注册接口。
- `import_plan.hpp`: 已有。ImportPlan 生成和应用。
- `html_diff.hpp`: 已有。HTML diff + graph diff + call context renderer。
- `chooser_model.hpp`: 待独立。当前 chooser 逻辑内联在 plugin 中。
- `graph_view_model.hpp`: 待独立。当前 graph diff 数据模型内联在 plugin 中。

## 关键数据字段迁移

`functions` 表必须覆盖：

- identity: `id`, `name`, `address`, `rva`, `segment_rva`, `mangled_function`, `source_file`
- graph metrics: `nodes`, `edges`, `indegree`, `outdegree`, `cyclomatic_complexity`, `strongly_connected`, `loops`
- instruction metrics: `size`, `instructions`, `mnemonics`, `mnemonics_spp`, `bytes_sum`
- hashes: `bytes_hash`, `function_hash`, `pseudocode_hash1/2/3`, `pseudocode_primes`, `md_index`, `kgh_hash`
- text: `assembly`, `clean_assembly`, `pseudocode`, `clean_pseudo`, `microcode`, `clean_microcode`
- type/comment: `prototype`, `prototype2`, `comment`, `function_flags`
- serialized arrays: `names`, `constants`, `switches`, `assembly_addrs`, `tarjan_topological_sort`, `strongly_connected_spp`, `microcode_spp`
- timing/userdata: `userdata`, `export_time`

关联表必须覆盖：

- `instructions`: native/microcode instruction rows, comments, operands, pseudocode comments。
- `basic_blocks`: native/microcode block nodes。
- `bb_relations`: CFG edges。
- `bb_instructions`: block-instruction mapping。
- `function_bblocks`: function-block mapping。
- `callgraph`: caller/callee entries。
- `constants`: normalized constants for SQL heuristics。
- `program_data`: TIL/types。
- `compilation_units`, `compilation_unit_functions`。

## 迁移风险

- `difflib.SequenceMatcher` parity：C++ 替代算法可能改变 ratio 分布。
- big integer/decimal：prime products、callgraph primes、MD index 不能用普通整数草率实现。
- IDA API 差异：当前目标是 IDA 9.3，旧版本兼容性不作为第一目标。
- Hex-Rays 可用性：decompiler/microcode 需要运行时检测并可降级。
- SQLite 锁和线程：Python 原实现每线程连接；C++ runner 需要明确 connection ownership。
- UI 导入副作用：rename/type/comment 会修改 IDB，必须可回滚或至少可预览。
- AGPL 许可：重写若直接移植算法/SQL/结构，发布和使用必须按许可证策略处理。

## 验证策略

- Unit tests：schema、heuristic descriptor、normalization、ratio、SCC、repository。
- Fixture DB tests：用 Python Diaphora 导出的 SQLite 做只读 diff。
- IDA integration tests：使用 `tester/samples/ls` 和 `ls-old`。
- Result parity：比较 best/partial/unreliable/multimatch/unmatched 数量和 top matches。
- Performance：记录导出耗时、每条 heuristic 耗时、DB row count、内存峰值。
- Regression artifacts：每次重要迁移保存 `inspect-db` JSON 和 `diff-db` summary。

## 当前下一步

M0–M10 已全部完成。以下基于深度源码审计制定后续完善计划。

### 深度审计发现的核心差距

**P0 — 直接影响 match 数量和正确性：**

1. **check_match() 完整验证**：Python 在接受每个 match 前做 4 步验证（reject nullsub、reject 已有 1.0 match、reject 已有更好 match、hook）。SOFF 仅调用 hook，缺少前 3 步。这可能导致 SOFF 产出 false positive。
2. **cleanup_matches() 迭代清理**：Python 在每个 heuristic category 后和 propagation 循环中反复调用 cleanup，按 ratio 降序排序、pair 去重、1:1 强制（保留最高 ratio）、重建 tracking。SOFF 仅有 inline 去重。
3. **find_matches_diffing() 迭代 diff 传播**：diff 已匹配对的 asm/pseudo，从 diff hunks 提取函数名，尝试匹配新函数。Python 最多 3 轮迭代。
4. **Bonus ratio (+0.01)**：Python 在 same-name 和 diffing 传播时对 ratio < 1.0 的 match 加 0.01 bonus。
5. **find_same_name() 完善**：当前 SOFF 版本缺少 relaxed_ratio + md_index 判定、nullsub 过滤。

**P1 — 影响 match 质量和覆盖率：**

6. **find_related_constants()**：从已匹配对提取共享常量，查找引用相同常量的其他函数。
7. **find_related_compilation_unit()**：推断 CU 边界，在同 CU 内暴力匹配。
8. **Multimatch 解析**：Python 收集所有 match → 按 address 分组 → 保留 max ratio → 其余移入 multimatch chooser。SOFF 标记但不解析。
9. **find_equal_matches() 独立 INTERSECT pass**：Python 用 7 字段 INTERSECT 快速找到 100% 相同函数。

**P2 — 影响特定场景：**

10. **编译单元导出**：Python 用 LFA + IDAMagicStrings 检测 CU，SOFF 未实现。
11. **字符串常量提取**：Python 从 data ref 提取字符串常量，SOFF 仅提取 o_imm。
12. **Stripped binary / patch diff 快速路径**：99% 地址匹配或 90% 名称匹配时的快速路径。
13. **Brute force unreliable**：MD-Index + KOKA hash 暴力匹配。
14. **TIL/结构体导入导出**。

**P3 — 影响可配置性和 UX：**

15. **完整配置系统**：Python 60+ 常量，SOFF 仅 ~8 个。
16. **SQLite WAL mode / pragma**。
17. **Multi-threaded heuristics**。
18. **ML 模型推理**。

### 后续里程碑规划

| 里程碑 | 核心交付 | 优先级 |
| --- | --- | --- |
| M11: Match 验证和清理 | check_match 完整验证、cleanup_matches 迭代清理、bonus ratio、multimatch 解析 | P0 |
| M12: 高级 Propagation | find_matches_diffing、find_related_constants、find_related_compilation_unit | P0 |
| M13: 导出补全 | 编译单元检测导出、字符串常量提取、callgraph primes、find_equal_matches INTERSECT | P1 |
| M14: 快速路径和配置 | stripped binary/patch diff speedup、完整配置系统、SQLite pragma | P2 |
| M15: 导入补全和重构 | TIL/结构体导入导出、插件模块化拆分 | P2 |

### 立即执行队列

1. **M13-A**：编译单元导出（LFA/codecut 检测，写入 compilation_units/compilation_unit_functions 表）。
2. **M13-B**：字符串常量提取（从 data ref 提取字符串常量，补全 constants 表）。
3. **M13-C**：`find_equal_matches()` 独立 INTERSECT pass。
4. **M14-A**：完整配置系统（`config.hpp`，60+ 常量，CLI 可覆盖）。
5. **M14-B**：stripped binary / patch diff 快速路径。
6. **M15-A**：TIL/结构体导入导出。
7. **M13-A**：编译单元导出（LFA/codecut）。
8. **M13-B**：字符串常量提取（data ref）。

## 深度源码审计补充

本节把 `diaphora.py`、`diaphora_ida.py`、`diaphora_heuristics.py`、`db_support/schema.py`、`diaphora_config.py` 的实际代码入口映射到 C++ 重写工程，作为后续提交的任务边界。迁移时按“数据格式兼容、行为可回归、UI 可替换”的顺序推进。

### `diaphora.py::CBinDiff` 方法簇迁移矩阵

| Python 方法簇 | 代表方法 | C++ 目标模块 | 迁移说明 |
| --- | --- | --- | --- |
| DB 生命周期 | `open_db`, `get_db`, `db_cursor`, `db_close`, `create_schema`, `create_indices`, `attach_database`, `try_attach` | `src/db/database.cpp`, `src/db/repository.cpp`, `src/db/attached_database.cpp` | 用 RAII connection/statement/transaction 替代裸 cursor，保留 attach secondary DB 行为。 |
| 导出写库 | `add_program_data`, `get_bb_id`, `save_instructions_to_database`, `insert_basic_blocks_to_database`, `save_microcode_instructions`, `save_function_to_database`, `save_function` | `src/db/repository.cpp`, `src/export/snapshot_writer.cpp` | repository 只写 SQLite，exporter 负责收集 IDA 数据。 |
| 文本规整 | `prettify_asm`, `re_sub`, `get_cmp_asm_lines`, `get_cmp_pseudo_lines`, `get_cmp_asm` | `src/analysis/normalize.cpp`, `src/ui/html_diff.cpp` | 规整逻辑必须单测覆盖；HTML 生成不能依赖 IDA。 |
| 图比较 | `compare_graphs_pass`, `compare_graphs`, `get_graph`, `get_callgraph_difference`, `check_callgraph` | `src/analysis/graph.cpp`, `src/diff/graph_ratio.cpp` | CFG/callgraph 建模为纯 C++ value object，big-int primes 延后但接口先稳定。 |
| match store | `add_match`, `has_best_match`, `has_better_match`, `cleanup_matches`, `count_different_matches`, `get_total_matches_for`, `show_summary` | `src/diff/match_store.cpp` | best/partial/unreliable/multimatch/unmatched 统一由 match store 管理。 |
| heuristic runner | `find_equal_matches`, `run_heuristics_for_category`, `continue_getting_sql_rows`, `add_matches_internal`, `add_matches_from_query*`, `find_partial_matches`, `find_unreliable_matches`, `find_experimental_matches` | `src/diff/heuristics.cpp`, `src/diff/sql_runner.cpp`, `src/diff/engine.cpp` | SQL descriptor 与执行分离；runner 负责 postfix、timeout、row cap、stats。 |
| ratio | `quick_ratio`, `real_quick_ratio`, `ast_ratio`, `check_ratio`, `deep_ratio`, `get_model_ratio` | `src/diff/ratio.cpp`, `src/analysis/prime.cpp`, `src/diff/ml_adapter.cpp` | M3 先实现可用 ratio，M4/M5 追求 Python parity。 |
| dirty speedups | `search_just_stripped_binaries`, `search_patchdiff_with_symbols`, `apply_dirty_heuristics` | `src/diff/speedups.cpp` | 只在高置信前置条件成立时启用，默认记录审计日志。 |
| propagation | `search_remaining_functions`, `find_remaining_functions`, `find_one_match_diffing`, `find_matches_diffing*`, `find_functions_between`, `find_locally_affine_functions`, `find_related_constants`, `find_related_compilation_unit`, `find_related_matches`, `get_callers_callees` | `src/diff/propagation.cpp`, `src/diff/local_affinity.cpp` | 是 Python 版结果质量的关键，必须在 SQL heuristic 完整后迁移。 |
| result persistence | `create_choosers`, `save_results`, `itemize_for_chooser`, `add_multimatches_to_chooser`, `add_final_chooser_items`, `final_pass`, `get_sorted_results`, `get_total_matched_functions` | `src/db/result_repository.cpp`, `src/ui/chooser_model.cpp` | result DB 格式需先与 Python 读写兼容。 |
| top-level diff | `same_processor_both_databases`, `functions_exists`, `equal_db`, `diff` | `src/diff/engine.cpp` | `DiffEngine::run()` 是 CLI 和 IDA UI 共用入口。 |
| hook/automation | `load_hooks`, `call_hook`, `call_on_match_hook`, `imp_load_source` | `src/core/hooks.cpp`, `src/plugin/script_bridge.cpp` | M9 前先定义 C++ hook interface，脚本桥接后置。 |

### `diaphora_ida.py::CIDABinDiff` 方法簇迁移矩阵

| Python 方法簇 | 代表方法 | C++ 目标模块 | 迁移说明 |
| --- | --- | --- | --- |
| IDA plugin/UI 生命周期 | `show_choosers`, `register_menu_action`, `register_menu`, `_diff_or_export`, `main` | `src/plugin/soff_plugin.cpp`, `src/ida/actions.cpp` | plugin 只注册 action；业务入口落到 application service。 |
| export orchestration | `get_last_crash_func`, `recalculate_primes`, `commit_and_start_transaction`, `do_export`, `export`, `read_function` | `src/export/exporter.cpp`, `src/ida/exporter.cpp` | 支持 crash/resume、批量 commit、waitbox cancel、batch mode。 |
| function feature extraction | `initialize_function_data`, `extract_all_features`, `extract_function_*`, `process_basic_block`, `process_instruction`, `build_topological_relations`, `get_decoded_instruction` | `src/ida/function_reader.cpp`, `src/analysis/function_features.cpp` | 先导出 native CFG/instructions/callgraph/constants，再导出 Hex-Rays 高级字段。 |
| Hex-Rays/pseudocode/microcode | `do_decompile`, `decompile_and_get`, `get_microcode`, `get_microcode_bblocks`, `get_microcode_instructions`, `get_plain_microcode_line`, `extract_microcode`, `CAstVisitor` | `src/ida/hexrays.cpp`, `src/analysis/microcode.cpp` | 运行时检测 Hex-Rays；不可用时字段为空但导出成功。 |
| import definitions | `import_til`, `import_definitions`, `import_definitions_only`, `GetOrdinalCount`, `GetLocalType`, `export_structures`, `export_til`, `get_til_names` | `src/ida/types.cpp`, `src/ida/importer.cpp` | 本地类型导入必须可选择、可预览、失败可继续。 |
| result import | `import_one`, `import_instruction`, `import_instruction_level`, `do_import_one`, `import_selected`, `import_items`, `do_import_all`, `import_all`, `import_all_auto`, `row_is_importable`, `update_choosers` | `src/ida/importer.cpp`, `src/ui/import_plan.cpp` | 先构建 `ImportPlan`，再应用到 IDB，减少 UI 与副作用耦合。 |
| visual diff | `generate_asm_diff*`, `generate_pseudo_diff`, `generate_microcode_diff`, `show_*_diff`, `save_*_diff`, `CHtmlDiff`, `CHtmlViewer` | `src/ui/html_diff.cpp`, `src/ida/html_view.cpp` | HTML diff 纯生成，IDA 只负责显示窗口。 |
| graph/callgraph UI | `show_graph_pair`, `graph_diff_internal`, `graph_diff`, `graph_diff_microcode`, `get_calls_graph`, `build_calls_graph`, `show_callgraph_context` | `src/ui/graph_view_model.cpp`, `src/ida/graph_view.cpp` | graph data 与 IDA GraphViewer 解耦。 |
| external tools | `diff_external`, `run_external_diff_tool`, `diff_external_asm`, `diff_external_pseudo` | 暂不迁移 | 用户工作流优先使用 IDA 内置文本/图形 diff；外部工具入口已从 M8 UI 移除。 |
| result load/re-diff | `populate_choosers`, `load_and_import_all_results`, `load_results`, `re_diff`, `equal_db` | `src/db/result_repository.cpp`, `src/diff/engine.cpp` | 支持加载旧 `.diaphora` 结果和重新 diff。 |

### SQLite schema 精确迁移清单

Python `db_support/schema.py` 当前定义 13 张导出表、41 条显式索引。C++ schema 必须逐项匹配；只有 result DB 可以额外增加 `soff_*` 元数据表。

| 表 | 列数 | 迁移状态 | 备注 |
| --- | ---: | --- | --- |
| `functions` | 49 | 已建表，需补全读写 | 核心表，所有 heuristic 依赖它。 |
| `program` | 5 | 已建表，需补 inspect | processor/md5/callgraph primes。 |
| `program_data` | 4 | 已建表，需补 types/TIL 导入导出 | struct/enum/union/TIL。 |
| `version` | 1 | 已建表，需固定兼容策略 | 读写 `3.4` baseline。 |
| `instructions` | 13 | 已建表，需补全写入 | native 和 microcode 指令共用。 |
| `basic_blocks` | 4 | 已建表，需补全写入 | `asm_type` 区分 native/microcode。 |
| `bb_relations` | 3 | 已建表，需补全写入 | CFG edge。 |
| `bb_instructions` | 3 | 已建表，需补全写入 | BB 到 instruction。 |
| `function_bblocks` | 4 | 已建表，需补全写入 | function 到 BB。 |
| `callgraph` | 4 | 已建表，需补全写入 | caller/callee entries。 |
| `constants` | 3 | 已建表，需补全写入 | 常量匹配依赖。 |
| `compilation_units` | 7 | 已建表，需迁移 LFA/codecut | CU heuristic 依赖。 |
| `compilation_unit_functions` | 3 | 已建表，需迁移 LFA/codecut | CU 到 function。 |

`functions` 的 49 列必须全部可 round-trip：`id`, `name`, `address`, `nodes`, `edges`, `indegree`, `outdegree`, `size`, `instructions`, `mnemonics`, `names`, `prototype`, `cyclomatic_complexity`, `primes_value`, `comment`, `mangled_function`, `bytes_hash`, `pseudocode`, `pseudocode_lines`, `pseudocode_hash1`, `pseudocode_primes`, `function_flags`, `assembly`, `prototype2`, `pseudocode_hash2`, `pseudocode_hash3`, `strongly_connected`, `loops`, `rva`, `tarjan_topological_sort`, `strongly_connected_spp`, `clean_assembly`, `clean_pseudo`, `mnemonics_spp`, `switches`, `function_hash`, `bytes_sum`, `md_index`, `constants`, `constants_count`, `segment_rva`, `assembly_addrs`, `kgh_hash`, `source_file`, `userdata`, `microcode`, `clean_microcode`, `microcode_spp`, `export_time`。

### 完整 heuristic 迁移清单

ratio mode：`0=NO_FPS`, `1=RATIO`, `2=RATIO_MAX`, `3=RATIO_MAX_TRUSTED`。flag：`3=SAME_CPU`, `2=SLOW`, `1=UNRELIABLE`。

| # | Category | Ratio | Flags | Min | Name | C++ 任务 |
| ---: | --- | ---: | --- | ---: | --- | --- |
| 1 | Best | 0 | 3 | | Same RVA and hash | M1 descriptor，M3 exact runner |
| 2 | Best | 0 | 3 | | Same order and hash | M1 descriptor，M3 exact runner |
| 3 | Best | 0 | 3 | | Function Hash | M1 descriptor，M3 exact runner |
| 4 | Best | 0 | 3 | | Bytes hash | M1 descriptor，M3 exact runner |
| 5 | Best | 1 | none | | Same address and mnemonics | M1 descriptor，M4 ratio |
| 6 | Best | 1 | 3 | | Same cleaned assembly | M1 descriptor，M4 ratio |
| 7 | Best | 1 | 3 | | Same cleaned microcode | M1 descriptor，M6 microcode |
| 8 | Best | 1 | none | | Same cleaned pseudo-code | M1 descriptor，M6 pseudocode |
| 9 | Best | 1 | none | | Same address, nodes, edges and mnemonics | M1 descriptor，M4 ratio |
| 10 | Best | 2 | 3 | 0.7 | Same RVA | M1 descriptor，M4 ratio-max |
| 11 | Best | 0 | none | | Equal assembly or pseudo-code | M1 descriptor，M3 exact runner |
| 12 | Best | 1 | none | | Microcode mnemonics small primes product | M1 descriptor，M6 microcode SPP |
| 13 | Partial | 3 | none | 0.44 | Same named compilation unit function match | M7 CU/local affinity |
| 14 | Partial | 2 | none | 0.449 | Same anonymous compilation unit function match | M7 CU/local affinity |
| 15 | Partial | 1 | 2 | | Same compilation unit | M7 CU/local affinity |
| 16 | Partial | 1 | none | | Same KOKA hash and constants | M6 KGH/constants |
| 17 | Partial | 1 | none | | Same KOKA hash and MD-Index | M6 KGH/MD index |
| 18 | Partial | 2 | none | 0.5 | Same constants | M4 constants ratio |
| 19 | Partial | 2 | none | 0.45 | Same rare KOKA hash | M6 KGH |
| 20 | Partial | 1 | none | | Same rare MD Index | M6 MD index |
| 21 | Partial | 2 | none | 0.5 | Same address and rare constant | M4 constants ratio |
| 22 | Partial | 2 | 2 | 0.2 | Same rare constant | M4 slow heuristic gate |
| 23 | Partial | 1 | none | | Same MD Index and constants | M6 MD index/constants |
| 24 | Partial | 1 | none | | Import names hash | M5 import names export |
| 25 | Partial | 1 | none | | Mnemonics and names | M4 ratio |
| 26 | Partial | 1 | none | | Pseudo-code fuzzy hash | M6 fuzzy hash |
| 27 | Partial | 2 | none | 0.579 | Similar pseudo-code and names | M6 pseudocode |
| 28 | Partial | 2 | none | 0.6 | Mnemonics small-primes-product | M4 SPP |
| 29 | Partial | 2 | none | 0.549 | Same nodes, edges, loops and strongly connected components | M4 graph ratio |
| 30 | Partial | 2 | none | 0.5 | Same low complexity, prototype and names | M4 prototype/names |
| 31 | Partial | 2 | none | 0.5 | Same low complexity and names | M4 names |
| 32 | Partial | 2 | none | 0.5 | Switch structures | M5 switches export |
| 33 | Partial | 2 | none | 0.5 | Pseudo-code fuzzy (normal) | M6 fuzzy hash |
| 34 | Partial | 1 | none | | Pseudo-code fuzzy (mixed) | M6 fuzzy hash |
| 35 | Partial | 1 | none | | Pseudo-code fuzzy (reverse) | M6 fuzzy hash |
| 36 | Partial | 2 | none | 0.35 | Pseudo-code fuzzy AST hash | M6 AST prime hash |
| 37 | Partial | 2 | 2+1 | 0.5 | Partial pseudo-code fuzzy hash (normal) | M6 slow/unreliable gate |
| 38 | Partial | 2 | 2+1 | 0.5 | Partial pseudo-code fuzzy hash (reverse) | M6 slow/unreliable gate |
| 39 | Partial | 2 | 2+1 | 0.5 | Partial pseudo-code fuzzy hash (mixed) | M6 slow/unreliable gate |
| 40 | Partial | 2 | 3 | 0.5 | Same rare assembly instruction | M5 instruction export |
| 41 | Partial | 2 | none | 0.5 | Same rare basic block mnemonics list | M5 basic block export |
| 42 | Partial | 2 | 2 | 0.49 | Loop count | M4 graph loop data |
| 43 | Unreliable | 2 | none | 0.5 | Same graph | M4 graph ratio |
| 44 | Unreliable | 2 | 2 | 0.8 | Strongly connected components | M4 SCC |
| 45 | Unreliable | 1 | 2 | | Nodes, edges, complexity and mnemonics | M4 graph/text ratio |
| 46 | Unreliable | 1 | 2 | | Nodes, edges, complexity and prototype | M4 graph/prototype ratio |
| 47 | Unreliable | 1 | 2 | | Nodes, edges, complexity, in-degree and out-degree | M4 graph ratio |
| 48 | Unreliable | 1 | 2 | | Nodes, edges and complexity | M4 graph ratio |
| 49 | Unreliable | 1 | 2 | | Same high complexity | M4 graph ratio |
| 50 | Unreliable | 1 | none | | Topological sort hash | M4 topological sort |

验收固定值：

- heuristic 总数：50。
- category：Best 12、Partial 30、Unreliable 8。
- ratio mode：NO_FPS 5、RATIO 22、RATIO_MAX 22、RATIO_MAX_TRUSTED 1。
- flags：无 flags 30、SAME_CPU 8、SLOW 9、SLOW+UNRELIABLE 3。
- `HEUR_TYPE_RATIO_MAX` 和 `HEUR_TYPE_RATIO_MAX_TRUSTED` 必须有 `min`。
- SQL 字段引用必须全部存在于 schema 或 `SELECT_FIELDS` alias。

### 配置迁移清单

| Python 配置 | C++ 目标 | 默认值策略 |
| --- | --- | --- |
| `DIFFING_ENABLE_UNRELIABLE`, `DIFFING_ENABLE_EXPERIMENTAL`, `DIFFING_ENABLE_SLOW_HEURISTICS` | `DiffOptions` | 与 Python 默认一致，CLI 可覆盖。 |
| `DIFFING_ENABLE_RELAXED_RATIO`, `DIFFING_IGNORE_*FUNCTION*` | `RatioOptions` | M4 生效。 |
| `EXPORTING_USE_DECOMPILER`, `EXPORTING_USE_MICROCODE`, `EXPORTING_FUNCTION_SUMMARIES_ONLY`, `EXPORTING_COMPILATION_UNITS` | `ExportOptions` | IDA UI 和 CLI batch 共用。 |
| `EXPORTING_EXCLUDE_LIBRARY_THUNK`, `EXPORTING_ONLY_NON_IDA_SUBS` | `ExportFilterOptions` | `should_skip_function()` 迁移验收。 |
| `SQL_MAX_PROCESSED_ROWS`, `SQL_TIMEOUT_LIMIT`, `SQL_DEFAULT_POSTFIX` | `SqlRunnerOptions` | M3 生效，stats 记录中断原因。 |
| `SQLITE_JOURNAL_MODE`, `SQLITE_PRAGMA_SYNCHRONOUS` | `DatabaseOptions` | 创建库时写入 pragma。 |
| `MATCHES_BONUS_RATIO`, `DEFAULT_PARTIAL_RATIO`, `DEFAULT_TRUSTED_PARTIAL_RATIO`, `RELATED_MATCHES_MIN_RATIO` | `MatchOptions` | M4/M7 生效。 |
| `MIN_FUNCTIONS_TO_DISABLE_SLOW`, `MIN_FUNCTIONS_TO_CONSIDER_MEDIUM`, `MIN_FUNCTIONS_TO_CONSIDER_HUGE` | `AutoTuningOptions` | 用于自动禁用 slow 和 summary-only。 |
| `RUN_DEFAULT_SCRIPTS`, `DEFAULT_SCRIPT_PATCH_DIFF` | `HookOptions` | M9 生效。 |
| `ML_USE_TRAINED_MODEL`, `ML_TRAINED_MODEL`, `ML_TRAINED_MODEL_MATCH_SCORE` | `MlOptions` | M9 先只输出特征，不内置 pickle/joblib。 |
| UI 颜色常量 | `UiTheme` | M7/M8 生效，核心不依赖。 |

### 迁移工程目录落地要求

当前实际布局（已有文件加粗）：

```text
include/soff/core/
  **version.hpp** config.hpp options.hpp logger.hpp progress.hpp hooks.hpp
include/soff/db/
  **database.hpp** **schema.hpp** **repository.hpp** **result_repository.hpp** inspect.hpp
include/soff/analysis/
  **model.hpp** graph.hpp tarjan.hpp prime.hpp normalize.hpp hash.hpp microcode.hpp
include/soff/export/
  **exporter.hpp** snapshot_writer.hpp resume.hpp
include/soff/diff/
  **heuristics.hpp** **sql_runner.hpp** **ratio.hpp** **session.hpp** **matcher.hpp** engine.hpp match_store.hpp propagation.hpp speedups.hpp
include/soff/ida/
  **adapter.hpp** exporter.hpp function_reader.hpp hexrays.hpp importer.hpp types.hpp **actions.hpp**
include/soff/ui/
  **actions.hpp** **html_diff.hpp** **import_plan.hpp** chooser_model.hpp graph_view_model.hpp
src/... mirrors include/...
  plugin/soff_plugin.cpp (6367 lines, contains IDA-side export/import/chooser/graph logic)
  cli/main.cpp (init-db/inspect-db/diff-db/check-m5-fixture)
tests/
  smoke.cpp (613 lines)
  fixtures/ (ratio_parity.json, m4/m5 calibration JSONs)
```

注意：大量逻辑当前内联在 `soff_plugin.cpp` 中（导出、import、chooser、graph diff），后续重构可拆出独立 `.cpp` 到 `src/ida/` 和 `src/ui/`，但不是功能阻塞项。

禁止把 IDA SDK header 泄漏到 `include/soff/core`, `include/soff/db`, `include/soff/diff`, `include/soff/analysis`。这些模块必须能在没有 IDA 的机器上构建和测试。

### 工程验收定义

每个 milestone 的提交必须满足：

- `xmake` 通过。
- `xmake run soff_smoke` 通过。
- 新增行为有至少一个 CLI 或 unit test 覆盖。
- `soff_core` 无 IDA SDK include。
- DB schema 变更能被 `soff_cli inspect-db` 观测。
- 对 Diaphora 旧 DB 的读操作不破坏原文件。

最终迁移完成的验收：

- 能在 IDA 9.3 中导出当前 IDB 为 Diaphora-compatible SQLite。
- 能用 CLI 对两个旧 Diaphora 导出库执行 diff 并保存结果。
- 能在 IDA 9.3 中加载 C++ 结果库，显示 chooser，并导入名称、类型、注释。
- `tester/samples/ls` 与 `ls-old` 的导出表计数、索引、best/partial/unreliable/multimatch 摘要可与 Python baseline 对比。
- 在无 Hex-Rays 环境中能降级运行；在有 Hex-Rays 环境中 pseudocode/microcode 字段非空并参与 heuristic。

### 立即执行队列

1. **M13-A**：编译单元导出（LFA/codecut 检测，写入 compilation_units/compilation_unit_functions 表）。
2. **M13-B**：字符串常量提取（从 data ref 提取字符串常量，补全 constants 表）。
3. **M13-C**：`find_equal_matches()` 独立 INTERSECT pass。
4. **M14-A**：完整配置系统（`config.hpp`，60+ 常量，CLI 可覆盖）。
5. **M14-B**：stripped binary / patch diff 快速路径。
6. **M15-A**：TIL/结构体导入导出。
