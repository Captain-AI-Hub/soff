#include "soff/diff/session.hpp"

#include "soff/core/hooks.hpp"
#include "soff/core/perf.hpp"
#include "soff/db/database.hpp"
#include "soff/db/repository.hpp"
#include "soff/diff/bb_matching.hpp"
#include "soff/diff/function_cache.hpp"
#include "soff/diff/ml_model.hpp"
#include "soff/diff/propagation.hpp"

#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

namespace soff::diff {
namespace {

std::filesystem::path normalized_path_for_comparison(const std::filesystem::path& path)
{
    std::error_code error;
    auto absolute = std::filesystem::absolute(path, error);
    if (error) {
        absolute = path;
    }
    absolute = absolute.lexically_normal();

    if (std::filesystem::exists(absolute, error) && !error) {
        const auto canonical = std::filesystem::weakly_canonical(absolute, error);
        if (!error) {
            return canonical;
        }
    }

    const auto parent = absolute.has_parent_path()
        ? absolute.parent_path()
        : std::filesystem::current_path(error);
    const auto canonical_parent = std::filesystem::weakly_canonical(parent, error);
    return error
        ? absolute
        : (canonical_parent / absolute.filename()).lexically_normal();
}

bool same_path(const std::filesystem::path& left, const std::filesystem::path& right)
{
    return normalized_path_for_comparison(left) == normalized_path_for_comparison(right);
}

void validate_output_path(
    const std::filesystem::path& main_db,
    const std::filesystem::path& diff_db,
    const std::filesystem::path& output_db)
{
    if (output_db.empty()) {
        throw std::invalid_argument("output database path must not be empty");
    }
    if (same_path(main_db, output_db)) {
        throw std::invalid_argument("output database path must differ from the primary database path");
    }
    if (same_path(diff_db, output_db)) {
        throw std::invalid_argument("output database path must differ from the secondary database path");
    }
}

bool detect_same_processor(db::Database& database)
{
    const auto primary_processor = database.query_text("select processor from program limit 1");
    const auto secondary_processor = database.query_text("select processor from diff.program limit 1");
    return !primary_processor.empty()
        && !secondary_processor.empty()
        && primary_processor == secondary_processor;
}

Address parse_address(const std::string& text)
{
    Address address = 0;
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, address, 10);
    if (result.ec == std::errc{} && result.ptr == end) {
        return address;
    }

    std::size_t consumed = 0;
    address = static_cast<Address>(std::stoull(text, &consumed, 0));
    if (consumed != text.size()) {
        throw std::runtime_error("invalid address text: " + text);
    }
    return address;
}

void cleanup_matches(
    std::vector<db::ResultMatch>& matches,
    boost::unordered_flat_set<Address>& matched_primary,
    boost::unordered_flat_set<Address>& matched_secondary)
{
    std::stable_sort(matches.begin(), matches.end(), [](const auto& a, const auto& b) {
        if (a.ratio != b.ratio) return a.ratio > b.ratio;
        if (a.kind != b.kind) {
            return a.kind != db::ResultKind::multimatch
                && b.kind == db::ResultKind::multimatch;
        }
        return false;
    });

    boost::unordered_flat_set<std::string> seen_pairs;
    boost::unordered_flat_map<Address, double> best_ratio_primary;
    boost::unordered_flat_map<Address, double> best_ratio_secondary;
    std::vector<db::ResultMatch> cleaned;
    std::vector<db::ResultMatch> multimatches;
    cleaned.reserve(matches.size());
    multimatches.reserve(matches.size());

    for (auto& match : matches) {
        const auto pair_key = std::to_string(match.primary) + "-"
            + std::to_string(match.secondary);
        if (seen_pairs.find(pair_key) != seen_pairs.end()) continue;
        seen_pairs.insert(pair_key);

        if (match.kind == db::ResultKind::multimatch) {
            multimatches.push_back(std::move(match));
            continue;
        }

        const auto pit = best_ratio_primary.find(match.primary);
        if (pit != best_ratio_primary.end() && pit->second > match.ratio) continue;

        const auto sit = best_ratio_secondary.find(match.secondary);
        if (sit != best_ratio_secondary.end() && sit->second > match.ratio) continue;

        best_ratio_primary[match.primary] = match.ratio;
        best_ratio_secondary[match.secondary] = match.ratio;
        cleaned.push_back(std::move(match));
    }

    matched_primary.clear();
    matched_secondary.clear();
    int line = 0;
    for (auto& m : cleaned) {
        m.line = line++;
        matched_primary.insert(m.primary);
        matched_secondary.insert(m.secondary);
    }
    for (auto& m : multimatches) {
        m.line = line++;
        cleaned.push_back(std::move(m));
    }
    matches = std::move(cleaned);
}

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
        std::size_t seed = std::hash<Address>{}(key.primary);
        seed ^= std::hash<Address>{}(key.secondary) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        return seed;
    }
};

