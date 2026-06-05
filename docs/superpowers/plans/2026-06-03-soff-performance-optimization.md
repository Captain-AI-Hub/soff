# Soff Performance Optimization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Soff feel lightweight when installed in IDA, then speed up export and diff without losing existing Diaphora-compatible output or match quality.

**Architecture:** Add low-overhead telemetry first, then use it to guide staged optimization. Keep startup lazy, make export write paths batch/prepared-statement based, and make diff/propagation reuse cached function data instead of repeated point SQL queries.

**Tech Stack:** C++17, xmake, IDA SDK, Hex-Rays SDK, SQLite loaded dynamically, Boost header-only containers, existing `soff_smoke` test binary.

---

## Files and Responsibilities

- Create `include/soff/core/perf.hpp`: disabled-by-default scoped timers and counters.
- Create `src/core/perf.cpp`: environment parsing, timing aggregation, and report rendering.
- Modify `xmake.lua`: include new core source automatically via existing `src/core/*.cpp` glob; no target changes expected.
- Modify `include/soff/db/database.hpp`: add `Statement::clear_bindings()` if reusable export inserts require it.
- Modify `src/db/database.cpp`: count SQL statements under `SOFF_PERF=1`; avoid extra work when disabled.
- Modify `src/db/repository.cpp`: apply export pragmas, reuse statements, batch high-volume inserts.
- Modify `src/db/schema.cpp`: add covering indexes used by diff and propagation.
- Modify `include/soff/diff/sql_runner.hpp`: pass optional cache and telemetry-aware options through the SQL runner.
- Create `include/soff/diff/function_cache.hpp`: cached function/constant/callgraph/BB lookup types.
- Create `src/diff/function_cache.cpp`: load cache from attached main/diff databases.
- Modify `src/diff/sql_runner.cpp`: instrument heuristic phases and replace `deep_ratio_bonus()` point queries with cache lookup.
- Modify `src/diff/session.cpp`: build caches, reduce full fixed-point reruns, cache BB matching results.
- Modify `src/diff/propagation.cpp`: convert N+1 propagation lookups to cache or batch access.
- Modify `src/diff/bb_matching.cpp`: support cached basic-block loads or memoization hooks.
- Modify `src/plugin/soff_plugin.cpp`: instrument plugin/export, make UI snapshot/result reloads cached, reduce export hot-loop allocation.
- Modify `tests/smoke.cpp`: add regression checks for telemetry disabled behavior, schema indexes, function cache, and diff stability.
- Reference spec: `docs/superpowers/specs/2026-06-03-soff-performance-optimization-design.md`.

## Validation Commands

- Build: `xmake build -y`
- Smoke test: `build\windows\x64\release\soff_smoke.exe`
- CLI fixture diff, when available: `build\windows\x64\release\soff_cli.exe diff ..\test\soff_test_a.sqlite ..\test\soff_test_b.sqlite -o build\soff_perf_check.soff`
- IDA plugin build: `xmake config --ida_plugin=y -y && xmake build soff_ida -y`

Do not commit automatically. If the user asks for commits, use the commit checkpoints listed in each task.

---

### Task 1: Add Low-Overhead Performance Telemetry

**Files:**
- Create: `include/soff/core/perf.hpp`
- Create: `src/core/perf.cpp`
- Modify: `tests/smoke.cpp`

- [ ] **Step 1: Add failing smoke checks for disabled telemetry**

Add near the existing early smoke assertions in `tests/smoke.cpp`:

```cpp
#include "soff/core/perf.hpp"
```

Add after the heuristic validation block:

```cpp
    assert(!soff::perf::enabled());
    {
        soff::perf::ScopedTimer timer("test.disabled_timer");
        soff::perf::add_counter("test.disabled_counter", 1);
    }
    const auto disabled_report = soff::perf::render_report();
    assert(disabled_report.empty());
```

- [ ] **Step 2: Run smoke to verify it fails**

Run: `xmake build soff_smoke -y`

Expected: compile failure because `soff/core/perf.hpp` does not exist.

- [ ] **Step 3: Create telemetry header**

Create `include/soff/core/perf.hpp`:

```cpp
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace soff::perf {

bool enabled();
void reset();
void add_counter(std::string_view name, std::uint64_t value = 1);
void add_duration_ns(std::string_view name, std::uint64_t nanoseconds);
std::string render_report();

class ScopedTimer {
public:
    explicit ScopedTimer(std::string_view name);
    ~ScopedTimer();

    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

private:
    std::string name_;
    std::uint64_t start_ns_ = 0;
    bool active_ = false;
};

} // namespace soff::perf
```

- [ ] **Step 4: Implement telemetry source**

Create `src/core/perf.cpp`:

