# SOFF 发布验收报告

- 验收日期：2026-08-06
- 工作区：`G:\DM\cyber\soff`
- 结论：**暂缓发布（HOLD，阻塞范围已缩小）**。本地可审计真实导出 fixture、Windows 构建、原生 smoke、保存故障注入、Rust/前端质量门禁均通过；官方历史 M5 parity 数据与 Linux/macOS/ARM64 hosted runner 仍未完成，因此暂不能宣称最终跨平台发布验收通过。

## 1. 插件单体拆分

状态：**通过**。

`src/plugin/soff_plugin.cpp` 已从约 7,000 行单体缩减为约 100 行入口文件，保留原有匿名命名空间、IDA callback ABI、初始化顺序与 `PLUGIN` 导出。实现拆分在 `src/plugin/modules/`：

- `common.inc`：公共常量、数据结构、校验与工具函数
- `settings.inc`：设置持久化、环境覆盖、IDA 选项对话框
- `export_helpers.inc`：导出辅助、crash marker、JSON/hash/feature 基础函数
- `hexrays.inc`：Hex-Rays 初始化、AST/pseudocode 支持与失败统计
- `microcode.inc`：microcode tokenization 与特征提取
- `export.inc`：函数/CFG 特征读取与 SQLite 导出
- `result_ui.inc`：结果 chooser/panel
- `graph_ui.inc`：文本、native graph、microcode graph、call-context UI
- `result_ui_actions.inc`：结果加载/另存/查看流程
- `import.inc`：将结果元数据导回当前 IDB
- `actions.inc`：export/diff/local-diff、graph action、IDC/auto mode
- `entry.inc`：`plugmod_t` 生命周期、action 注册和初始化

由于 IDA SDK 回调大量依赖共享的匿名命名空间状态，这一版采用 `.inc` compile-time module，而不是改变 ABI 的多个 DLL/object target。模块边界已记录在 `src/plugin/modules/README.md`。

验收命令：

```powershell
xmake build -y soff_ida
```

结果：`soff.dll` 构建成功。

## 2. 真实 fixture 匹配校准

### 2.1 官方 M5 fixture

状态：**阻塞，未通过**。

已实际执行：

```powershell
build\windows\x64\release\soff_cli.exe check-m5-fixture `
  tests\fixtures\m5_native_fields_reexport_calibration.json `
  --out build\m5_native_recalibrated.soff
```

fixture 中的数据库路径为：

- `G:\DM\diaphora-cpp\test\soff_pair_a.sqlite`
- `G:\DM\diaphora-cpp\test\soff_pair_b.sqlite`

这两个外部 fixture 在当前工作区均不存在，因此校准工具明确报告 primary/secondary database missing。`tests/fixtures/m5_two_file_diff_calibration.json`、`m4_real_export_calibration.json` 也引用同一外部数据源，不能用当前本地文件冒充官方 parity fixture。

为避免再次修改 fixture JSON，CLI 现在支持显式指定外部 fixture 根目录：

```powershell
soff_cli check-m5-fixture tests\fixtures\m5_native_fields_reexport_calibration.json `
  --root G:\DM\diaphora-cpp `
  --out build\m5_native_recalibrated.soff
```

`--root` 会尝试 `<root>/<filename>`、`<root>/test/<filename>` 和 `<root>/tests/<filename>`，适配当前历史 fixture 的 `G:\DM\diaphora-cpp\test\...` 布局。

### 2.2 当前工作区可用的真实导出替代样本

为验证算法链路仍可运行，使用本地已有的 M6 导出：

```powershell
build\windows\x64\release\soff_cli.exe diff-db `
  build\m6_pair_a_nullfix.sqlite `
  build\m6_pair_b_nullfix.sqlite `
  --out build\m6_pair_ab_recalibrated.soff `
  --unreliable --max-rows 200000 --timeout 120
```

观测结果：

| 指标 | 结果 |
|---|---:|
| best | 87 |
| partial | 6 |
| unreliable | 0 |
| multimatch | 0 |
| unmatched primary | 2 |
| unmatched secondary | 2 |
| row-limited | 0 |
| timed out | 0 |
| cancelled | 0 |