void resolve_multimatches(std::vector<db::ResultMatch>& matches)
{
    boost::unordered_flat_map<Address, double> max_ratio_primary;
    boost::unordered_flat_map<Address, double> max_ratio_secondary;
    boost::unordered_flat_map<Address, std::size_t> max_count_primary;
    boost::unordered_flat_map<Address, std::size_t> max_count_secondary;

    for (const auto& m : matches) {
        if (m.kind == db::ResultKind::multimatch) continue;
        auto it = max_ratio_primary.find(m.primary);
        if (it == max_ratio_primary.end() || m.ratio > it->second) {
            max_ratio_primary[m.primary] = m.ratio;
            max_count_primary[m.primary] = 1;
        } else if (m.ratio == it->second) {
            ++max_count_primary[m.primary];
        }
        auto it2 = max_ratio_secondary.find(m.secondary);
        if (it2 == max_ratio_secondary.end() || m.ratio > it2->second) {
            max_ratio_secondary[m.secondary] = m.ratio;
            max_count_secondary[m.secondary] = 1;
        } else if (m.ratio == it2->second) {
            ++max_count_secondary[m.secondary];
        }
    }

    for (auto& m : matches) {
        if (m.kind == db::ResultKind::multimatch) continue;
        const auto pit = max_ratio_primary.find(m.primary);
        const auto sit = max_ratio_secondary.find(m.secondary);
        const bool primary_conflict = pit != max_ratio_primary.end()
            && (m.ratio < pit->second
                || (m.ratio == pit->second && max_count_primary[m.primary] > 1));
        const bool secondary_conflict = sit != max_ratio_secondary.end()
            && (m.ratio < sit->second
                || (m.ratio == sit->second && max_count_secondary[m.secondary] > 1));
        if (primary_conflict || secondary_conflict) {
            m.kind = db::ResultKind::multimatch;
        }
    }
}

void append_unmatched(
    db::DiffResultSet& results,
    const std::vector<db::QueryRow>& rows,
    const boost::unordered_flat_set<Address>& covered,
    db::UnmatchedKind kind)
{
    int line = 0;
    for (const auto& row : rows) {
        if (row.size() < 2) {
            continue;
        }

        const auto address = parse_address(row[0]);
        if (covered.find(address) != covered.end()) {
            continue;
        }

        db::UnmatchedFunction unmatched;
        unmatched.kind = kind;
        unmatched.line = line++;
        unmatched.address = address;
        unmatched.name = row[1];
        results.unmatched.push_back(std::move(unmatched));
    }
}

void final_pass_unmatched(db::Database& database, db::DiffResultSet& results)
{
    boost::unordered_flat_set<Address> covered_primary;
    boost::unordered_flat_set<Address> covered_secondary;
    for (const auto& match : results.matches) {
        covered_primary.insert(match.primary);
        covered_secondary.insert(match.secondary);
    }

    append_unmatched(
        results,
        database.query_rows("select address, name from functions order by id"),
        covered_primary,
        db::UnmatchedKind::primary);
    append_unmatched(
        results,
        database.query_rows("select address, name from diff.functions order by id"),
        covered_secondary,
        db::UnmatchedKind::secondary);
}

int result_rank(db::ResultKind kind)
{
    switch (kind) {
    case db::ResultKind::best:
        return 0;
    case db::ResultKind::partial:
        return 1;
    case db::ResultKind::unreliable:
        return 2;
    case db::ResultKind::multimatch:
        return 3;
    }
    return 4;
}

void cleanup_result_order(db::DiffResultSet& results)
{
    std::stable_sort(results.matches.begin(), results.matches.end(), [](const auto& left, const auto& right) {
        const auto left_rank = result_rank(left.kind);
        const auto right_rank = result_rank(right.kind);
        if (left_rank != right_rank) {
            return left_rank < right_rank;
        }
        if (left.ratio != right.ratio) {
            return left.ratio > right.ratio;
        }
        if (left.primary != right.primary) {
            return left.primary < right.primary;
        }
        return left.secondary < right.secondary;
    });

    int line = 0;
    for (auto& match : results.matches) {
        match.line = line++;
    }
}