```cpp
#include "soff/core/perf.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace soff::perf {
namespace {

struct Metric {
    std::uint64_t count = 0;
    std::uint64_t total_ns = 0;
};

std::mutex& metrics_mutex()
{
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<std::string, Metric>& metrics()
{
    static std::unordered_map<std::string, Metric> values;
    return values;
}

std::uint64_t now_ns()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

bool env_enabled()
{
    const char* value = std::getenv("SOFF_PERF");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

} // namespace

bool enabled()
{
    static const bool value = env_enabled();
    return value;
}

void reset()
{
    if (!enabled()) return;
    std::lock_guard<std::mutex> lock(metrics_mutex());
    metrics().clear();
}

void add_counter(std::string_view name, std::uint64_t value)
{
    if (!enabled()) return;
    std::lock_guard<std::mutex> lock(metrics_mutex());
    auto& metric = metrics()[std::string(name)];
    metric.count += value;
}

void add_duration_ns(std::string_view name, std::uint64_t nanoseconds)
{
    if (!enabled()) return;
    std::lock_guard<std::mutex> lock(metrics_mutex());
    auto& metric = metrics()[std::string(name)];
    ++metric.count;
    metric.total_ns += nanoseconds;
}

std::string render_report()
{
    if (!enabled()) return {};
    std::vector<std::pair<std::string, Metric>> rows;
    {
        std::lock_guard<std::mutex> lock(metrics_mutex());
        rows.assign(metrics().begin(), metrics().end());
    }
    std::sort(rows.begin(), rows.end(), [](const auto& left, const auto& right) {
        return left.first < right.first;
    });
    std::ostringstream out;
    for (const auto& [name, metric] : rows) {
        out << name << " count=" << metric.count;
        if (metric.total_ns != 0) {
            out << " total_ms=" << (static_cast<double>(metric.total_ns) / 1000000.0);
        }
        out << '\n';
    }
    return out.str();
}

ScopedTimer::ScopedTimer(std::string_view name)
    : name_(name)
    , start_ns_(enabled() ? now_ns() : 0)
    , active_(enabled())
{
}

ScopedTimer::~ScopedTimer()
{
    if (!active_) return;
    add_duration_ns(name_, now_ns() - start_ns_);
}

} // namespace soff::perf
```

- [ ] **Step 5: Run smoke to verify telemetry passes**

Run: `xmake build soff_smoke -y && build\windows\x64\release\soff_smoke.exe`

Expected: build succeeds and smoke exits with code 0.

- [ ] **Step 6: Optional commit checkpoint**

Only if the user explicitly requested commits:

```bash
git add include/soff/core/perf.hpp src/core/perf.cpp tests/smoke.cpp
git commit -m "perf: add disabled-by-default telemetry"
```

---

### Task 2: Instrument Database, Diff, Export, and Plugin Startup

**Files:**
- Modify: `src/db/database.cpp`
- Modify: `src/diff/sql_runner.cpp`
- Modify: `src/diff/session.cpp`
- Modify: `src/diff/propagation.cpp`
- Modify: `src/plugin/soff_plugin.cpp`

- [ ] **Step 1: Count SQL statements in the SQLite wrapper**

In `src/db/database.cpp`, add:

```cpp
#include "soff/core/perf.hpp"
```

In `Database::execute(std::string_view sql)`, `Database::execute(std::string_view sql, const std::vector<std::string>& values)`, `Database::prepare(std::string_view sql)`, and `Database::query_rows(std::string_view sql)`, add the relevant counter at the top of each function body:

```cpp
soff::perf::add_counter("sql.execute");
soff::perf::add_counter("sql.execute.bound");
soff::perf::add_counter("sql.prepare");
soff::perf::add_counter("sql.query_rows");
```

- [ ] **Step 2: Add timing spans to diff runner**

In `src/diff/sql_runner.cpp`, add:

```cpp
#include "soff/core/perf.hpp"
```

Inside `query_with_controls()` before `database.query_rows(sql)`:

```cpp
        soff::perf::ScopedTimer timer("sql.query");
        auto rows = database.query_rows(sql);
```

Inside the candidate loop in `SqlHeuristicRunner::run_all()`, wrap parse and ratio blocks with:

```cpp
soff::perf::ScopedTimer parse_timer("sql.parse_candidate");
```

and:

```cpp
soff::perf::ScopedTimer ratio_timer("ratio.compute");
```

- [ ] **Step 3: Add timing spans to session and propagation**

In `src/diff/session.cpp` and `src/diff/propagation.cpp`, include `soff/core/perf.hpp` and add scoped timers at the start of `DiffSession::run_all()`, `DiffSession::run_exact()`, and `run_propagation()`:

```cpp
soff::perf::ScopedTimer timer("diff.run_all");
soff::perf::ScopedTimer timer("diff.run_exact");
soff::perf::ScopedTimer timer("diff.propagation");
```

For heuristic reruns and BB matching loops, add counters:

```cpp
soff::perf::add_counter("diff.fixed_point.rerun");
soff::perf::add_counter("diff.bb_matching.attempt");
```

- [ ] **Step 4: Add plugin/export timing and report output**

In `src/plugin/soff_plugin.cpp`, include `soff/core/perf.hpp` after the existing core includes.

At the start of `build_ida_snapshot()`:

```cpp
soff::perf::ScopedTimer timer("export.total");
```

Around each `read_function_feature()` call:

```cpp
soff::perf::ScopedTimer function_timer("export.read_function");
auto feature = read_function_feature(function, imagebase, &hexrays, options.use_microcode, options.ignore_small_functions);
```

At the start of `plugmod_t* idaapi init()`:

```cpp
soff::perf::ScopedTimer timer("plugin.init");
```

After export/diff user actions complete, emit:

```cpp
if (soff::perf::enabled()) {
    msg("%s", soff::perf::render_report().c_str());
}
```

- [ ] **Step 5: Build and run smoke**

Run: `xmake build -y && build\windows\x64\release\soff_smoke.exe`

Expected: build succeeds and smoke exits with code 0.

- [ ] **Step 6: Verify disabled telemetry has no output**

Run: `build\windows\x64\release\soff_smoke.exe`

Expected: no telemetry report is printed by smoke.

- [ ] **Step 7: Optional commit checkpoint**

Only if the user explicitly requested commits:

