#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace soff {

struct DiffConfig
{
    // Match acceptance
    double matches_bonus_ratio = 0.01;
    double default_partial_ratio = 0.5;
    double trusted_partial_ratio = 0.3;
    double related_matches_min_ratio = 0.8;
    double minimum_rare_md_index = 10.0;

    // Propagation
    int max_functions_per_gap = 100;
    int diffing_matches_max_different_bblocks_percent = 25;
    int diffing_matches_min_bblocks = 3;

    // Speedups
    double speedup_stripped_binaries_min_percent = 99.0;
    double speedup_patch_diff_symbols_min_percent = 90.0;
    double speedup_patch_diff_renamed_function_min_ratio = 0.6;

    // SQL
    std::size_t sql_max_processed_rows = 1000000;
    std::uint32_t sql_timeout_seconds = 300;
    std::string sql_default_postfix =
        " and f.instructions > 5 and df.instructions > 5 ";

    // Feature flags
    bool enable_unreliable = false;
    bool enable_experimental = false;
    bool enable_slow_heuristics = true;
    bool enable_relaxed_ratio = false;
    bool ignore_sub_function_names = true;
    bool ignore_all_function_names = false;
    bool ignore_small_functions = false;

    // Auto-tuning thresholds
    std::size_t min_functions_to_disable_slow = 4001;
    std::size_t min_functions_to_consider_medium = 8001;
    std::size_t min_functions_to_consider_huge = 100000;

    // Ratio tuning
    double increase_ratio_per_constant_match_same_cpu = 0.006;
    double increase_ratio_per_constant_match = 0.008;
};

struct ExportConfig
{
    bool use_decompiler = true;
    bool use_microcode = true;
    bool exclude_library_thunk = true;
    bool only_non_ida_subs = true;
    bool function_summaries_only = false;
    bool export_compilation_units = true;
    int functions_to_commit = 5000;
    int fuzzy_hashing_block_size = 512;
};

struct DatabaseConfig
{
    std::string journal_mode = "WAL";
    std::string synchronous = "1";
};

} // namespace soff