std::vector<db::HeuristicStat> convert_stats(const std::vector<SqlHeuristicStats>& stats)
{
    std::vector<db::HeuristicStat> out;
    out.reserve(stats.size());
    int line = 0;
    for (const auto& item : stats) {
        db::HeuristicStat stored;
        stored.line = line++;
        stored.name = item.name;
        stored.candidates = item.candidates;
        stored.accepted = item.accepted;
        stored.rejected = item.rejected;
        stored.skipped = item.skipped;
        stored.multimatches = item.multimatches;
        stored.row_limit_hit = item.row_limit_hit;
        stored.timeout_hit = item.timeout_hit;
        stored.cancelled = item.cancelled;
        out.push_back(std::move(stored));
    }
    return out;
}

} // namespace

void resolve_multimatches_for_testing(std::vector<db::ResultMatch>& matches)
{
    resolve_multimatches(matches);
}

DiffSession::DiffSession(DiffSessionOptions options)
    : options_(std::move(options))
{
}

namespace {

std::vector<db::ResultMatch> find_equal_matches(db::Database& database)
{
    std::vector<db::ResultMatch> matches;
    const auto sql =
        "select f.address, f.name, f.nodes "
        "from (select address, name, mangled_function, nodes, edges, size, bytes_hash "
        "from functions "
        "intersect "
        "select address, name, mangled_function, nodes, edges, size, bytes_hash "
        "from diff.functions) f";

    int line = 0;
    for (const auto& row : database.query_rows(sql)) {
        if (row.size() < 3) continue;
        Address addr = 0;
        const auto* begin = row[0].data();
        const auto* end = row[0].data() + row[0].size();
        std::from_chars(begin, end, addr, 10);

        db::ResultMatch match;
        match.kind = db::ResultKind::best;
        match.line = line++;
        match.primary = addr;
        match.primary_name = row[1];
        match.secondary = addr;
        match.secondary_name = row[1];
        match.ratio = 1.0;
        int nodes = 0;
        if (!row[2].empty()) {
            try { nodes = std::stoi(row[2]); } catch (...) {}
        }
        match.primary_nodes = nodes;
        match.secondary_nodes = nodes;
        match.description = "Equal match";
        matches.push_back(std::move(match));
    }
    return matches;
}

std::vector<db::ResultMatch> find_md_index_matches(
    const FunctionCache& primary_cache,
    const FunctionCache& secondary_cache,
    boost::unordered_flat_set<Address>& matched_primary,
    boost::unordered_flat_set<Address>& matched_secondary)
{
    boost::unordered_flat_map<std::string, Address> primary_by_md;
    boost::unordered_flat_set<std::string> duplicate_md;
    for (const auto& [address, function] : primary_cache.by_address) {
        if (function.md_index.empty() || function.nodes < 3) continue;
        auto [it, inserted] = primary_by_md.emplace(function.md_index, address);
        if (!inserted) duplicate_md.emplace(function.md_index);
    }
    std::vector<db::ResultMatch> matches;
    for (const auto& [secondary_address, secondary] : secondary_cache.by_address) {
        if (secondary.md_index.empty() || duplicate_md.find(secondary.md_index) != duplicate_md.end()) continue;
        const auto primary = primary_by_md.find(secondary.md_index);
        if (primary == primary_by_md.end()) continue;
        const auto primary_function = primary_cache.by_address.find(primary->second);
        if (primary_function == primary_cache.by_address.end()) continue;
        const auto& left = primary_function->second;
        if (left.nodes > secondary.nodes + 1 || secondary.nodes > left.nodes + 1) continue;
        if (matched_primary.count(primary->second) || matched_secondary.count(secondary_address)) continue;
        db::ResultMatch match;
        match.kind = db::ResultKind::best;
        match.primary = primary->second;
        match.primary_name = left.name;
        match.secondary = secondary_address;
        match.secondary_name = secondary.name;
        match.ratio = 1.0;
        match.primary_nodes = left.nodes;
        match.secondary_nodes = secondary.nodes;
        match.description = "Same unique MD index";
        matches.push_back(std::move(match));
        matched_primary.insert(primary->second);
        matched_secondary.insert(secondary_address);
    }
    return matches;
}

DiffSessionSummary run_session(
    const DiffSessionOptions& options,
    const std::filesystem::path& main_db,
    const std::filesystem::path& diff_db,
    const std::filesystem::path& output_db,
    bool exact_only)
{
    validate_output_path(main_db, diff_db, output_db);
    if (!std::isfinite(options.fixed_point_min_delta_ratio) || options.fixed_point_min_delta_ratio < 0.0) {
        throw std::invalid_argument("fixed_point_min_delta_ratio must be finite and non-negative");
    }

    db::Database database;
    database.open(main_db);

    SnapshotRepository snapshot_repository;
    snapshot_repository.attach_diff(database, diff_db);

    auto primary_cache = load_function_cache(database, "main");
    auto secondary_cache = load_function_cache(database, "diff");

    auto sql_options = options.sql;
    sql_options.primary_cache = &primary_cache;
    sql_options.secondary_cache = &secondary_cache;
    if (options.auto_detect_same_processor) {
        sql_options.same_processor = detect_same_processor(database);
    }
    if (options.hooks != nullptr) {
        sql_options.hooks = options.hooks;
    }

    // Auto-tuning: disable slow heuristics for large databases
    const auto primary_count = database.query_int("select count(*) from functions");
    const auto secondary_count = database.query_int("select count(*) from diff.functions");
    const auto total_functions = std::max<std::size_t>(
        1, static_cast<std::size_t>(primary_count + secondary_count));
    if (total_functions >= options.config.min_functions_to_disable_slow) {
        sql_options.enable_slow = false;
    }
    if (total_functions >= options.config.min_functions_to_consider_medium) {
        sql_options.enable_relaxed_ratio = true;
    }

    SqlHeuristicRunner runner;
    auto equal_matches = find_equal_matches(database);

    // Fast path: stripped binaries (99%+ address+hash match)
    bool is_stripped_fast_path = false;
    if (!exact_only && !equal_matches.empty()) {
        const auto total_primary = database.query_int(
            "select count(*) from functions");
        const auto total_secondary = database.query_int(
            "select count(*) from diff.functions");
        if (total_primary > 0 && total_secondary > 0) {
            // Count functions with same address AND same bytes_hash (not just address)
            const auto address_hash_matches = database.query_int(
                "select count(*) from functions f, diff.functions df "
                "where f.address = df.address and f.bytes_hash = df.bytes_hash "
                "and f.bytes_hash != '' and f.bytes_hash is not null");
            const auto min_total = std::min(total_primary, total_secondary);
            if (min_total > 0) {
                const double percent = 100.0 * static_cast<double>(address_hash_matches)
                    / static_cast<double>(min_total);
                if (percent >= options.config.speedup_stripped_binaries_min_percent) {
                    is_stripped_fast_path = true;
                    equal_matches.clear();
                    const auto rows = database.query_rows(
                        "select f.address, f.name, f.nodes from functions f, diff.functions df "
                        "where f.address = df.address and f.bytes_hash = df.bytes_hash "
                        "and f.bytes_hash != '' and f.bytes_hash is not null");
                    int line = 0;
                    for (const auto& row : rows) {
                        if (row.size() < 3) continue;
                        Address addr = 0;
                        std::from_chars(row[0].data(), row[0].data() + row[0].size(), addr, 10);
                        db::ResultMatch m;
                        m.kind = db::ResultKind::best;
                        m.line = line++;
                        m.primary = addr;
                        m.primary_name = row[1];
                        m.secondary = addr;
                        m.secondary_name = row[1];
                        m.ratio = 1.0;
                        int nodes = 0;
                        try { nodes = std::stoi(row[2]); } catch (...) {}
                        m.primary_nodes = nodes;
                        m.secondary_nodes = nodes;
                        m.description = "Stripped binary (address+hash match)";
                        equal_matches.push_back(std::move(m));
                    }
                }
            }
        }
    }

    if (!exact_only && !equal_matches.empty()) {
        for (const auto& m : equal_matches) {
            sql_options.pre_matched_primary.insert(m.primary);
            sql_options.pre_matched_secondary.insert(m.secondary);
        }
    }

    // Pre-heuristic pass: find_same_name to lock in named function matches early
    std::vector<db::ResultMatch> pre_same_name_matches;
    boost::unordered_flat_set<Address> pre_matched_p = sql_options.pre_matched_primary;
    boost::unordered_flat_set<Address> pre_matched_s = sql_options.pre_matched_secondary;
    if (!exact_only && !is_stripped_fast_path) {
        find_same_name(database, pre_same_name_matches, pre_matched_p, pre_matched_s,
            options.propagation.same_name_min_ratio, sql_options.same_processor,
            &primary_cache, &secondary_cache);
        for (const auto& m : pre_same_name_matches) {
            sql_options.pre_matched_primary.insert(m.primary);
            sql_options.pre_matched_secondary.insert(m.secondary);
        }
    }

    // Patchdiff-with-symbols fast path: if >90% matched by name, skip heuristics
    bool is_patchdiff_fast_path = false;
    if (!exact_only && !is_stripped_fast_path && !pre_same_name_matches.empty()) {
        const auto min_total = std::min(primary_count, secondary_count);
        if (min_total > 0) {
            const double percent = 100.0 * static_cast<double>(pre_same_name_matches.size())
                / static_cast<double>(min_total);
            if (percent >= 90.0) {
                is_patchdiff_fast_path = true;
            }
        }
    }

    db::DiffResultSet results;
    results.main_db = main_db.string();
    results.diff_db = diff_db.string();

    if (!exact_only) {
        auto md_matches = find_md_index_matches(primary_cache, secondary_cache, pre_matched_p, pre_matched_s);
        results.matches.insert(results.matches.end(), md_matches.begin(), md_matches.end());
        soff::perf::add_counter("diff.md_index_fast_path.matches", md_matches.size());
        sql_options.pre_matched_primary = pre_matched_p;
        sql_options.pre_matched_secondary = pre_matched_s;
    }

    SqlHeuristicRunResult run_result;
    if (is_stripped_fast_path || is_patchdiff_fast_path) {
        // Skip SQL heuristics entirely
    } else {
        run_result = exact_only
            ? runner.run_exact(database, sql_options)
            : runner.run_all(database, sql_options);
    }

    if (!exact_only) {
        results.matches.insert(results.matches.end(),
            equal_matches.begin(), equal_matches.end());
        results.matches.insert(results.matches.end(),
            pre_same_name_matches.begin(), pre_same_name_matches.end());
        results.matches.insert(results.matches.end(),
            run_result.matches.begin(), run_result.matches.end());
    } else {
        results.matches = run_result.matches;
    }
    results.heuristic_stats = convert_stats(run_result.stats);

    boost::unordered_flat_set<Address> matched_primary;
    boost::unordered_flat_set<Address> matched_secondary;
    for (const auto& m : results.matches) {
        matched_primary.insert(m.primary);
        matched_secondary.insert(m.secondary);
    }

    if (!exact_only) {
        cleanup_matches(results.matches, matched_primary, matched_secondary);
    }

    // Fixed-point iteration: heuristics -> propagation -> repeat until convergence
    PropagationStats propagation_stats;
    const int max_fixed_point_iterations = std::max(0, options.max_fixed_point_iterations);
    for (int fp_iter = 0; fp_iter < max_fixed_point_iterations && !exact_only && options.propagation.enabled; ++fp_iter) {
        const auto before_propagation_count = results.matches.size();

        auto prop_options = options.propagation;
        prop_options.same_processor = sql_options.same_processor;
        prop_options.enable_slow = sql_options.enable_slow;
        prop_options.primary_cache = &primary_cache;
        prop_options.secondary_cache = &secondary_cache;
        const auto round_stats = run_propagation(
            database, results.matches,
            matched_primary, matched_secondary, prop_options);
        propagation_stats.same_name_matches += round_stats.same_name_matches;
        propagation_stats.affine_matches += round_stats.affine_matches;
        propagation_stats.diffing_matches += round_stats.diffing_matches;
        propagation_stats.related_constants_matches += round_stats.related_constants_matches;
        propagation_stats.compilation_unit_matches += round_stats.compilation_unit_matches;
        propagation_stats.call_reference_matches += round_stats.call_reference_matches;
        propagation_stats.iterations_run += round_stats.iterations_run;
        cleanup_matches(results.matches, matched_primary, matched_secondary);

        const auto matches_after = results.matches.size();
        const auto propagation_delta = matches_after > before_propagation_count
            ? matches_after - before_propagation_count
            : std::size_t{0};
        if (propagation_delta == 0) break;

        const double delta_ratio = static_cast<double>(propagation_delta)
            / static_cast<double>(total_functions);

        // Propagation found new matches; re-run heuristics with updated exclusions
        if (fp_iter < max_fixed_point_iterations - 1 && !is_stripped_fast_path && !is_patchdiff_fast_path) {
            if (delta_ratio < options.fixed_point_min_delta_ratio) {
                soff::perf::add_counter("diff.fixed_point.skip_small_delta");
                break;
            }
            soff::perf::add_counter("diff.fixed_point.rerun");
            sql_options.pre_matched_primary = matched_primary;
            sql_options.pre_matched_secondary = matched_secondary;
            auto rerun = runner.run_all(database, sql_options);
            for (auto& m : rerun.matches) {
                if (matched_primary.count(m.primary) || matched_secondary.count(m.secondary)) continue;
                matched_primary.insert(m.primary);
                matched_secondary.insert(m.secondary);
                results.matches.push_back(std::move(m));
            }
            cleanup_matches(results.matches, matched_primary, matched_secondary);
        }
    }

    resolve_multimatches(results.matches);

    // BB-level structural similarity refinement for partial matches
    // Blends text-based ratio with structural BB match quality
    boost::unordered_flat_map<AddressPairKey, BasicBlockMatchResult, AddressPairHash> bb_cache;
    for (auto& match : results.matches) {
        if (match.kind != db::ResultKind::partial && match.kind != db::ResultKind::unreliable) {
            continue;
        }
        if (match.ratio < 0.30) {
            soff::perf::add_counter("diff.bb_matching.skip_low_ratio");
            continue;
        }
        if (match.kind == db::ResultKind::unreliable && match.ratio >= 0.95) {
            soff::perf::add_counter("diff.bb_matching.skip_high_unreliable");
            continue;
        }
        if (match.primary_nodes < 3 || match.secondary_nodes < 3) {
            continue;
        }
        try {
            soff::perf::add_counter("diff.bb_matching.attempt");
            const AddressPairKey key{match.primary, match.secondary};
            auto cached = bb_cache.find(key);
            if (cached == bb_cache.end()) {
                soff::perf::add_counter("diff.bb_matching.cache_miss");
                cached = bb_cache.emplace(key, match_basic_blocks(database, match.primary, match.secondary)).first;
            } else {
                soff::perf::add_counter("diff.bb_matching.cache_hit");
            }
            const auto& bb_result = cached->second;
            if (bb_result.matches.empty()) continue;
            const double structural = bb_result.similarity();
            // Blend: 60% text ratio + 40% structural similarity
            const double blended = 0.6 * match.ratio + 0.4 * structural;
            match.ratio = blended;
            // Structural evidence adjusts confidence but does not promote a partial
            // match to best without independent exact evidence.
            match.description += " [BB structural=" + std::to_string(structural) + "]";
        } catch (const std::bad_alloc&) {
            throw;
        } catch (...) {
            soff::perf::add_counter("diff.bb_matching.exception");
            // BB data may not be available; skip silently
        }
    }

    // ML model post-filter: reject low-confidence partial/unreliable matches
    if (!options.ml_model_path.empty() && std::filesystem::exists(options.ml_model_path)) {
        auto model = MlModel::load(options.ml_model_path);
        model.filter_matches(database, results.matches, matched_primary, matched_secondary);
    }

    final_pass_unmatched(database, results);
    cleanup_result_order(results);

    if (options.hooks != nullptr) {
        options.hooks->on_finish();
    }
    db::ResultRepository result_repository;
    result_repository.save(results, output_db);

    DiffSessionSummary summary;
    summary.main_db = main_db;
    summary.diff_db = diff_db;
    summary.output_db = output_db;
    summary.same_processor = sql_options.same_processor;
    summary.heuristics = run_result.stats.size();
    summary.propagation = propagation_stats;
    summary.results = result_repository.summarize(output_db);
    for (const auto& stats : run_result.stats) {
        summary.candidates += stats.candidates;
        summary.accepted += stats.accepted;
        summary.multimatches += stats.multimatches;
        if (stats.row_limit_hit) {
            ++summary.row_limited_heuristics;
        }
        if (stats.timeout_hit) {
            ++summary.timed_out_heuristics;
        }
        if (stats.cancelled) {
            ++summary.cancelled_heuristics;
        }
    }
    return summary;
}

} // namespace

DiffSessionSummary DiffSession::run_exact(
    const std::filesystem::path& main_db,
    const std::filesystem::path& diff_db,
    const std::filesystem::path& output_db) const
{
    soff::perf::ScopedTimer timer("diff.run_exact");
    return run_session(options_, main_db, diff_db, output_db, true);
}

DiffSessionSummary DiffSession::run_all(
    const std::filesystem::path& main_db,
    const std::filesystem::path& diff_db,
    const std::filesystem::path& output_db) const
{
    soff::perf::ScopedTimer timer("diff.run_all");
    return run_session(options_, main_db, diff_db, output_db, false);
}

} // namespace soff::diff
