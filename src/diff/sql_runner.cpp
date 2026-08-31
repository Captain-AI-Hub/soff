#include "soff/diff/sql_runner.hpp"

#include "soff/core/hooks.hpp"
#include "soff/core/perf.hpp"
#include "soff/core/thread_pool.hpp"
#include "soff/diff/heuristics.hpp"
#include "soff/diff/matching_assignment.hpp"
#include "soff/diff/ratio.hpp"

#include <charconv>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <iterator>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <boost/unordered/unordered_flat_set.hpp>

namespace soff::diff {
namespace {

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

std::string wrap_with_limit(const std::string& sql, std::size_t limit)
{
    if (limit == 0) {
        return sql;
    }

    std::ostringstream stream;
    stream << "select * from (" << sql << ") limit " << (limit + 1);
    return stream.str();
}

std::vector<db::QueryRow> query_with_controls(
    db::Database& database,
    const std::string& sql,
    const SqlRunnerOptions& options,
    SqlHeuristicStats& stats)
{
    const bool has_cancel = static_cast<bool>(options.cancel_requested);
    const bool has_timeout = options.timeout_seconds > 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(options.timeout_seconds);

    if (has_cancel || has_timeout) {
        const auto progress_interval = std::max(1, options.progress_check_interval);
        database.set_progress_handler(progress_interval, [&]() {
            if (has_cancel && options.cancel_requested()) {
                stats.cancelled = true;
                return true;
            }
            if (has_timeout && std::chrono::steady_clock::now() >= deadline) {
                stats.timeout_hit = true;
                return true;
            }
            return false;
        });
    }

    try {
        soff::perf::ScopedTimer timer("sql.query");
        auto rows = database.query_rows(sql);
        database.clear_progress_handler();
        return rows;
    } catch (...) {
        database.clear_progress_handler();
        if (stats.cancelled || stats.timeout_hit) {
            return {};
        }
        throw;
    }
}

struct CandidateRow
{
    Address primary = 0;
    std::string primary_name;
    Address secondary = 0;
    std::string secondary_name;
    std::string description;
    std::string pseudo1;
    std::string pseudo2;
    std::string asm1;
    std::string asm2;
    int primary_nodes = 0;
    int secondary_nodes = 0;
    std::string pseudo_primes1;
    std::string pseudo_primes2;
    double md1 = 0.0;
    double md2 = 0.0;
    std::string stripped_assembly1;
    std::string stripped_assembly2;
    std::string stripped_pseudocode1;
    std::string stripped_pseudocode2;
    std::string stripped_micro1;
    std::string stripped_micro2;
    std::string bytes_hash1;
    std::string bytes_hash2;
    int primary_edges = 0;
    int secondary_edges = 0;
    int primary_indegree = 0;
    int secondary_indegree = 0;
    int primary_outdegree = 0;
    int secondary_outdegree = 0;
    int primary_instructions = 0;
    int secondary_instructions = 0;
    int primary_cc = 0;
    int secondary_cc = 0;
    int primary_strongly_connected = 0;
    int secondary_strongly_connected = 0;
    int primary_loops = 0;
    int secondary_loops = 0;
    int primary_constants_count = 0;
    int secondary_constants_count = 0;
    int primary_size = 0;
    int secondary_size = 0;
    std::string kgh_hash1;
    std::string kgh_hash2;
};

int parse_int_or_zero(const std::string& text)
{
    if (text.empty()) {
        return 0;
    }
    return std::stoi(text);
}

double parse_double_or_zero(const std::string& text)
{
    if (text.empty()) {
        return 0.0;
    }
    return std::stod(text);
}

CandidateRow parse_candidate(const db::QueryRow& row)
{
    if (row.size() < 45) {
        throw std::runtime_error("heuristic row does not contain SELECT_FIELDS columns");
    }

    CandidateRow candidate;
    candidate.primary = parse_address(row[0]);
    candidate.primary_name = row[1];
    candidate.secondary = parse_address(row[2]);
    candidate.secondary_name = row[3];
    candidate.description = row[4];
    candidate.pseudo1 = row[5];
    candidate.pseudo2 = row[6];
    candidate.asm1 = row[7];
    candidate.asm2 = row[8];
    candidate.pseudo_primes1 = row[9];
    candidate.pseudo_primes2 = row[10];
    candidate.primary_nodes = parse_int_or_zero(row[11]);
    candidate.secondary_nodes = parse_int_or_zero(row[12]);
    candidate.md1 = parse_double_or_zero(row[13]);
    candidate.md2 = parse_double_or_zero(row[14]);
    candidate.stripped_assembly1 = row[15];
    candidate.stripped_assembly2 = row[16];
    candidate.stripped_pseudocode1 = row[17];
    candidate.stripped_pseudocode2 = row[18];
    candidate.stripped_micro1 = row[21];
    candidate.stripped_micro2 = row[22];
    candidate.bytes_hash1 = row[23];
    candidate.bytes_hash2 = row[24];
    candidate.primary_edges = parse_int_or_zero(row[25]);
    candidate.secondary_edges = parse_int_or_zero(row[26]);
    candidate.primary_indegree = parse_int_or_zero(row[27]);
    candidate.secondary_indegree = parse_int_or_zero(row[28]);
    candidate.primary_outdegree = parse_int_or_zero(row[29]);
    candidate.secondary_outdegree = parse_int_or_zero(row[30]);
    candidate.primary_instructions = parse_int_or_zero(row[31]);
    candidate.secondary_instructions = parse_int_or_zero(row[32]);
    candidate.primary_cc = parse_int_or_zero(row[33]);
    candidate.secondary_cc = parse_int_or_zero(row[34]);
    candidate.primary_strongly_connected = parse_int_or_zero(row[35]);
    candidate.secondary_strongly_connected = parse_int_or_zero(row[36]);
    candidate.primary_loops = parse_int_or_zero(row[37]);
    candidate.secondary_loops = parse_int_or_zero(row[38]);
    candidate.primary_constants_count = parse_int_or_zero(row[39]);
    candidate.secondary_constants_count = parse_int_or_zero(row[40]);
    candidate.primary_size = parse_int_or_zero(row[41]);
    candidate.secondary_size = parse_int_or_zero(row[42]);
    candidate.kgh_hash1 = row[43];
    candidate.kgh_hash2 = row[44];
    return candidate;
}

double equal_non_empty_ratio(const std::string& left, const std::string& right)
{
    return !left.empty() && left == right ? 1.0 : 0.0;
}

std::vector<std::string> parse_jsonish_array_values(const std::string& text)
{
    std::vector<std::string> values;
    std::string current;
    bool in_string = false;
    for (const auto ch : text) {
        if (ch == '"') {
            in_string = !in_string;
            continue;
        }
        if (!in_string && (ch == '[' || ch == ']' || ch == ' ')) {
            continue;
        }
        if (!in_string && ch == ',') {
            if (!current.empty()) {
                values.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(ch);
    }
    if (!current.empty()) {
        values.push_back(current);
    }
    return values;
}

std::size_t intersection_size(std::vector<std::string> left, std::vector<std::string> right)
{
    std::sort(left.begin(), left.end());
    std::sort(right.begin(), right.end());
    std::vector<std::string> out;
    std::set_intersection(left.begin(), left.end(), right.begin(), right.end(), std::back_inserter(out));
    return out.size();
}

double deep_ratio_bonus(db::Database& database, const CandidateRow& candidate, const SqlRunnerOptions& options)
{
    if (options.primary_cache != nullptr && options.secondary_cache != nullptr) {
        const auto primary = options.primary_cache->by_address.find(candidate.primary);
        const auto secondary = options.secondary_cache->by_address.find(candidate.secondary);
        if (primary != options.primary_cache->by_address.end()
            && secondary != options.secondary_cache->by_address.end()) {
            soff::perf::add_counter("ratio.deep_bonus.cache_hit");
            const auto& left = primary->second;
            const auto& right = secondary->second;
            // Same magnitudes as the SQL fallback below (Diaphora deep_ratio
            // parity): tiny nudges that only break multimatch ties.
            double bonus = 0.0;
            if (!left.source_file.empty() && left.source_file == right.source_file) bonus += 0.001;
            if (!left.pseudocode_primes.empty() && left.pseudocode_primes == right.pseudocode_primes) bonus += 0.001;
            if (candidate.primary_indegree == candidate.secondary_indegree && candidate.primary_indegree != 0) bonus += 0.001;
            if (candidate.primary_outdegree == candidate.secondary_outdegree && candidate.primary_outdegree != 0) bonus += 0.001;
            if (candidate.primary_cc == candidate.secondary_cc && candidate.primary_cc != 0) bonus += 0.001;
            if (!left.constants.empty() && left.constants != "[]" && !right.constants.empty()) {
                const auto common = intersection_size(
                    parse_jsonish_array_values(left.constants),
                    parse_jsonish_array_values(right.constants));
                bonus += static_cast<double>(common) * (options.same_processor ? 0.006 : 0.008);
            }
            return std::min(0.20, bonus);
        }
    }
    soff::perf::add_counter("ratio.deep_bonus.sql_fallback");

    const auto main_rows = database.query_rows(
        "select source_file, switches, constants "
        "from functions where address = '" + std::to_string(candidate.primary) + "' limit 1");
    const auto diff_rows = database.query_rows(
        "select source_file, switches, constants "
        "from diff.functions where address = '" + std::to_string(candidate.secondary) + "' limit 1");
    if (main_rows.empty() || diff_rows.empty()) {
        return 0.0;
    }

    const auto& left = main_rows.front();
    const auto& right = diff_rows.front();
    double score = 0.0;
    // source_file
    if (!left[0].empty() && left[0] == right[0]) {
        score += 0.001;
    }
    // pseudocode_primes
    if (!candidate.pseudo_primes1.empty() && candidate.pseudo_primes1 == candidate.pseudo_primes2) {
        score += 0.001;
    }
    // indegree
    if (candidate.primary_indegree == candidate.secondary_indegree && candidate.primary_indegree != 0) {
        score += 0.001;
    }
    // outdegree
    if (candidate.primary_outdegree == candidate.secondary_outdegree && candidate.primary_outdegree != 0) {
        score += 0.001;
    }
    // switches
    if (left[1] == right[1] && left[1] != "[]" && !left[1].empty()) {
        score += 0.003;
    }
    // cyclomatic_complexity
    if (candidate.primary_cc == candidate.secondary_cc && candidate.primary_cc != 0) {
        score += 0.001;
    }
    // constants: per-constant accumulation (same_cpu: +0.006, diff_cpu: +0.008)
    if (left[2] != "[]" && !left[2].empty() && !right[2].empty()) {
        const auto left_consts = parse_jsonish_array_values(left[2]);
        const auto right_consts = parse_jsonish_array_values(right[2]);
        const auto common = intersection_size(left_consts, right_consts);
        score += static_cast<double>(common) * (options.same_processor ? 0.006 : 0.008);
    }
    return std::min(0.20, score);
}

double compute_ratio_fast(const CandidateRow& candidate)
{
    if (equal_non_empty_ratio(candidate.bytes_hash1, candidate.bytes_hash2) == 1.0) {
        return 1.0;
    }

    const double v1 = !candidate.pseudo1.empty() && !candidate.pseudo2.empty()
        ? candidate_text_ratio("", "", "", "", "", "", candidate.stripped_pseudocode1, candidate.stripped_pseudocode2)
        : 0.0;
    const double v2 = candidate_text_ratio("", "", "", "", candidate.stripped_assembly1, candidate.stripped_assembly2, "", "");

    // v3: AST prime difference ratio
    double v3 = 0.0;
    if (!candidate.pseudo_primes1.empty() && !candidate.pseudo_primes2.empty()) {
        v3 = ast_prime_difference_ratio(candidate.pseudo_primes1, candidate.pseudo_primes2);
    }

    double v4 = 0.0;
    if (candidate.md1 == candidate.md2 && candidate.md1 > 0.0) {
        // Diaphora parity: a shared MD index lifts the average considerably.
        v4 = std::min((v1 + v2 + v3 + 3.0) / 5.0, 1.0);
    }
    const double v5 = candidate_text_ratio("", "", "", "", candidate.stripped_micro1, candidate.stripped_micro2, "", "");
    if (v5 == 1.0) {
        return 1.0;
    }

    double ratio = std::max({v1, v2, v3, v4, v5});
    if (ratio == 1.0 && candidate.md1 != candidate.md2) {
        ratio = 0.0;
        for (const auto value : {v1, v2, v3, v4, v5}) {
            if (value != 1.0 && value > ratio) {
                ratio = value;
            }
        }
    }
    return ratio;
}

double minimum_ratio_for(const HeuristicDefinition& heuristic, const SqlRunnerOptions& options)
{
    switch (heuristic.ratio_mode) {
    case RatioMode::no_false_positives:
        return 1.0;
    case RatioMode::ratio:
        return options.default_partial_ratio;
    case RatioMode::ratio_with_minimum:
        return heuristic.minimum_ratio;
    case RatioMode::trusted_ratio_with_minimum:
        return heuristic.minimum_ratio > 0.0 ? heuristic.minimum_ratio : options.trusted_partial_ratio;
    }
    return options.default_partial_ratio;
}

bool should_run(const HeuristicDefinition& heuristic, const SqlRunnerOptions& options)
{
    if (heuristic.category == HeuristicCategory::experimental && !options.enable_experimental) {
        return false;
    }
    if (is_unreliable(heuristic) && !options.enable_unreliable) {
        return false;
    }
    if (supports_same_cpu_only(heuristic) && !options.same_processor) {
        return false;
    }
    if (is_slow(heuristic) && !options.enable_slow) {
        return false;
    }
    return true;
}

enum class AppendOutcome
{
    skipped,
    accepted,
    multimatch,
};

std::string match_pair_key(Address primary, Address secondary)
{
    return std::to_string(primary) + ":" + std::to_string(secondary);
}

db::ResultKind result_kind_for(const HeuristicDefinition& heuristic, double ratio)
{
    if (ratio >= 1.0) {
        return db::ResultKind::best;
    }
    if (heuristic.ratio_mode != RatioMode::no_false_positives
        && is_unreliable(heuristic)) {
        return db::ResultKind::unreliable;
    }
    return db::ResultKind::partial;
}

// Everything append_match/append_forced_multimatch and the assignment need
// from a candidate once its ratio is computed. The text columns of
// CandidateRow are deliberately dropped: on huge databases the full
// pseudocode/assembly texts of an entire heuristic candidate set are tens of
// GB and previously OOM-killed the process.
struct SlimCandidate
{
    Address primary = 0;
    std::string primary_name;
    Address secondary = 0;
    std::string secondary_name;
    std::string description;
    double ratio = 0.0;
    int primary_nodes = 0;
    int secondary_nodes = 0;
};

SlimCandidate slim_candidate(const CandidateRow& candidate, double ratio)
{
    SlimCandidate slim;
    slim.primary = candidate.primary;
    slim.primary_name = candidate.primary_name;
    slim.secondary = candidate.secondary;
    slim.secondary_name = candidate.secondary_name;
    slim.description = candidate.description;
    slim.ratio = ratio;
    slim.primary_nodes = candidate.primary_nodes;
    slim.secondary_nodes = candidate.secondary_nodes;
    return slim;
}

AppendOutcome append_match(
    SqlHeuristicRunResult& result,
    const SlimCandidate& candidate,
    const HeuristicDefinition& heuristic,
    double ratio,
    int& line,
    boost::unordered_flat_set<Address>& matched_primary,
    boost::unordered_flat_set<Address>& matched_secondary,
    boost::unordered_flat_set<std::string>& emitted_pairs)
{
    // Validation 1: reject nullsub functions
    if (candidate.primary_name.substr(0, 8) == "nullsub_"
        || candidate.secondary_name.substr(0, 8) == "nullsub_") {
        return AppendOutcome::skipped;
    }

    const auto pair_key = match_pair_key(candidate.primary, candidate.secondary);
    if (emitted_pairs.find(pair_key) != emitted_pairs.end()) {
        return AppendOutcome::skipped;
    }

    const bool primary_matched = matched_primary.find(candidate.primary) != matched_primary.end();
    const bool secondary_matched = matched_secondary.find(candidate.secondary) != matched_secondary.end();

    // Validation 2: reject if both sides already matched (has_best_match equivalent)
    if (primary_matched && secondary_matched) {
        return AppendOutcome::skipped;
    }

    // Validation 3: if one side matched and ratio < 1.0, skip (has_better_match for exact)
    if ((primary_matched || secondary_matched) && ratio < 1.0) {
        return AppendOutcome::skipped;
    }

    db::ResultMatch match;
    if (primary_matched || secondary_matched) {
        match.kind = db::ResultKind::multimatch;
    } else {
        match.kind = result_kind_for(heuristic, ratio);
    }

    match.line = line++;
    match.primary = candidate.primary;
    match.primary_name = candidate.primary_name;
    match.secondary = candidate.secondary;
    match.secondary_name = candidate.secondary_name;
    match.ratio = ratio;
    match.primary_nodes = candidate.primary_nodes;
    match.secondary_nodes = candidate.secondary_nodes;
    match.description = candidate.description;
    result.matches.push_back(std::move(match));
    emitted_pairs.insert(pair_key);

    if (primary_matched || secondary_matched) {
        return AppendOutcome::multimatch;
    }

    matched_primary.insert(candidate.primary);
    matched_secondary.insert(candidate.secondary);
    return AppendOutcome::accepted;
}


AppendOutcome append_forced_multimatch(
    SqlHeuristicRunResult& result,
    const SlimCandidate& candidate,
    double ratio,
    int& line,
    boost::unordered_flat_set<std::string>& emitted_pairs)
{
    const auto pair_key = match_pair_key(candidate.primary, candidate.secondary);
    if (emitted_pairs.find(pair_key) != emitted_pairs.end()) {
        return AppendOutcome::skipped;
    }

    db::ResultMatch match;
    match.kind = db::ResultKind::multimatch;
    match.line = line++;
    match.primary = candidate.primary;
    match.primary_name = candidate.primary_name;
    match.secondary = candidate.secondary;
    match.secondary_name = candidate.secondary_name;
    match.ratio = ratio;
    match.primary_nodes = candidate.primary_nodes;
    match.secondary_nodes = candidate.secondary_nodes;
    match.description = candidate.description;
    result.matches.push_back(std::move(match));
    emitted_pairs.insert(pair_key);
    return AppendOutcome::multimatch;
}

} // namespace

std::string apply_postfix(std::string sql, std::string_view postfix)
{
    constexpr std::string_view marker = "%POSTFIX%";
    std::size_t position = 0;
    while ((position = sql.find(marker, position)) != std::string::npos) {
        sql.replace(position, marker.size(), postfix);
        position += postfix.size();
    }
    return sql;
}

SqlHeuristicRunResult SqlHeuristicRunner::run_exact(db::Database& database, const SqlRunnerOptions& options) const
{
    SqlHeuristicRunResult result;
    boost::unordered_flat_set<Address> matched_primary;
    boost::unordered_flat_set<Address> matched_secondary;
    boost::unordered_flat_set<std::string> emitted_pairs;
    int line = 0;

    for (const auto& heuristic : builtin_heuristics()) {
        if (heuristic.ratio_mode != RatioMode::no_false_positives) {
            continue;
        }
        if (!should_run(heuristic, options)) {
            continue;
        }

        SqlHeuristicStats stats;
        stats.name = std::string(heuristic.name);

        const auto sql = wrap_with_limit(apply_postfix(std::string(heuristic.sql), options.postfix), options.max_processed_rows);
        auto rows = query_with_controls(database, sql, options, stats);
        if (options.max_processed_rows != 0 && rows.size() > options.max_processed_rows) {
            rows.resize(options.max_processed_rows);
            stats.row_limit_hit = true;
        }
        stats.candidates = rows.size();
        if (stats.cancelled || stats.timeout_hit) {
            result.stats.push_back(std::move(stats));
            if (stats.cancelled) {
                break;
            }
            continue;
        }

        for (const auto& row : rows) {
            if (row.size() < 45) {
                ++stats.rejected;
                continue;
            }

            CandidateRow candidate;
            {
                soff::perf::ScopedTimer timer("sql.parse_candidate");
                candidate = parse_candidate(row);
            }
            const auto outcome = append_match(
                result,
                slim_candidate(candidate, 1.0),
                heuristic,
                1.0,
                line,
                matched_primary,
                matched_secondary,
                emitted_pairs);
            if (outcome == AppendOutcome::accepted) {
                ++stats.accepted;
            } else if (outcome == AppendOutcome::multimatch) {
                ++stats.multimatches;
            } else {
                ++stats.skipped;
            }
        }

        result.stats.push_back(std::move(stats));
    }

    return result;
}

SqlHeuristicRunResult SqlHeuristicRunner::run_all(db::Database& database, const SqlRunnerOptions& options) const
{
    SqlHeuristicRunResult result;
    boost::unordered_flat_set<Address> matched_primary = options.pre_matched_primary;
    boost::unordered_flat_set<Address> matched_secondary = options.pre_matched_secondary;
    boost::unordered_flat_set<std::string> emitted_pairs;
    int line = 0;

    const auto& base_heuristics = builtin_heuristics();
    const auto heuristics = options.hooks != nullptr
        ? options.hooks->get_heuristics(HeuristicCategory::best, base_heuristics)
        : base_heuristics;

    static soff::ThreadPool pool;
    // Rows are parsed/scored in chunks instead of materializing the entire
    // candidate set; text columns are dropped right after the ratio is known.
    constexpr std::size_t chunk_size = 4000;

    for (const auto& heuristic : heuristics) {
        if (!should_run(heuristic, options)) {
            continue;
        }

        SqlHeuristicStats stats;
        stats.name = std::string(heuristic.name);

        auto effective_postfix = options.postfix;
        if (options.hooks != nullptr) {
            effective_postfix = options.hooks->get_queries_postfix(heuristic.category, effective_postfix);
        }

        auto prepared_sql = apply_postfix(std::string(heuristic.sql), effective_postfix);
        if (options.hooks != nullptr) {
            const auto modified = options.hooks->on_launch_heuristic(heuristic.name, prepared_sql);
            if (!modified.has_value()) {
                continue;
            }
            prepared_sql = modified.value();
        }

        const auto sql = wrap_with_limit(prepared_sql, options.max_processed_rows);
        const double minimum_ratio = minimum_ratio_for(heuristic, options);

        // Accumulators for this heuristic; on cancel/timeout they are rolled
        // back (the previous all-or-nothing behavior is preserved).
        std::vector<WeightedMatchCandidate> assignment_candidates;
        std::vector<SlimCandidate> assignment_slim;
        std::vector<SlimCandidate> direct_multimatch_slim;
        std::size_t eligible_candidates = 0;

        // Progress handler setup (same semantics as query_with_controls).
        const bool has_cancel = static_cast<bool>(options.cancel_requested);
        const bool has_timeout = options.timeout_seconds > 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(options.timeout_seconds);
        if (has_cancel || has_timeout) {
            const auto progress_interval = std::max(1, options.progress_check_interval);
            database.set_progress_handler(progress_interval, [&]() {
                if (has_cancel && options.cancel_requested()) {
                    stats.cancelled = true;
                    return true;
                }
                if (has_timeout && std::chrono::steady_clock::now() >= deadline) {
                    stats.timeout_hit = true;
                    return true;
                }
                return false;
            });
        }

        auto statement = [&]() {
            try {
                return database.prepare(sql);
            } catch (...) {
                database.clear_progress_handler();
                if (!stats.cancelled && !stats.timeout_hit) {
                    throw;
                }
                return db::Statement{};
            }
        }();
        if (stats.cancelled || stats.timeout_hit) {
            stats.candidates = 0;
            result.stats.push_back(std::move(stats));
            if (stats.cancelled) {
                break;
            }
            continue;
        }
        std::size_t rows_seen = 0;
        bool interrupted = false;

        const auto process_chunk = [&](std::vector<CandidateRow>& chunk, std::vector<double>& ratios) {
            // Phase 1: parallel ratio computation for the chunk.
            std::vector<std::future<void>> futures;
            futures.reserve(chunk.size());
            for (std::size_t idx = 0; idx < chunk.size(); ++idx) {
                if (heuristic.ratio_mode != RatioMode::no_false_positives) {
                    futures.push_back(pool.post([&chunk, &ratios, idx]() {
                        soff::perf::ScopedTimer timer("ratio.compute");
                        ratios[idx] = compute_ratio_fast(chunk[idx]);
                    }));
                }
            }
            for (auto& f : futures) {
                f.get();
            }

            // Phase 2: score, filter, and keep only the slim fields.
            for (std::size_t idx = 0; idx < chunk.size(); ++idx) {
                const auto& candidate = chunk[idx];
                double ratio = ratios[idx];

                if (options.enable_relaxed_ratio
                    && candidate.md1 == candidate.md2 && candidate.md1 > 10.0) {
                    ratio = 1.0;
                }

                if (heuristic.ratio_mode != RatioMode::no_false_positives && ratio < 1.0) {
                    double bonus = 0.0;
                    {
                        soff::perf::ScopedTimer timer("ratio.deep_bonus");
                        bonus = deep_ratio_bonus(database, candidate, options);
                    }
                    ratio = ratio + bonus < 1.0 ? ratio + bonus : 0.99;
                }

                if (ratio < minimum_ratio || !std::isfinite(ratio)) {
                    ++stats.rejected;
                    continue;
                }

                if (options.hooks != nullptr) {
                    MatchContext context;
                    context.primary = candidate.primary;
                    context.primary_name = candidate.primary_name;
                    context.secondary = candidate.secondary;
                    context.secondary_name = candidate.secondary_name;
                    context.description = candidate.description;
                    context.ratio = ratio;
                    context.primary_nodes = candidate.primary_nodes;
                    context.secondary_nodes = candidate.secondary_nodes;
                    const auto decision = options.hooks->on_match(context);
                    if (!decision.accept || !std::isfinite(decision.adjusted_ratio)
                        || decision.adjusted_ratio < minimum_ratio) {
                        ++stats.rejected;
                        continue;
                    }
                    ratio = decision.adjusted_ratio;
                }
                ratio = std::clamp(ratio, 0.0, 1.0);

                ++eligible_candidates;
                const bool primary_matched = matched_primary.find(candidate.primary) != matched_primary.end();
                const bool secondary_matched = matched_secondary.find(candidate.secondary) != matched_secondary.end();
                if (primary_matched && secondary_matched) {
                    ++stats.skipped;
                    continue;
                }
                if (primary_matched || secondary_matched) {
                    if (ratio >= 1.0) {
                        direct_multimatch_slim.push_back(slim_candidate(candidate, ratio));
                    } else {
                        ++stats.skipped;
                    }
                    continue;
                }
                assignment_candidates.push_back({candidate.primary, candidate.secondary, ratio});
                assignment_slim.push_back(slim_candidate(candidate, ratio));
            }
        };

        while (true) {
            std::vector<CandidateRow> chunk;
            std::vector<double> ratios;
            chunk.reserve(chunk_size);
            ratios.reserve(chunk_size);

            // Fill one chunk from the statement.
            while (chunk.size() < chunk_size) {
                bool has_row = false;
                try {
                    has_row = statement.step();
                } catch (...) {
                    if (!stats.cancelled && !stats.timeout_hit) {
                        database.clear_progress_handler();
                        throw;
                    }
                    has_row = false;
                    interrupted = true;
                }
                if (!has_row) {
                    break;
                }
                ++rows_seen;
                if (options.max_processed_rows != 0 && rows_seen > options.max_processed_rows) {
                    // The wrap_with_limit overflow row proves more rows exist;
                    // it is not processed and not counted.
                    stats.row_limit_hit = true;
                    --rows_seen;
                    break;
                }
                if (statement.column_count() < 45) {
                    ++stats.rejected;
                    continue;
                }
                std::vector<std::string> values;
                values.reserve(45);
                for (int column = 0; column < 45; ++column) {
                    values.push_back(statement.column_text(column));
                }
                try {
                    soff::perf::ScopedTimer timer("sql.parse_candidate");
                    chunk.push_back(parse_candidate(db::QueryRow(std::move(values))));
                    ratios.push_back(1.0);
                } catch (...) {
                    ++stats.rejected;
                }
            }

            if (!chunk.empty() && !interrupted) {
                process_chunk(chunk, ratios);
            }
            if (interrupted || chunk.size() < chunk_size || stats.row_limit_hit) {
                break;
            }
        }
        database.clear_progress_handler();

        if (interrupted) {
            // Match the previous behavior: an interrupted heuristic contributes
            // no candidates at all.
            stats.candidates = 0;
            result.stats.push_back(std::move(stats));
            if (stats.cancelled) {
                break;
            }
            continue;
        }
        stats.candidates = rows_seen;

        for (const auto& slim : direct_multimatch_slim) {
            const auto outcome = append_forced_multimatch(result, slim, slim.ratio, line, emitted_pairs);
            if (outcome == AppendOutcome::multimatch) ++stats.multimatches;
            else ++stats.skipped;
        }

        const auto resolution = resolve_weighted_matches(assignment_candidates);
        if (resolution.greedy_fallback_components != 0) {
            soff::perf::add_counter(
                "diff.assignment.greedy_fallback",
                resolution.greedy_fallback_components);
        }
        boost::unordered_flat_set<std::size_t> emitted_assignment_indices;
        for (const auto assignment_index : resolution.multimatches) {
            emitted_assignment_indices.insert(assignment_index);
            const auto outcome = append_forced_multimatch(
                result,
                assignment_slim[assignment_index],
                assignment_slim[assignment_index].ratio,
                line,
                emitted_pairs);
            if (outcome == AppendOutcome::multimatch) ++stats.multimatches;
            else ++stats.skipped;
        }
        for (const auto assignment_index : resolution.selected) {
            emitted_assignment_indices.insert(assignment_index);
            const auto outcome = append_match(
                result,
                assignment_slim[assignment_index],
                heuristic,
                assignment_slim[assignment_index].ratio,
                line,
                matched_primary,
                matched_secondary,
                emitted_pairs);
            if (outcome == AppendOutcome::accepted) ++stats.accepted;
            else if (outcome == AppendOutcome::multimatch) ++stats.multimatches;
            else ++stats.skipped;
        }

        const auto resolved_count = emitted_assignment_indices.size()
            + direct_multimatch_slim.size();
        if (eligible_candidates > resolved_count + stats.skipped) {
            stats.rejected += eligible_candidates - resolved_count - stats.skipped;
        }

        result.stats.push_back(std::move(stats));
    }

    return result;
}

} // namespace soff::diff