未匹配函数为：

- primary：`item_score`, `clamp_score`
- secondary：`item_score`, `clamp_score_v2`

best 样本包含 `WinMainCRTStartup`、`__tmainCRTStartup`、`mainCRTStartup`、`__dyn_tls_init` 等 runtime/CRT 函数，符合该样本的 runtime 骨架特征。该结果可作为本地 smoke/calibration evidence，但**不替代**缺失的官方 M5 fixture。

本地 fixture 已固定进仓库：

```text
tests/fixtures/local_m6_nullfix_recalibration.json
tests/fixtures/data/m6_pair_a_nullfix.sqlite
tests/fixtures/data/m6_pair_b_nullfix.sqlite
```

数据库总大小约 2.21 MiB，fixture 校准已加入 `.github/workflows/ci.yml` 和 `.github/workflows/release.yml`。可复现命令：

```powershell
xmake run soff_cli check-m5-fixture `
  tests/fixtures/local_m6_nullfix_recalibration.json `
  --out build/local_m6_nullfix_recalibration.soff
```

结果：`fixture=ok`，best/partial/multimatch/unmatched 为 `87/6/0/2/2`。这解决了“每次 CI 都能运行一组真实导出回归”的问题，但该 fixture 仍不是历史 Python Diaphora M5 parity 基线。

### 2.3 runtime / memory 采样

对同一 M6 diff 命令进行 Windows 进程采样：

- wall time：约 `1037 ms`
- peak working set：约 `8.33 MiB`
- exit code：`0`

该采样是单次本地观测，不是跨平台性能基线，也不应作为发布 SLA。

## 3. 保存故障注入

状态：**通过**。

`tests/test_repository.cpp` 新增故障注入场景：把目标路径预置为目录，使 atomic replacement 在 sidecar preservation/replacement 阶段失败，并验证：

- `commit()` 抛出异常；
- 原目标目录仍存在；
- 原 `-wal` sidecar 仍存在且内容不变；
- 临时输出在 writer 析构后被清理。

执行：

```powershell
xmake build -y soff_smoke
xmake run soff_smoke
```

结果：**exit code 0**，输出包含 `repository: save fault-injection regression passed`。

## 4. 本地 CI 等价检查

状态：**Windows runner 等价检查通过**。

已通过：

```powershell
xmake build -y -a
xmake build -y soff_ida
xmake run soff_smoke
bun install --frozen-lockfile                 # desktop/
cargo fmt --manifest-path desktop/src-tauri/Cargo.toml -- --check
cargo clippy --manifest-path desktop/src-tauri/Cargo.toml --all-targets -- -D warnings
cargo test --manifest-path desktop/src-tauri/Cargo.toml
bun run build                                  # desktop/
```

产物存在并成功链接：`soff.dll`、`soff_cli.exe`、`soff_ffi.dll`。

尚未实跑：

- Ubuntu x64
- Ubuntu ARM64
- macOS ARM64
- GitHub Actions hosted runner 的完整 bundle/package 流程

当前环境没有可用的 WSL/`act` 本地 runner；`gh auth status` 也显示当前 GitHub token 已失效，因此本次没有伪造远程 CI 结果，也没有触发外部 workflow。

## 5. 发布判定与解锁条件

当前判定：**HOLD，不发布**。本地回归 gate 已解除；剩余是外部数据和 hosted runner gate。

解除 HOLD 需要：

1. 将官方 M5 fixture 数据库放回 `G:\DM\diaphora-cpp\test\`，或更新 fixture 指向可审计的共享 artifact；
2. 使用 `--root` 重新执行 `check-m5-fixture`，确认 best/partial/multimatch/unmatched 与校准基线；
3. 在 GitHub Actions 上成功完成 Linux x64、Linux ARM64、macOS ARM64、Windows x64 四个矩阵 job，并验证 installer artifacts；
4. 若正式发布要求 runtime/memory SLA，再补充固定硬件、固定输入、重复多次的性能基线。