```bash
git add src/db/database.cpp src/diff/sql_runner.cpp src/diff/session.cpp src/diff/propagation.cpp src/plugin/soff_plugin.cpp
git commit -m "perf: instrument soff hotspots"
```

---

### Task 3: Cache Plugin Result and Snapshot Reloads

**Files:**
- Modify: `src/plugin/soff_plugin.cpp`

- [ ] **Step 1: Add a small result/snapshot cache helper**

Near the anonymous namespace helpers in `src/plugin/soff_plugin.cpp`, add:

```cpp
template <typename Value>
struct PathCache
{
    std::filesystem::path path;
    std::optional<Value> value;

    void clear()
    {
        path.clear();
        value.reset();
    }
};

soff::ProgramSnapshot load_snapshot_cached(
    PathCache<soff::ProgramSnapshot>& cache,
    const std::filesystem::path& path)
{
    if (!cache.value || cache.path != path) {
        soff::perf::ScopedTimer timer("plugin.snapshot.load");
        cache.path = path;
        cache.value = soff::SnapshotRepository{}.load(path);
    }
    return *cache.value;
}
```

Add `#include <optional>` if the file does not already include it.

- [ ] **Step 2: Replace repeated snapshot loads in graph/diff handlers**

For each repeated pair like:

```cpp
const auto primary_snapshot = soff::SnapshotRepository{}.load(results.main_db);
const auto secondary_snapshot = soff::SnapshotRepository{}.load(results.diff_db);
```

replace with class/member-level caches where the handler object persists:

```cpp
auto primary_snapshot = load_snapshot_cached(primary_snapshot_cache_, results.main_db);
auto secondary_snapshot = load_snapshot_cached(secondary_snapshot_cache_, results.diff_db);
```

Add members to the owning chooser/view class:

```cpp
PathCache<soff::ProgramSnapshot> primary_snapshot_cache_;
PathCache<soff::ProgramSnapshot> secondary_snapshot_cache_;
```

- [ ] **Step 3: Stop chooser refresh/edit from reloading unchanged results**

In the chooser class around lines where `ResultRepository{}.load(result_path_)` is called, add:

```cpp
void ensure_results_loaded()
{
    if (!results_loaded_) {
        soff::perf::ScopedTimer timer("plugin.results.load");
        results_ = soff::db::ResultRepository{}.load(result_path_);
        results_loaded_ = true;
    }
}
```

Replace direct loads in `refresh()` and `edit()` with `ensure_results_loaded()`.

Add members:

```cpp
bool results_loaded_ = false;
```

When `result_path_` changes, set `results_loaded_ = false;` and clear snapshot caches.

- [ ] **Step 4: Build plugin target**

Run: `xmake config --ida_plugin=y -y && xmake build soff_ida -y`

Expected: `build\windows\x64\release\soff.dll` builds.

- [ ] **Step 5: Run smoke**

Run: `xmake build soff_smoke -y && build\windows\x64\release\soff_smoke.exe`

Expected: smoke exits with code 0.

- [ ] **Step 6: Optional commit checkpoint**

Only if the user explicitly requested commits:

```bash
git add src/plugin/soff_plugin.cpp
git commit -m "perf: cache plugin result and snapshot loads"
```

---

### Task 4: Add Diff Function Cache Skeleton

**Files:**
- Create: `include/soff/diff/function_cache.hpp`
- Create: `src/diff/function_cache.cpp`
- Modify: `tests/smoke.cpp`

- [ ] **Step 1: Add failing smoke test for cache loading**

Add include to `tests/smoke.cpp`:

```cpp
#include "soff/diff/function_cache.hpp"
```

After `smoke_db.open(db_path);`, add:

```cpp
    const auto primary_cache = soff::diff::load_function_cache(smoke_db, "main");
    assert(primary_cache.by_address.find(0x401000) != primary_cache.by_address.end());
    assert(primary_cache.by_address.at(0x401000).name == "start");
    assert(primary_cache.by_name.find("start") != primary_cache.by_name.end());
```

- [ ] **Step 2: Run smoke to verify failure**

Run: `xmake build soff_smoke -y`

Expected: compile failure because `function_cache.hpp` does not exist.

- [ ] **Step 3: Create cache header**

Create `include/soff/diff/function_cache.hpp`:

```cpp
#pragma once

#include "soff/analysis/model.hpp"
#include "soff/db/database.hpp"

#include <boost/unordered/unordered_flat_map.hpp>
#include <string>
#include <vector>

namespace soff::diff {

struct CachedFunction
{
    Address address = 0;
    std::string name;
    std::string clean_assembly;
    std::string clean_pseudo;
    std::string pseudocode_primes;
    std::string bytes_hash;
    std::string md_index;
    std::string constants;
    std::string source_file;
    int nodes = 0;
    int edges = 0;
    int instructions = 0;
    int size = 0;
    int constants_count = 0;
};

struct FunctionCache
{
    boost::unordered_flat_map<Address, CachedFunction> by_address;
    boost::unordered_flat_map<std::string, Address> by_name;
};

FunctionCache load_function_cache(db::Database& database, std::string_view schema_name);

} // namespace soff::diff
```

- [ ] **Step 4: Implement cache loading**

Create `src/diff/function_cache.cpp`:

