# soff codemap

Diaphora C++ 重写。保留功能语义、SQLite schema 兼容性和工作流。

## 工程现状

M0–M15 + P0–P3 全部完成。50 条 SQL heuristic、全字段导出、deep_ratio、propagation（含 CU）、chooser/import/graph diff、DIAPHORA_AUTO 无头模式、导出 hooks 均已实现。

代码规模：`soff_plugin.cpp` ~6800 行，`smoke.cpp` 730+ 行，核心库 20 个 .cpp + 20 个 .hpp。

## 架构

```text
src/plugin/     IDA plugmod_t, actions, IDC functions, DIAPHORA_AUTO
src/core/       version, error, hooks (DiffHooks + ExportHooks), thread_pool
src/db/         SQLite runtime, schema, repository, result_repository
src/diff/       heuristics, sql_runner, ratio, session, propagation, patch_diff, ml_features
src/ui/         line_diff, html_diff, import_plan
src/cli/        inspect-db, init-db, diff-db, batch-diff, parity-report, ml-export, patch-diff
tests/          smoke.cpp + fixtures/
```

约束：`soff_core` 不含 IDA SDK include。IDA SDK 只在 `src/plugin/soff_plugin.cpp`。

## SQLite schema

`functions` 表 49 列，`program` 表含 `callgraph_primes` / `callgraph_all_primes`。
关联表：instructions, basic_blocks, bb_relations, bb_instructions, function_bblocks, callgraph, constants, program_data, compilation_units, compilation_unit_functions, version。

## 验收定义

- `xmake` 通过
- `xmake run soff_smoke` 通过
- `soff_core` 无 IDA SDK include
- 对 Diaphora 旧 DB 的读操作不破坏原文件

## soff 独有功能（超越 Diaphora）

| 功能 | 说明 |
|------|------|
| Microcode 导出 | MMAT_GENERATED IR + "Same cleaned microcode" heuristic |
| ML 特征向量 | extract_ml_features() CSV/JSON + 决策树/随机森林推理过滤 |
| Patch Diff 分析器 | signed/unsigned、unsafe function、size check |
| 原生 IDA GraphViewer 双窗 | graph_sync_peer / graph_text_diff |
| 崩溃恢复 | 批量保存 + 崩溃标记 + 断点续传 |
| 并行 ratio 计算 | 线程池 |
| Boost flat_map/set | 高性能哈希容器 |
| IDC/MCP 桥接 | soff_export/soff_diff/soff_diff_asm/soff_diff_pseudo |
| DIAPHORA_AUTO | 无头导出+diff，`qexit(0)` |
| ExportHooks | before/after_export_function, on_export_crash |

---

## 深度差异审计（2026-05-22 第二轮）

### Critical（严重影响匹配质量）

| # | 差异 | Python | soff | 影响 |
|---|------|--------|------|------|
| 1 | **常量过滤缺失** | 过滤 `<0x1000`、2的幂、`0xFFFFFF00` 掩码、已有 data ref 的立即数 | 所有 `o_imm` 直接存入 | constants 字段充满噪声，影响 deep_ratio 和 constants 启发式 |
| 2 | **v3 (AST ratio) 硬编码 0** | relaxed 模式下计算 `ast_ratio(primes1, primes2)` | `v3 = 0.0` | relaxed 模式丢失关键比较维度，v4 md_index boost 公式受影响 |
| 3 | **find_matches_diffing 算法降级** | unified_diff 对已匹配函数的 asm/pseudo 做 diff，从 hunks 用正则提取函数名对（发现重命名） | 仅比较 `names` JSON 字段找相同名称 | 无法发现 A 调用 foo() 而 B 调用 bar() 的重命名关系 |

### Medium（中等影响）

| # | 差异 | 说明 |
|---|------|------|
| 4 | **deep_ratio bonus 值 10x 偏大** | Python: +0.001/+0.003；soff: +0.01/+0.05。需回退到 Python 原值 |
| 5 | **deep_ratio 常量评分方式** | Python: 每个共同常量 +0.006/0.008 累加；soff: >50% 重叠则固定 +0.05 |
| 6 | **mangled_function 匹配缺失** | find_same_name 仅用 `name`，Python 还用 `mangled_function` |
| 7 | **find_same_name 独立初始传播** | Python 在 SQL heuristics 之前先跑 find_same_name；soff 仅在 propagation 阶段 |
| 8 | **relaxed_ratio 模式未实现** | 配置项存在但 compute_ratio 中未使用 |
| 9 | **search_patchdiff_with_symbols** | >90% 同名时的 patch diff 加速完全缺失 |
| 10 | **find_matches_diffing 节点数过滤** | Python 检查 min/max nodes < 25% 和 nodes < 3；soff 无 |
| 11 | **全局变量重命名/类型导入** | 导入时不处理引用的全局变量 |

### Low（低影响）

| # | 差异 |
|---|------|
| 12 | auto-tuning 未自动应用（函数数 > 4001 禁用 slow） |
| 13 | ML 模型推理缺失 |
| 14 | 导入后不更新主数据库 |
| 15 | find_related_constants 始终运行（Python 仅 slow 模式） |

---

## 修复路线图 v2

| 优先级 | 任务 | 状态 |
|--------|------|------|
| **P0** | 常量过滤：`<0x1000`、2的幂、掩码、data ref 排除 | ✅ |
| **P0** | deep_ratio bonus 回退到 Python 原值（+0.001/+0.003/+0.006） | ✅ |
| **P1** | 实现 relaxed_ratio 模式（v3 AST ratio + md_index 快速接受） | ✅ |
| **P1** | find_matches_diffing 改为 diff-based rename detection + 节点数过滤 | ✅ |
| **P2** | find_same_name 加 mangled_function 匹配 | ✅ |
| **P2** | find_same_name 独立初始传播（SQL heuristics 之前） | ✅ |
| **P2** | find_matches_diffing 节点数过滤 | ✅（含在 P1 中） |
| **P2** | search_patchdiff_with_symbols 加速 | ✅ |
| **P3** | 全局变量重命名/类型导入 | ✅ |
| **P3** | auto-tuning 自动应用 | ✅ |
