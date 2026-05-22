#pragma once

#include "soff/db/database.hpp"
#include "soff/db/result_repository.hpp"

#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>
#include <boost/unordered/unordered_flat_set.hpp>
#include <vector>

namespace soff {
class DiffHooks;
} // namespace soff

namespace soff::diff {

struct SqlRunnerOptions
{
    std::string postfix = " and f.instructions > 5 and df.instructions > 5 ";
    bool same_processor = true;
    std::size_t max_processed_rows = 1000000;
    double default_partial_ratio = 0.5;
    double trusted_partial_ratio = 0.3;
    bool enable_unreliable = false;
    bool enable_slow = true;
    bool enable_experimental = false;
    bool enable_relaxed_ratio = false;
    std::uint32_t timeout_seconds = 300;
    int progress_check_interval = 1000;
    std::function<bool()> cancel_requested;
    DiffHooks* hooks = nullptr;
    boost::unordered_flat_set<Address> pre_matched_primary;
    boost::unordered_flat_set<Address> pre_matched_secondary;
};

struct SqlHeuristicStats
{
    std::string name;
    std::size_t candidates = 0;
    std::size_t accepted = 0;
    std::size_t rejected = 0;
    std::size_t skipped = 0;
    std::size_t multimatches = 0;
    bool row_limit_hit = false;
    bool timeout_hit = false;
    bool cancelled = false;
};

struct SqlHeuristicRunResult
{
    std::vector<db::ResultMatch> matches;
    std::vector<SqlHeuristicStats> stats;
};

class SqlHeuristicRunner
{
public:
    SqlHeuristicRunResult run_exact(db::Database& database, const SqlRunnerOptions& options = {}) const;
    SqlHeuristicRunResult run_all(db::Database& database, const SqlRunnerOptions& options = {}) const;
};

std::string apply_postfix(std::string sql, std::string_view postfix);

} // namespace soff::diff