```cpp
#include "soff/diff/function_cache.hpp"

#include "soff/core/perf.hpp"

#include <charconv>
#include <stdexcept>

namespace soff::diff {
namespace {

Address parse_address(const std::string& text)
{
    Address value = 0;
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, value, 10);
    if (result.ec == std::errc{} && result.ptr == end) return value;
    return static_cast<Address>(std::stoull(text, nullptr, 0));
}

int parse_int(const std::string& text)
{
    return text.empty() ? 0 : std::stoi(text);
}

std::string table_prefix(std::string_view schema_name)
{
    return schema_name == "main" ? std::string{} : std::string(schema_name) + ".";
}

} // namespace

FunctionCache load_function_cache(db::Database& database, std::string_view schema_name)
{
    soff::perf::ScopedTimer timer("diff.function_cache.load");
    const auto prefix = table_prefix(schema_name);
    const auto rows = database.query_rows(
        "select address, coalesce(name, ''), coalesce(clean_assembly, ''), coalesce(clean_pseudo, ''), "
        "coalesce(pseudocode_primes, ''), coalesce(bytes_hash, ''), coalesce(md_index, ''), "
        "coalesce(constants, ''), coalesce(source_file, ''), coalesce(nodes, 0), coalesce(edges, 0), "
        "coalesce(instructions, 0), coalesce(size, 0), coalesce(constants_count, 0) "
        "from " + prefix + "functions");

    FunctionCache cache;
    cache.by_address.reserve(rows.size());
    cache.by_name.reserve(rows.size());
    for (const auto& row : rows) {
        CachedFunction function;
        function.address = parse_address(row[0]);
        function.name = row[1];
        function.clean_assembly = row[2];
        function.clean_pseudo = row[3];
        function.pseudocode_primes = row[4];
        function.bytes_hash = row[5];
        function.md_index = row[6];
        function.constants = row[7];
        function.source_file = row[8];
        function.nodes = parse_int(row[9]);
        function.edges = parse_int(row[10]);
        function.instructions = parse_int(row[11]);
        function.size = parse_int(row[12]);
        function.constants_count = parse_int(row[13]);
        if (!function.name.empty()) cache.by_name.emplace(function.name, function.address);
        cache.by_address.emplace(function.address, std::move(function));
    }
    return cache;
}

} // namespace soff::diff
```

- [ ] **Step 5: Run smoke**

Run: `xmake build soff_smoke -y && build\windows\x64\release\soff_smoke.exe`

Expected: smoke exits with code 0.

- [ ] **Step 6: Optional commit checkpoint**

Only if the user explicitly requested commits:

```bash
git add include/soff/diff/function_cache.hpp src/diff/function_cache.cpp tests/smoke.cpp
git commit -m "perf: add diff function cache"
```

---

### Task 5: Use Function Cache for Deep Ratio Bonus

**Files:**
- Modify: `include/soff/diff/sql_runner.hpp`
- Modify: `src/diff/sql_runner.cpp`
- Modify: `src/diff/session.cpp`
- Modify: `tests/smoke.cpp`

- [ ] **Step 1: Extend SQL runner options with cache pointers**

In `include/soff/diff/sql_runner.hpp`, include the cache header:

```cpp
#include "soff/diff/function_cache.hpp"
```

Add fields to `SqlRunnerOptions`:

```cpp
    const FunctionCache* primary_cache = nullptr;
    const FunctionCache* secondary_cache = nullptr;
```

- [ ] **Step 2: Replace point queries in `deep_ratio_bonus()`**

In `src/diff/sql_runner.cpp`, change the helper signature from:

```cpp
double deep_ratio_bonus(db::Database& database, const CandidateRow& candidate)
```

to:

```cpp
double deep_ratio_bonus(
    db::Database& database,
    const CandidateRow& candidate,
    const SqlRunnerOptions& options)
```

At the top of the function, add the cache fast path:

```cpp
    if (options.primary_cache != nullptr && options.secondary_cache != nullptr) {
        const auto primary = options.primary_cache->by_address.find(candidate.primary);
        const auto secondary = options.secondary_cache->by_address.find(candidate.secondary);
        if (primary != options.primary_cache->by_address.end()
            && secondary != options.secondary_cache->by_address.end()) {
            soff::perf::add_counter("ratio.deep_bonus.cache_hit");
            const auto& left = primary->second;
            const auto& right = secondary->second;
            double bonus = 0.0;
            if (!left.source_file.empty() && left.source_file == right.source_file) bonus += 0.05;
            if (!left.pseudocode_primes.empty() && left.pseudocode_primes == right.pseudocode_primes) bonus += 0.10;
            if (!left.constants.empty() && left.constants == right.constants) bonus += 0.05;
            return std::min(0.20, bonus);
        }
    }
    soff::perf::add_counter("ratio.deep_bonus.sql_fallback");
```

Update call sites from `deep_ratio_bonus(database, candidate)` to `deep_ratio_bonus(database, candidate, options)`.

- [ ] **Step 3: Build caches in diff session**

In `src/diff/session.cpp`, include:

```cpp
#include "soff/diff/function_cache.hpp"
```

After attaching the diff database and before constructing `sql_options`, add:

```cpp
    auto primary_cache = load_function_cache(database, "main");
    auto secondary_cache = load_function_cache(database, "diff");
```

After `auto sql_options = options.sql;`, add:

```cpp
    sql_options.primary_cache = &primary_cache;
    sql_options.secondary_cache = &secondary_cache;
```

- [ ] **Step 4: Run smoke and fixture diff**

Run: `xmake build -y && build\windows\x64\release\soff_smoke.exe`

Expected: smoke exits with code 0.

Run if CLI fixture paths exist: `build\windows\x64\release\soff_cli.exe diff ..\test\soff_test_a.sqlite ..\test\soff_test_b.sqlite -o build\soff_perf_check.soff`

