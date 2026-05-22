#pragma once

#include "soff/core/config.hpp"
#include "soff/db/result_repository.hpp"
#include "soff/diff/propagation.hpp"
#include "soff/diff/sql_runner.hpp"

#include <filesystem>
#include <string>

namespace soff {
class DiffHooks;
} // namespace soff

namespace soff::diff {

struct DiffSessionOptions
{
    DiffConfig config;
    SqlRunnerOptions sql;
    PropagationOptions propagation;
    bool auto_detect_same_processor = true;
    std::filesystem::path ml_model_path;
    DiffHooks* hooks = nullptr;
};

struct DiffSessionSummary
{
    std::filesystem::path main_db;
    std::filesystem::path diff_db;
    std::filesystem::path output_db;
    bool same_processor = false;
    std::size_t heuristics = 0;
    std::size_t candidates = 0;
    std::size_t accepted = 0;
    std::size_t multimatches = 0;
    std::size_t row_limited_heuristics = 0;
    std::size_t timed_out_heuristics = 0;
    std::size_t cancelled_heuristics = 0;
    PropagationStats propagation;
    db::ResultSummary results;
};

class DiffSession
{
public:
    explicit DiffSession(DiffSessionOptions options = {});

    DiffSessionSummary run_exact(
        const std::filesystem::path& main_db,
        const std::filesystem::path& diff_db,
        const std::filesystem::path& output_db) const;
    DiffSessionSummary run_all(
        const std::filesystem::path& main_db,
        const std::filesystem::path& diff_db,
        const std::filesystem::path& output_db) const;

private:
    DiffSessionOptions options_;
};

} // namespace soff::diff
