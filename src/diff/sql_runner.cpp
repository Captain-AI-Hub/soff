#include "soff/diff/sql_runner.hpp"

#include "soff/core/hooks.hpp"
#include "soff/core/thread_pool.hpp"
#include "soff/diff/heuristics.hpp"
#include "soff/diff/ratio.hpp"

#include <charconv>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <iterator>
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

double numeric_similarity(int left, int right)
{
    if (left == right) {
        return 1.0;
    }
    if (left <= 0 || right <= 0) {
        return 0.0;
    }
    const auto low = static_cast<double>(std::min(left, right));
    const auto high = static_cast<double>(std::max(left, right));
    return low / high;
}

double graph_metric_ratio(const CandidateRow& candidate)
{
    const double values[] = {
        numeric_similarity(candidate.primary_nodes, candidate.secondary_nodes),
        numeric_similarity(candidate.primary_edges, candidate.secondary_edges),
        numeric_similarity(candidate.primary_indegree, candidate.secondary_indegree),
        numeric_similarity(candidate.primary_outdegree, candidate.secondary_outdegree),
        numeric_similarity(candidate.primary_instructions, candidate.secondary_instructions),
        numeric_similarity(candidate.primary_cc, candidate.secondary_cc),
        numeric_similarity(candidate.primary_strongly_connected, candidate.secondary_strongly_connected),
        numeric_similarity(candidate.primary_loops, candidate.secondary_loops),
        numeric_similarity(candidate.primary_constants_count, candidate.secondary_constants_count),
        numeric_similarity(candidate.primary_size, candidate.secondary_size),
    };

    double total = 0.0;
    for (const auto value : values) {
        total += value;
    }
    return total / static_cast<double>(sizeof(values) / sizeof(values[0]));
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

double deep_ratio_bonus(db::Database& database, const CandidateRow& candidate, bool same_processor)
{
    const auto main_rows = database.query_rows(
        "select source_file, pseudocode_primes, indegree, outdegree, switches, cyclomatic_complexity, constants "
        "from functions where address = '" + std::to_string(candidate.primary) + "' limit 1");
    const auto diff_rows = database.query_rows(
        "select source_file, pseudocode_primes, indegree, outdegree, switches, cyclomatic_complexity, constants "
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
    if (!left[1].empty() && left[1] == right[1]) {
        score += 0.001;
    }
    // indegree
    if (left[2] == right[2] && parse_int_or_zero(left[2]) != 0) {
        score += 0.001;
    }
    // outdegree
    if (left[3] == right[3] && parse_int_or_zero(left[3]) != 0) {
        score += 0.001;
    }
    // switches
    if (left[4] == right[4] && left[4] != "[]" && !left[4].empty()) {
        score += 0.003;
    }
    // cyclomatic_complexity
    if (left[5] == right[5] && parse_int_or_zero(left[5]) != 0) {
        score += 0.001;
    }
    // constants: per-constant accumulation (same_cpu: +0.006, diff_cpu: +0.008)
    if (left[6] != "[]" && !left[6].empty() && !right[6].empty()) {
        const auto left_consts = parse_jsonish_array_values(left[6]);
        const auto right_consts = parse_jsonish_array_values(right[6]);
        const auto common = intersection_size(left_consts, right_consts);
        score += static_cast<double>(common) * (same_processor ? 0.006 : 0.008);
    }
    return score;
}

double compute_ratio(db::Database& database, const CandidateRow& candidate, bool same_processor, bool relaxed_ratio = false)
{
    if (equal_non_empty_ratio(candidate.bytes_hash1, candidate.bytes_hash2) == 1.0) {
        return 1.0;
    }

    // In relaxed mode with matching high md_index, accept immediately
    if (relaxed_ratio && candidate.md1 == candidate.md2 && candidate.md1 > 10.0) {
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

    if (ratio < 1.0) {
        const auto score = deep_ratio_bonus(database, candidate, same_processor);
        ratio = ratio + score < 1.0 ? ratio + score : 0.99;
    }

    return std::min(1.0, ratio);
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
        v4 = std::min((v1 + v2 + v3 + 3.0) / 5.0, 1.0);
    }
    const double v5 = candidate_text_ratio("", "", "", "", candidate.stripped_micro1, candidate.stripped_micro2, "", "");
    if (v5 == 1.0) return 1.0;

    double ratio = std::max({v1, v2, v3, v4, v5});
    if (ratio == 1.0 && candidate.md1 != candidate.md2) {
        ratio = 0.0;
        for (const auto value : {v1, v2, v3, v4, v5}) {
            if (value != 1.0 && value > ratio) ratio = value;
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

AppendOutcome append_match(
    SqlHeuristicRunResult& result,
    const CandidateRow& candidate,
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

            const auto candidate = parse_candidate(row);
            const auto outcome = append_match(
                result,
                candidate,
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

        const double minimum_ratio = minimum_ratio_for(heuristic, options);

        // Parse candidates and compute ratios in parallel
        struct ScoredCandidate {
            CandidateRow candidate;
            double ratio = 1.0;
            bool valid = false;
        };
        std::vector<ScoredCandidate> scored(rows.size());

        // Phase 1: parse + parallel ratio computation
        {
            static soff::ThreadPool pool;
            std::vector<std::future<void>> futures;
            futures.reserve(rows.size());
            for (std::size_t idx = 0; idx < rows.size(); ++idx) {
                if (rows[idx].size() < 45) continue;
                scored[idx].candidate = parse_candidate(rows[idx]);
                scored[idx].valid = true;
                if (heuristic.ratio_mode != RatioMode::no_false_positives) {
                    futures.push_back(pool.post([&scored, idx]() {
                        scored[idx].ratio = compute_ratio_fast(scored[idx].candidate);
                    }));
                }
            }
            for (auto& f : futures) f.get();
        }

        // Phase 2: sequential deep_ratio_bonus + filtering
        for (auto& sc : scored) {
            if (!sc.valid) {
                ++stats.rejected;
                continue;
            }
            auto& candidate = sc.candidate;
            double ratio = sc.ratio;

            // Relaxed ratio: matching high md_index -> accept as 1.0
            if (options.enable_relaxed_ratio
                && candidate.md1 == candidate.md2 && candidate.md1 > 10.0) {
                ratio = 1.0;
            }

            if (heuristic.ratio_mode != RatioMode::no_false_positives && ratio < 1.0 && ratio >= minimum_ratio - 0.05) {
                const auto bonus = deep_ratio_bonus(database, candidate, options.same_processor);
                ratio = ratio + bonus < 1.0 ? ratio + bonus : 0.99;
            }

            if (ratio < minimum_ratio) {
                ++stats.rejected;
                continue;
            }
            if (!std::isfinite(ratio)) {
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
                if (!decision.accept) {
                    ++stats.rejected;
                    continue;
                }
                ratio = decision.adjusted_ratio;
            }

            const auto outcome = append_match(
                result,
                candidate,
                heuristic,
                ratio,
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

} // namespace soff::diff