Expected: diff completes and writes `build\soff_perf_check.soff`.

- [ ] **Step 5: Optional commit checkpoint**

Only if the user explicitly requested commits:

```bash
git add include/soff/diff/sql_runner.hpp src/diff/sql_runner.cpp src/diff/session.cpp tests/smoke.cpp
git commit -m "perf: use function cache for deep ratio bonus"
```

---

### Task 6: Add Covering Indexes for Diff Queries

**Files:**
- Modify: `src/db/schema.cpp`
- Modify: `tests/smoke.cpp`

- [ ] **Step 1: Add smoke assertions for new indexes**

After the existing index-count assertion in `tests/smoke.cpp`, add:

```cpp
    assert(diaphora_version_db.query_int(
        "select count(*) from sqlite_master where type = 'index' and sql like '%address, bytes_hash%'") > 0);
    assert(diaphora_version_db.query_int(
        "select count(*) from sqlite_master where type = 'index' and sql like '%md_index, nodes, edges%'") > 0);
    assert(diaphora_version_db.query_int(
        "select count(*) from sqlite_master where type = 'index' and sql like '%func_id, type, address%'") > 0);
```

- [ ] **Step 2: Run smoke to verify failure**

Run: `xmake build soff_smoke -y && build\windows\x64\release\soff_smoke.exe`

Expected: assertion failure because the new covering indexes do not exist.

- [ ] **Step 3: Add index definitions**

In `src/db/schema.cpp`, append these entries to the `indexes` list:

```cpp
            {"functions", "address, bytes_hash"},
            {"functions", "name, nodes, instructions"},
            {"functions", "md_index, nodes, edges"},
            {"callgraph", "func_id, type, address"},
            {"compilation_unit_functions", "cu_id, func_id"},
```

- [ ] **Step 4: Run smoke**

Run: `xmake build soff_smoke -y && build\windows\x64\release\soff_smoke.exe`

Expected: smoke exits with code 0.

- [ ] **Step 5: Optional commit checkpoint**

Only if the user explicitly requested commits:

```bash
git add src/db/schema.cpp tests/smoke.cpp
git commit -m "perf: add diff covering indexes"
```

---

### Task 7: Reuse Prepared Statements for Snapshot Export Inserts

**Files:**
- Modify: `src/db/repository.cpp`
- Modify: `tests/smoke.cpp`

- [ ] **Step 1: Add a statement bundle in repository.cpp**

In `src/db/repository.cpp`, before `insert_function_rows()`, add:

```cpp
struct SnapshotInsertStatements
{
    db::Statement insert_function;
    db::Statement insert_instruction;
    db::Statement insert_basic_block;
    db::Statement insert_function_block;
    db::Statement insert_bb_instruction;
    db::Statement insert_bb_relation;
    db::Statement insert_callgraph;
    db::Statement insert_constant;
};

void bind_and_step(db::Statement& statement, const std::vector<std::string>& values)
{
    for (std::size_t i = 0; i < values.size(); ++i) {
        statement.bind(static_cast<int>(i + 1), values[i]);
    }
    if (statement.step()) {
        throw std::runtime_error("SQLite insert unexpectedly returned a row");
    }
    statement.reset();
}
```

- [ ] **Step 2: Add Database statement clear support if reset is insufficient**

If repeated statement binding leaks previous values, add `Statement::clear_bindings()` to `include/soff/db/database.hpp` and implement it in `src/db/database.cpp` using `sqlite3_clear_bindings`. Then call it from `bind_and_step()` after `statement.reset()`:

```cpp
statement.clear_bindings();
```

- [ ] **Step 3: Refactor `insert_function_rows()` to accept reusable statements**

Change:

```cpp
void insert_function_rows(db::Database& database, const FunctionFeature& function)
```

to:

```cpp
void insert_function_rows(db::Database& database, SnapshotInsertStatements& statements, const FunctionFeature& function)
```

Use `bind_and_step(statements.insert_function, values);` for the `functions` insert instead of `database.execute(sql, values)`.

For instructions, basic blocks, relations, callgraph, and constants, replace `database.execute(insert_sql, values)` with the matching prepared statement and `bind_and_step()`.

- [ ] **Step 4: Create statements once per append transaction**

In `SnapshotRepository::append_functions()`, after opening the database and starting the transaction, create the bundle:

```cpp
SnapshotInsertStatements statements{
    database.prepare(
        "insert into functions (name, address, rva, segment_rva, nodes, edges, size, instructions, "
        "indegree, outdegree, cyclomatic_complexity, primes_value, strongly_connected, loops, "
        "tarjan_topological_sort, strongly_connected_spp, mnemonics_spp, switches, mnemonics, names, "
        "prototype, mangled_function, bytes_hash, assembly, prototype2, function_flags, "
        "clean_assembly, function_hash, bytes_sum, md_index, constants_count, constants, "
        "assembly_addrs, kgh_hash, source_file, userdata, export_time, comment, pseudocode, clean_pseudo, pseudocode_lines, pseudocode_hash1, "
        "pseudocode_hash2, pseudocode_hash3, pseudocode_primes, microcode, clean_microcode, microcode_spp) "
        "values (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, "
        "nullif(?, ''), nullif(?, ''), ?, nullif(?, ''), nullif(?, ''), nullif(?, ''), nullif(?, ''), "
        "nullif(?, ''), nullif(?, ''), ?)") ,
    database.prepare("insert into instructions (func_id, address, disasm, mnemonic, comment1, comment2, operand_names, name, type, pseudocomment, pseudoitp, asm_type) values (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"),
    database.prepare("insert into basic_blocks (num, address, asm_type) values (?, ?, ?)"),
    database.prepare("insert into function_bblocks (function_id, basic_block_id, asm_type) values (?, ?, ?)"),
    database.prepare("insert into bb_instructions (basic_block_id, instruction_id) values (?, ?)"),
    database.prepare("insert into bb_relations (parent_id, child_id) values (?, ?)"),
    database.prepare("insert into callgraph (func_id, address, type) values (?, ?, ?)"),
    database.prepare("insert into constants (func_id, constant) values (?, ?)")
};
```

Keep this SQL text in a local `const char* insert_function_sql` so the statement bundle is readable and matches the existing function column order.

Update loop calls:

```cpp
insert_function_rows(database, statements, function);
```

- [ ] **Step 5: Run smoke and compare export row counts**

Run: `xmake build soff_smoke -y && build\windows\x64\release\soff_smoke.exe`

Expected: smoke exits with code 0 and existing row-count assertions still pass.

- [ ] **Step 6: Optional commit checkpoint**

Only if the user explicitly requested commits:

```bash
git add include/soff/db/database.hpp src/db/database.cpp src/db/repository.cpp tests/smoke.cpp
git commit -m "perf: reuse snapshot export insert statements"
```

---

### Task 8: Apply Export Pragmas and Tune Export Batches

**Files:**
- Modify: `src/db/repository.cpp`
- Modify: `src/plugin/soff_plugin.cpp`

- [ ] **Step 1: Apply performance pragmas during incremental export**

In `SnapshotRepository::begin_incremental_save()` and `SnapshotRepository::append_functions()`, after `database.open(path);`, add:

```cpp
database.apply_performance_pragmas();
```

Keep `PRAGMA foreign_keys = ON` after opening and before schema writes.

- [ ] **Step 2: Increase async export writer batch size conservatively**

In `src/plugin/soff_plugin.cpp`, find the export async writer/batch constant around `build_ida_snapshot()` and change a value below `512` to:

```cpp
constexpr std::size_t export_batch_size = 512;
```

If the existing value is already greater than or equal to 512, leave it unchanged.

- [ ] **Step 3: Preserve crash-resume metadata**

Verify the code still updates:

```cpp
stats.last_function_index
stats.last_function_address
stats.last_function_name
```

before handing a batch to the writer. If not, move those assignments before batch enqueue.

- [ ] **Step 4: Run smoke**

Run: `xmake build -y && build\windows\x64\release\soff_smoke.exe`

Expected: smoke exits with code 0.

- [ ] **Step 5: Optional commit checkpoint**

Only if the user explicitly requested commits:

```bash
git add src/db/repository.cpp src/plugin/soff_plugin.cpp
git commit -m "perf: tune export sqlite pragmas and batches"
```

---

### Task 9: Reduce Export Hot-Loop Allocation Safely

**Files:**
- Modify: `src/plugin/soff_plugin.cpp`

- [ ] **Step 1: Reserve function feature containers**

At the start of `read_function_feature()` after creating `feature`, add:

```cpp
const auto estimated_instructions = static_cast<std::size_t>(std::max<ea_t>(1, function->end_ea - function->start_ea) / 4);
feature.instruction_details.reserve(estimated_instructions);
feature.call_references.reserve(16);
feature.constants.reserve(16);
feature.blocks.reserve(static_cast<std::size_t>(std::max(1, function->size())));
feature.assembly.reserve(estimated_instructions * 32);
feature.stripped_assembly.reserve(estimated_instructions * 24);
feature.mnemonics.reserve(estimated_instructions * 8);
```

- [ ] **Step 2: Avoid repeated assembly delimiter allocations**

Where assembly/mnemonics strings append newlines, use direct char append:

```cpp
feature.assembly.push_back('\n');
feature.stripped_assembly.push_back('\n');
feature.mnemonics.push_back('\n');
```

instead of appending temporary `"\n"` strings in the hot loop.

- [ ] **Step 3: Add telemetry counters for expensive optional paths**

Inside switch extraction, string xref extraction, and decompiler/microcode branches, add:

```cpp
soff::perf::add_counter("export.switch_info.attempt");
soff::perf::add_counter("export.string_xref.attempt");
soff::perf::add_counter("export.hexrays.pseudocode.attempt");
soff::perf::add_counter("export.hexrays.microcode.attempt");
```

- [ ] **Step 4: Build plugin and smoke**

Run: `xmake config --ida_plugin=y -y && xmake build -y && build\windows\x64\release\soff_smoke.exe`

Expected: all targets build and smoke exits with code 0.

- [ ] **Step 5: Optional commit checkpoint**

Only if the user explicitly requested commits:

```bash
git add src/plugin/soff_plugin.cpp
git commit -m "perf: reduce export hot-loop allocation"
```

---

### Task 10: Control Fixed-Point Heuristic Reruns

**Files:**
- Modify: `include/soff/diff/session.hpp`
- Modify: `src/diff/session.cpp`
- Modify: `tests/smoke.cpp`

- [ ] **Step 1: Add configurable fixed-point thresholds**

In `include/soff/diff/session.hpp`, add fields to `DiffSessionOptions`:

```cpp
    int max_fixed_point_iterations = 2;
    double fixed_point_min_delta_ratio = 0.005;
```

- [ ] **Step 2: Replace hardcoded iteration count**

In `src/diff/session.cpp`, replace:

```cpp
constexpr int max_fixed_point_iterations = 3;
```

with:

```cpp
const int max_fixed_point_iterations = std::max(0, options.max_fixed_point_iterations);
const auto total_function_count = std::max<std::size_t>(1, primary_count + secondary_count);
```

After propagation returns, compute:

```cpp
const auto propagation_delta = results.matches.size() - before_propagation_count;
const auto delta_ratio = static_cast<double>(propagation_delta) / static_cast<double>(total_function_count);
```

Only rerun heuristics if:

```cpp
delta_ratio >= options.fixed_point_min_delta_ratio
```

Add counter:

```cpp
if (delta_ratio < options.fixed_point_min_delta_ratio) {
    soff::perf::add_counter("diff.fixed_point.skip_small_delta");
}
```

- [ ] **Step 3: Run smoke and fixture diff**

Run: `xmake build -y && build\windows\x64\release\soff_smoke.exe`

Expected: smoke exits with code 0.

Run fixture diff if available and compare result summary with baseline generated before this task.

- [ ] **Step 4: Optional commit checkpoint**

Only if the user explicitly requested commits:

```bash
git add include/soff/diff/session.hpp src/diff/session.cpp tests/smoke.cpp
git commit -m "perf: reduce fixed point heuristic reruns"
```

---

### Task 11: Cache Basic-Block Matching Results

**Files:**
- Modify: `src/diff/session.cpp`
- Modify: `src/diff/bb_matching.cpp`

- [ ] **Step 1: Memoize BB match results in session**

In `src/diff/session.cpp`, add a local key type near helper functions:

```cpp
struct AddressPairKey
{
    Address primary = 0;
    Address secondary = 0;

    bool operator==(const AddressPairKey& other) const noexcept
    {
        return primary == other.primary && secondary == other.secondary;
    }
};

struct AddressPairHash
{
    std::size_t operator()(const AddressPairKey& key) const noexcept
    {
        return std::hash<Address>{}(key.primary) ^ (std::hash<Address>{}(key.secondary) << 1);
    }
};
```

Before the BB matching loop, add:

```cpp
std::unordered_map<AddressPairKey, BasicBlockMatchResult, AddressPairHash> bb_cache;
```

Replace direct calls:

```cpp
const auto bb_result = match_basic_blocks(database, match.primary, match.secondary);
```

with:

```cpp
const AddressPairKey key{match.primary, match.secondary};
auto cached = bb_cache.find(key);
if (cached == bb_cache.end()) {
    soff::perf::add_counter("diff.bb_matching.cache_miss");
    cached = bb_cache.emplace(key, match_basic_blocks(database, match.primary, match.secondary)).first;
} else {
    soff::perf::add_counter("diff.bb_matching.cache_hit");
}
const auto& bb_result = cached->second;
```

- [ ] **Step 2: Skip BB matching when it cannot improve classification**

Before attempting BB matching, add:

```cpp
if (match.ratio >= 0.95 || match.ratio < 0.30) {
    soff::perf::add_counter("diff.bb_matching.skip_ratio");
    continue;
}
```

Keep the existing node-count skip.

- [ ] **Step 3: Run smoke and fixture diff**

Run: `xmake build -y && build\windows\x64\release\soff_smoke.exe`

Expected: smoke exits with code 0.

Run fixture diff if available and inspect changes in partial/unreliable counts.

- [ ] **Step 4: Optional commit checkpoint**

Only if the user explicitly requested commits:

```bash
git add src/diff/session.cpp src/diff/bb_matching.cpp
git commit -m "perf: cache basic block matching"
```

---

### Task 12: Convert Propagation Hot Queries to Cache or Batched Loads

**Files:**
- Modify: `include/soff/diff/propagation.hpp`
- Modify: `src/diff/propagation.cpp`
- Modify: `src/diff/session.cpp`

- [ ] **Step 1: Extend propagation options with caches**

In `include/soff/diff/propagation.hpp`, include `soff/diff/function_cache.hpp` and add to `PropagationOptions`:

```cpp
    const FunctionCache* primary_cache = nullptr;
    const FunctionCache* secondary_cache = nullptr;
```

- [ ] **Step 2: Pass caches from session to propagation**

In `src/diff/session.cpp`, before `run_propagation()`, copy options and assign caches:

```cpp
auto propagation_options = options.propagation;
propagation_options.primary_cache = &primary_cache;
propagation_options.secondary_cache = &secondary_cache;
```

Pass `propagation_options` instead of `options.propagation`.

- [ ] **Step 3: Replace same-name propagation SQL with cache path**

At the top of `find_same_name()` in `src/diff/propagation.cpp`, if both caches exist, iterate the smaller `by_name` map and create matches for shared names not in pre-matched sets:

```cpp
if (options.primary_cache != nullptr && options.secondary_cache != nullptr) {
    soff::perf::add_counter("propagation.same_name.cache_path");
    for (const auto& [name, primary_address] : options.primary_cache->by_name) {
        const auto secondary = options.secondary_cache->by_name.find(name);
        if (secondary == options.secondary_cache->by_name.end()) continue;
        if (matched_primary.count(primary_address) != 0 || matched_secondary.count(secondary->second) != 0) continue;
        matches.push_back({db::MatchKind::partial, primary_address, name, secondary->second, name, 1.0, "Same name propagation", ""});
        matched_primary.insert(primary_address);
        matched_secondary.insert(secondary->second);
    }
    return matches.size();
}
```

The cache branch uses the existing `matches`, `matched_primary`, `matched_secondary`, `min_ratio`, and `same_processor` variables from `find_same_name()`.

- [ ] **Step 4: Batch constants and callgraph queries for remaining SQL paths**

For each propagation pass that still performs per-match queries, first build local maps once:

```cpp
auto constant_rows = database.query_rows("select func_id, constant from constants union all select func_id, constant from diff.constants");
auto call_rows = database.query_rows("select func_id, address, type from callgraph union all select func_id, address, type from diff.callgraph");
```

Replace inner-loop point queries with map lookups. Keep SQL fallback behind cache absence.

- [ ] **Step 5: Run smoke and fixture diff**

Run: `xmake build -y && build\windows\x64\release\soff_smoke.exe`

Expected: smoke exits with code 0.

Run fixture diff if available and compare match counts.

- [ ] **Step 6: Optional commit checkpoint**

Only if the user explicitly requested commits:

```bash
git add include/soff/diff/propagation.hpp src/diff/propagation.cpp src/diff/session.cpp
git commit -m "perf: reduce propagation point queries"
```

---

### Task 13: Add MD Index Fast Path

**Files:**
- Modify: `src/diff/session.cpp`
- Modify: `tests/smoke.cpp`

- [ ] **Step 1: Add helper to find unique MD index matches**

In `src/diff/session.cpp`, add helper:

```cpp
std::vector<db::ResultMatch> find_md_index_matches(
    const FunctionCache& primary_cache,
    const FunctionCache& secondary_cache,
    boost::unordered_flat_set<Address>& matched_primary,
    boost::unordered_flat_set<Address>& matched_secondary)
{
    std::unordered_map<std::string, Address> primary_by_md;
    std::unordered_map<std::string, bool> duplicate_md;
    for (const auto& [address, function] : primary_cache.by_address) {
        if (function.md_index.empty() || function.nodes < 3) continue;
        auto [it, inserted] = primary_by_md.emplace(function.md_index, address);
        if (!inserted) duplicate_md[function.md_index] = true;
    }

    std::vector<db::ResultMatch> matches;
    for (const auto& [secondary_address, secondary] : secondary_cache.by_address) {
        if (secondary.md_index.empty() || duplicate_md[secondary.md_index]) continue;
        const auto primary = primary_by_md.find(secondary.md_index);
        if (primary == primary_by_md.end()) continue;
        const auto& left = primary_cache.by_address.at(primary->second);
        if (std::abs(left.nodes - secondary.nodes) > 1) continue;
        if (matched_primary.count(primary->second) != 0 || matched_secondary.count(secondary_address) != 0) continue;
        matches.push_back({db::MatchKind::best, primary->second, left.name, secondary_address, secondary.name, 1.0, "Same unique MD index", ""});
        matched_primary.insert(primary->second);
        matched_secondary.insert(secondary_address);
    }
    return matches;
}
```

- [ ] **Step 2: Invoke before broad SQL heuristics**

After equal/name fast paths and before `runner.run_all()`, add:

```cpp
auto md_matches = find_md_index_matches(primary_cache, secondary_cache, pre_matched_p, pre_matched_s);
results.matches.insert(results.matches.end(), md_matches.begin(), md_matches.end());
soff::perf::add_counter("diff.md_index_fast_path.matches", md_matches.size());
```

- [ ] **Step 3: Run smoke and fixture diff**

Run: `xmake build -y && build\windows\x64\release\soff_smoke.exe`

Expected: smoke exits with code 0.

Run fixture diff and inspect that new MD index matches are plausible and do not introduce duplicates.

- [ ] **Step 4: Optional commit checkpoint**

Only if the user explicitly requested commits:

```bash
git add src/diff/session.cpp tests/smoke.cpp
git commit -m "perf: add md index diff fast path"
```

---

### Task 14: Final Benchmark and Regression Pass

**Files:**
- Modify: `docs/superpowers/specs/2026-06-03-soff-performance-optimization-design.md` only if measured acceptance criteria need clarification.

- [ ] **Step 1: Clean build**

Run: `xmake build -y`

Expected: all default targets build.

- [ ] **Step 2: Run smoke**

Run: `build\windows\x64\release\soff_smoke.exe`

Expected: exits with code 0.

- [ ] **Step 3: Run fixture diff**

Run: `build\windows\x64\release\soff_cli.exe diff ..\test\soff_test_a.sqlite ..\test\soff_test_b.sqlite -o build\soff_perf_check.soff`

Expected: diff completes and writes `build\soff_perf_check.soff`.

- [ ] **Step 4: Run fixture diff with telemetry**

Run in PowerShell:

```powershell
$env:SOFF_PERF='1'; build\windows\x64\release\soff_cli.exe diff ..\test\soff_test_a.sqlite ..\test\soff_test_b.sqlite -o build\soff_perf_check_perf.soff; Remove-Item Env:\SOFF_PERF
```

Expected: diff completes and telemetry contains `diff.function_cache.load`, `sql.query`, and heuristic timing rows.

- [ ] **Step 5: Build IDA plugin**

Run: `xmake config --ida_plugin=y -y && xmake build soff_ida -y`

Expected: `build\windows\x64\release\soff.dll` builds.

- [ ] **Step 6: Summarize measured results**

Record in the final handoff:

```text
Smoke: PASS/FAIL
CLI fixture diff: PASS/FAIL
IDA plugin build: PASS/FAIL
Telemetry enabled run: PASS/FAIL
Notable remaining hotspots: <top telemetry rows>
```

- [ ] **Step 7: Optional commit checkpoint**

Only if the user explicitly requested commits:

```bash
git add .
git commit -m "perf: optimize soff startup export and diff"
```
