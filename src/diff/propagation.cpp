#include "soff/diff/propagation.hpp"
#include "soff/diff/ratio.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace soff::diff {
namespace {

Address parse_addr(const std::string& text)
{
    Address addr = 0;
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, addr, 10);
    if (result.ec == std::errc{} && result.ptr == end) return addr;
    std::size_t consumed = 0;
    addr = static_cast<Address>(std::stoull(text, &consumed, 0));
    return addr;
}

int parse_int_safe(const std::string& text)
{
    if (text.empty()) return 0;
    try { return std::stoi(text); } catch (...) { return 0; }
}

bool already_matched(Address addr,
    const boost::unordered_flat_set<Address>& primary_set,
    const boost::unordered_flat_set<Address>& secondary_set,
    bool is_primary)
{
    return is_primary
        ? primary_set.find(addr) != primary_set.end()
        : secondary_set.find(addr) != secondary_set.end();
}

struct FuncRow
{
    Address address = 0;
    std::string name;
    int nodes = 0;
};

std::vector<FuncRow> query_functions_in_range(
    db::Database& database,
    Address low, Address high,
    const std::string& prefix)
{
    std::vector<FuncRow> result;
    const auto sql = "select address, name, nodes from " + prefix
        + "functions where address >= " + std::to_string(low)
        + " and address <= " + std::to_string(high)
        + " order by address";
    for (const auto& row : database.query_rows(sql)) {
        if (row.size() < 3) continue;
        FuncRow fr;
        fr.address = parse_addr(row[0]);
        fr.name = row[1];
        fr.nodes = parse_int_safe(row[2]);
        result.push_back(std::move(fr));
    }
    return result;
}

} // namespace

std::size_t find_same_name(
    db::Database& database,
    std::vector<db::ResultMatch>& matches,
    boost::unordered_flat_set<Address>& matched_primary,
    boost::unordered_flat_set<Address>& matched_secondary,
    double min_ratio,
    bool same_processor)
{
    constexpr double bonus_ratio = 0.01;
    std::size_t added = 0;
    const auto sql =
        "select f.address, f.name, df.address, df.name, f.nodes, df.nodes, "
        "f.clean_assembly, df.clean_assembly, f.clean_pseudo, df.clean_pseudo "
        "from functions f, diff.functions df "
        "where (f.name = df.name or (f.mangled_function = df.mangled_function "
        "and f.mangled_function != '' and f.mangled_function is not null)) "
        "and f.name not like 'sub_%' "
        "and f.name not like 'nullsub_%' "
        "and f.name not like 'j_%' "
        "and f.name != 'unknown' "
        "and length(f.name) > 3 "
        "order by f.address";

    for (const auto& row : database.query_rows(sql)) {
        if (row.size() < 10) continue;
        const auto primary_addr = parse_addr(row[0]);
        const auto secondary_addr = parse_addr(row[2]);

        if (matched_primary.find(primary_addr) != matched_primary.end()) continue;
        if (matched_secondary.find(secondary_addr) != matched_secondary.end()) continue;

        double ratio = 1.0;
        const auto& clean_asm1 = row[6];
        const auto& clean_asm2 = row[7];
        const auto& clean_pseudo1 = row[8];
        const auto& clean_pseudo2 = row[9];

        if (!clean_asm1.empty() && clean_asm1 == clean_asm2) {
            ratio = 1.0;
        } else if (!clean_pseudo1.empty() && clean_pseudo1 == clean_pseudo2) {
            ratio = 1.0;
        } else {
            ratio = candidate_text_ratio("", "", "", "",
                clean_asm1, clean_asm2, clean_pseudo1, clean_pseudo2);
        }

        if (ratio < min_ratio) continue;

        // Bonus ratio: boost near-perfect same-name matches
        if (ratio < 1.0 && ratio + bonus_ratio < 1.0) {
            ratio += bonus_ratio;
        }

        db::ResultMatch match;
        match.kind = ratio >= 1.0 ? db::ResultKind::best : db::ResultKind::partial;
        match.line = static_cast<int>(matches.size());
        match.primary = primary_addr;
        match.primary_name = row[1];
        match.secondary = secondary_addr;
        match.secondary_name = row[3];
        match.ratio = ratio;
        match.primary_nodes = parse_int_safe(row[4]);
        match.secondary_nodes = parse_int_safe(row[5]);
        match.description = "Same name";
        matches.push_back(std::move(match));
        matched_primary.insert(primary_addr);
        matched_secondary.insert(secondary_addr);
        ++added;
    }
    return added;
}

std::size_t find_locally_affine_functions(
    db::Database& database,
    std::vector<db::ResultMatch>& matches,
    boost::unordered_flat_set<Address>& matched_primary,
    boost::unordered_flat_set<Address>& matched_secondary,
    double min_ratio,
    int max_gap_size,
    bool same_processor)
{
    struct AddrPair { Address primary; Address secondary; };
    std::vector<AddrPair> sorted_matches;
    sorted_matches.reserve(matches.size());
    for (const auto& m : matches) {
        if (m.kind == db::ResultKind::best || m.kind == db::ResultKind::partial) {
            sorted_matches.push_back({m.primary, m.secondary});
        }
    }
    std::sort(sorted_matches.begin(), sorted_matches.end(),
        [](const auto& a, const auto& b) { return a.primary < b.primary; });

    std::size_t added = 0;
    for (std::size_t i = 0; i + 1 < sorted_matches.size(); ++i) {
        const auto primary_low = sorted_matches[i].primary;
        const auto primary_high = sorted_matches[i + 1].primary;
        const auto secondary_low = sorted_matches[i].secondary;
        const auto secondary_high = sorted_matches[i + 1].secondary;

        if (primary_high <= primary_low || secondary_high <= secondary_low) continue;

        const auto gap_primary = query_functions_in_range(
            database, primary_low + 1, primary_high - 1, "");
        const auto gap_secondary = query_functions_in_range(
            database, secondary_low + 1, secondary_high - 1, "diff.");

        if (gap_primary.empty() || gap_secondary.empty()) continue;
        if (static_cast<int>(gap_primary.size()) > max_gap_size) continue;
        if (static_cast<int>(gap_secondary.size()) > max_gap_size) continue;

        for (const auto& pf : gap_primary) {
            if (matched_primary.find(pf.address) != matched_primary.end()) continue;

            for (const auto& sf : gap_secondary) {
                if (matched_secondary.find(sf.address) != matched_secondary.end()) continue;

                if (pf.name == sf.name && !pf.name.empty()
                    && pf.name.substr(0, 4) != "sub_") {
                    db::ResultMatch match;
                    match.kind = db::ResultKind::partial;
                    match.line = static_cast<int>(matches.size());
                    match.primary = pf.address;
                    match.primary_name = pf.name;
                    match.secondary = sf.address;
                    match.secondary_name = sf.name;
                    match.ratio = 0.7;
                    match.primary_nodes = pf.nodes;
                    match.secondary_nodes = sf.nodes;
                    match.description = "Locally affine (same name)";
                    matches.push_back(std::move(match));
                    matched_primary.insert(pf.address);
                    matched_secondary.insert(sf.address);
                    ++added;
                    break;
                }
            }
        }
    }
    return added;
}

std::size_t find_matches_diffing(
    db::Database& database,
    std::vector<db::ResultMatch>& matches,
    boost::unordered_flat_set<Address>& matched_primary,
    boost::unordered_flat_set<Address>& matched_secondary,
    double min_ratio,
    bool same_processor)
{
    constexpr double bonus_ratio = 0.01;
    std::size_t added = 0;

    const auto current_size = matches.size();
    for (std::size_t i = 0; i < current_size; ++i) {
        const auto& m = matches[i];
        if (m.kind != db::ResultKind::best && m.kind != db::ResultKind::partial) continue;
        if (m.ratio < 0.5) continue;

        const auto field = same_processor ? "assembly" : "pseudocode";
        const auto sql_p = "select " + std::string(field) + ", names from functions where address = '"
            + std::to_string(m.primary) + "' limit 1";
        const auto sql_s = "select " + std::string(field) + ", names from diff.functions where address = '"
            + std::to_string(m.secondary) + "' limit 1";

        const auto rows_p = database.query_rows(sql_p);
        const auto rows_s = database.query_rows(sql_s);
        if (rows_p.empty() || rows_s.empty()) continue;
        if (rows_p.front().size() < 2 || rows_s.front().size() < 2) continue;

        const auto& names_p = rows_p.front()[1];
        const auto& names_s = rows_s.front()[1];
        if (names_p.empty() || names_s.empty()) continue;

        auto extract_names = [](const std::string& json) {
            std::vector<std::string> result;
            std::size_t pos = 0;
            while (pos < json.size()) {
                const auto q1 = json.find('"', pos);
                if (q1 == std::string::npos) break;
                const auto q2 = json.find('"', q1 + 1);
                if (q2 == std::string::npos) break;
                auto name = json.substr(q1 + 1, q2 - q1 - 1);
                if (!name.empty() && name.substr(0, 4) != "sub_"
                    && name.substr(0, 8) != "nullsub_"
                    && name.substr(0, 2) != "j_"
                    && name.size() > 3) {
                    result.push_back(std::move(name));
                }
                pos = q2 + 1;
            }
            return result;
        };

        const auto primary_names = extract_names(names_p);
        const auto secondary_names = extract_names(names_s);

        for (const auto& pn : primary_names) {
            for (const auto& sn : secondary_names) {
                if (pn != sn) continue;

                const auto lookup_p = database.query_rows(
                    "select address, name, nodes from functions where name = '"
                    + pn + "' limit 1");
                const auto lookup_s = database.query_rows(
                    "select address, name, nodes from diff.functions where name = '"
                    + sn + "' limit 1");
                if (lookup_p.empty() || lookup_s.empty()) continue;
                if (lookup_p.front().size() < 3 || lookup_s.front().size() < 3) continue;

                const auto addr_p = parse_addr(lookup_p.front()[0]);
                const auto addr_s = parse_addr(lookup_s.front()[0]);
                if (matched_primary.find(addr_p) != matched_primary.end()) continue;
                if (matched_secondary.find(addr_s) != matched_secondary.end()) continue;

                // Node count filter: reject if size difference > 75%
                const auto nodes_p = parse_int_safe(lookup_p.front()[2]);
                const auto nodes_s = parse_int_safe(lookup_s.front()[2]);
                if (nodes_p > 0 && nodes_s > 0) {
                    const auto min_n = std::min(nodes_p, nodes_s);
                    const auto max_n = std::max(nodes_p, nodes_s);
                    if (max_n > 0 && min_n * 100 / max_n < 25) continue;
                }
                if (nodes_p < 3 && nodes_s < 3) continue;

                // Compute real text ratio
                double ratio = 0.6;
                const auto text_p = database.query_rows(
                    "select clean_assembly, clean_pseudo from functions where address = '"
                    + std::to_string(addr_p) + "' limit 1");
                const auto text_s = database.query_rows(
                    "select clean_assembly, clean_pseudo from diff.functions where address = '"
                    + std::to_string(addr_s) + "' limit 1");
                if (!text_p.empty() && !text_s.empty() && text_p.front().size() >= 2 && text_s.front().size() >= 2) {
                    const auto& tp = text_p.front();
                    const auto& ts = text_s.front();
                    const double r = diff::candidate_text_ratio(
                        "", "", "", "", tp[0], ts[0], tp[1], ts[1]);
                    if (r > ratio) ratio = r;
                }
                if (ratio + bonus_ratio < 1.0) ratio += bonus_ratio;

                db::ResultMatch new_match;
                new_match.kind = db::ResultKind::partial;
                new_match.line = static_cast<int>(matches.size());
                new_match.primary = addr_p;
                new_match.primary_name = lookup_p.front()[1];
                new_match.secondary = addr_s;
                new_match.secondary_name = lookup_s.front()[1];
                new_match.ratio = ratio;
                new_match.primary_nodes = nodes_p;
                new_match.secondary_nodes = nodes_s;
                new_match.description = "Matches diffing";
                matches.push_back(std::move(new_match));
                matched_primary.insert(addr_p);
                matched_secondary.insert(addr_s);
                ++added;
                break;
            }
        }

        // Phase 2: diff-based rename detection
        // Compare text lines to find names unique to each side (renamed functions)
        const auto& text_p = rows_p.front()[0];
        const auto& text_s = rows_s.front()[0];
        if (text_p.empty() || text_s.empty()) continue;

        // Extract function-like names from each side's text
        auto extract_called_names = [](const std::string& text) {
            boost::unordered_flat_set<std::string> names;
            // Simple heuristic: find "call xxx" or "bl xxx" patterns
            std::size_t pos = 0;
            while (pos < text.size()) {
                const auto nl = text.find('\n', pos);
                const auto line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
                pos = nl == std::string::npos ? text.size() : nl + 1;
                // Look for call-like instructions
                const auto call_pos = line.find("call ");
                if (call_pos != std::string::npos) {
                    auto name_start = call_pos + 5;
                    while (name_start < line.size() && line[name_start] == ' ') ++name_start;
                    auto name_end = name_start;
                    while (name_end < line.size() && line[name_end] != ' ' && line[name_end] != ';' && line[name_end] != '\n') ++name_end;
                    if (name_end > name_start) {
                        auto n = line.substr(name_start, name_end - name_start);
                        if (n.size() > 3 && n.substr(0, 4) != "sub_" && n.substr(0, 4) != "loc_") {
                            names.insert(std::move(n));
                        }
                    }
                }
            }
            return names;
        };

        const auto calls_p = extract_called_names(text_p);
        const auto calls_s = extract_called_names(text_s);

        // Names only in primary (removed) paired with names only in secondary (added)
        std::vector<std::string> only_p, only_s;
        for (const auto& n : calls_p) {
            if (!calls_s.contains(n)) only_p.push_back(n);
        }
        for (const auto& n : calls_s) {
            if (!calls_p.contains(n)) only_s.push_back(n);
        }

        // Try to match renamed pairs
        for (const auto& pn : only_p) {
            for (const auto& sn : only_s) {
                const auto lookup_p2 = database.query_rows(
                    "select address, name, nodes from functions where name = '" + pn + "' limit 1");
                const auto lookup_s2 = database.query_rows(
                    "select address, name, nodes from diff.functions where name = '" + sn + "' limit 1");
                if (lookup_p2.empty() || lookup_s2.empty()) continue;
                if (lookup_p2.front().size() < 3 || lookup_s2.front().size() < 3) continue;

                const auto addr_p2 = parse_addr(lookup_p2.front()[0]);
                const auto addr_s2 = parse_addr(lookup_s2.front()[0]);
                if (matched_primary.contains(addr_p2)) continue;
                if (matched_secondary.contains(addr_s2)) continue;

                const auto nodes_p2 = parse_int_safe(lookup_p2.front()[2]);
                const auto nodes_s2 = parse_int_safe(lookup_s2.front()[2]);
                if (nodes_p2 < 3 && nodes_s2 < 3) continue;

                // Compute ratio for the renamed pair
                double ratio = 0.5;
                const auto tp2 = database.query_rows(
                    "select clean_assembly, clean_pseudo from functions where address = '"
                    + std::to_string(addr_p2) + "' limit 1");
                const auto ts2 = database.query_rows(
                    "select clean_assembly, clean_pseudo from diff.functions where address = '"
                    + std::to_string(addr_s2) + "' limit 1");
                if (!tp2.empty() && !ts2.empty() && tp2.front().size() >= 2 && ts2.front().size() >= 2) {
                    const double r = diff::candidate_text_ratio(
                        "", "", "", "", tp2.front()[0], ts2.front()[0], tp2.front()[1], ts2.front()[1]);
                    if (r > ratio) ratio = r;
                }
                if (ratio < min_ratio) continue;
                if (ratio + bonus_ratio < 1.0) ratio += bonus_ratio;

                db::ResultMatch new_match;
                new_match.kind = db::ResultKind::partial;
                new_match.line = static_cast<int>(matches.size());
                new_match.primary = addr_p2;
                new_match.primary_name = lookup_p2.front()[1];
                new_match.secondary = addr_s2;
                new_match.secondary_name = lookup_s2.front()[1];
                new_match.ratio = ratio;
                new_match.primary_nodes = nodes_p2;
                new_match.secondary_nodes = nodes_s2;
                new_match.description = "Matches diffing (renamed)";
                matches.push_back(std::move(new_match));
                matched_primary.insert(addr_p2);
                matched_secondary.insert(addr_s2);
                ++added;
                break;
            }
        }
    }
    return added;
}

std::size_t find_related_constants(
    db::Database& database,
    std::vector<db::ResultMatch>& matches,
    boost::unordered_flat_set<Address>& matched_primary,
    boost::unordered_flat_set<Address>& matched_secondary,
    double min_ratio)
{
    std::size_t added = 0;
    const auto current_size = matches.size();

    for (std::size_t i = 0; i < current_size; ++i) {
        const auto& m = matches[i];
        if (m.kind != db::ResultKind::best && m.kind != db::ResultKind::partial) continue;
        if (m.ratio < min_ratio) continue;

        const auto sql_p = "select constant from constants where func_id = '"
            + std::to_string(m.primary) + "'";
        const auto sql_s = "select constant from constants where func_id = '"
            + std::to_string(m.secondary) + "'";

        const auto rows_p = database.query_rows(sql_p);
        const auto rows_s = database.query_rows("select constant from diff.constants where func_id = '"
            + std::to_string(m.secondary) + "'");

        if (rows_p.empty() || rows_s.empty()) continue;

        boost::unordered_flat_set<std::string> constants_p;
        for (const auto& r : rows_p) {
            if (!r.empty() && !r[0].empty()) constants_p.insert(r[0]);
        }

        std::vector<std::string> shared;
        for (const auto& r : rows_s) {
            if (!r.empty() && constants_p.find(r[0]) != constants_p.end()) {
                shared.push_back(r[0]);
            }
        }
        if (shared.empty()) continue;

        for (const auto& constant : shared) {
            // Frequency filter: skip constants referenced by too many functions
            const auto freq_p = database.query_rows(
                "select count(distinct func_id) from constants where constant = '"
                + constant + "'");
            const auto freq_s = database.query_rows(
                "select count(distinct func_id) from diff.constants where constant = '"
                + constant + "'");
            if (!freq_p.empty() && !freq_p.front().empty()) {
                if (parse_int_safe(freq_p.front()[0]) > 512) continue;
            }
            if (!freq_s.empty() && !freq_s.front().empty()) {
                if (parse_int_safe(freq_s.front()[0]) > 512) continue;
            }

            const auto funcs_p = database.query_rows(
                "select distinct func_id from constants where constant = '"
                + constant + "'");
            const auto funcs_s = database.query_rows(
                "select distinct func_id from diff.constants where constant = '"
                + constant + "'");

            for (const auto& fp : funcs_p) {
                if (fp.empty()) continue;
                const auto addr_p = parse_addr(fp[0]);
                if (matched_primary.find(addr_p) != matched_primary.end()) continue;

                for (const auto& fs : funcs_s) {
                    if (fs.empty()) continue;
                    const auto addr_s = parse_addr(fs[0]);
                    if (matched_secondary.find(addr_s) != matched_secondary.end()) continue;

                    const auto info_p = database.query_rows(
                        "select name, nodes from functions where address = '"
                        + std::to_string(addr_p) + "' limit 1");
                    const auto info_s = database.query_rows(
                        "select name, nodes from diff.functions where address = '"
                        + std::to_string(addr_s) + "' limit 1");
                    if (info_p.empty() || info_s.empty()) continue;
                    if (info_p.front().size() < 2 || info_s.front().size() < 2) continue;

                    const auto& name_p = info_p.front()[0];
                    const auto& name_s = info_s.front()[0];
                    if (name_p != name_s) continue;
                    if (name_p.substr(0, 4) == "sub_") continue;

                    db::ResultMatch new_match;
                    new_match.kind = db::ResultKind::partial;
                    new_match.line = static_cast<int>(matches.size());
                    new_match.primary = addr_p;
                    new_match.primary_name = name_p;
                    new_match.secondary = addr_s;
                    new_match.secondary_name = name_s;
                    new_match.ratio = 0.6;
                    new_match.primary_nodes = parse_int_safe(info_p.front()[1]);
                    new_match.secondary_nodes = parse_int_safe(info_s.front()[1]);
                    new_match.description = "Related constants";
                    matches.push_back(std::move(new_match));
                    matched_primary.insert(addr_p);
                    matched_secondary.insert(addr_s);
                    ++added;
                    goto next_constant;
                }
            }
            next_constant:;
        }
    }
    return added;
}

PropagationStats run_propagation(
    db::Database& database,
    std::vector<db::ResultMatch>& matches,
    boost::unordered_flat_set<Address>& matched_primary,
    boost::unordered_flat_set<Address>& matched_secondary,
    const PropagationOptions& options)
{
    PropagationStats stats;
    if (!options.enabled) return stats;

    stats.same_name_matches = find_same_name(
        database, matches, matched_primary, matched_secondary,
        options.same_name_min_ratio, options.same_processor);

    for (int iter = 0; iter < options.max_iterations; ++iter) {
        std::size_t round_added = 0;

        const auto diffing = find_matches_diffing(
            database, matches, matched_primary, matched_secondary,
            options.diffing_min_ratio, options.same_processor);
        stats.diffing_matches += diffing;
        round_added += diffing;

        const auto related = options.enable_slow
            ? find_related_constants(
                database, matches, matched_primary, matched_secondary,
                options.related_min_ratio)
            : 0;
        stats.related_constants_matches += related;
        round_added += related;

        const auto cu_matches = find_compilation_unit_matches(
            database, matches, matched_primary, matched_secondary,
            options.same_name_min_ratio, options.same_processor);
        round_added += cu_matches;

        const auto affine = find_locally_affine_functions(
            database, matches, matched_primary, matched_secondary,
            options.affine_min_ratio, options.max_functions_per_gap,
            options.same_processor);
        stats.affine_matches += affine;
        round_added += affine;

        stats.iterations_run = iter + 1;
        if (round_added == 0) break;
    }

    return stats;
}

std::size_t find_compilation_unit_matches(
    db::Database& database,
    std::vector<db::ResultMatch>& matches,
    boost::unordered_flat_set<Address>& matched_primary,
    boost::unordered_flat_set<Address>& matched_secondary,
    double min_ratio,
    bool same_processor)
{
    // Find CU names that exist in both databases
    const auto cu_rows = database.query_rows(
        "select cu.name from compilation_units cu "
        "inner join diff.compilation_units dcu on cu.name = dcu.name "
        "where cu.name != ''");
    if (cu_rows.empty()) return 0;

    std::size_t added = 0;
    for (const auto& cu_row : cu_rows) {
        if (cu_row.empty()) continue;
        const auto& cu_name = cu_row[0];

        // Get unmatched functions in this CU from both sides
        const auto primary_funcs = database.query_rows(
            "select f.address, f.name, f.nodes, f.clean_assembly, f.clean_pseudo "
            "from functions f "
            "inner join compilation_unit_functions cuf on cuf.address = f.address "
            "inner join compilation_units cu on cu.id = cuf.cu_id "
            "where cu.name = '" + cu_name + "'");
        const auto secondary_funcs = database.query_rows(
            "select f.address, f.name, f.nodes, f.clean_assembly, f.clean_pseudo "
            "from diff.functions f "
            "inner join diff.compilation_unit_functions cuf on cuf.address = f.address "
            "inner join diff.compilation_units cu on cu.id = cuf.cu_id "
            "where cu.name = '" + cu_name + "'");

        for (const auto& pf : primary_funcs) {
            if (pf.size() < 5) continue;
            const auto addr_p = parse_addr(pf[0]);
            if (matched_primary.contains(addr_p)) continue;
            const auto& name_p = pf[1];
            if (name_p.empty()) continue;

            for (const auto& sf : secondary_funcs) {
                if (sf.size() < 5) continue;
                const auto addr_s = parse_addr(sf[0]);
                if (matched_secondary.contains(addr_s)) continue;
                if (sf[1] != name_p) continue;

                // Same name in same CU - compute ratio
                double ratio = candidate_text_ratio(
                    "", "", "", "", pf[3], sf[3], pf[4], sf[4]);
                if (ratio < min_ratio) continue;
                if (ratio + 0.01 < 1.0) ratio += 0.01;

                db::ResultMatch match;
                match.kind = db::ResultKind::partial;
                match.line = static_cast<int>(matches.size());
                match.primary = addr_p;
                match.primary_name = name_p;
                match.secondary = addr_s;
                match.secondary_name = sf[1];
                match.ratio = ratio;
                match.primary_nodes = parse_int_safe(pf[2]);
                match.secondary_nodes = parse_int_safe(sf[2]);
                match.description = "Same compilation unit";
                matches.push_back(std::move(match));
                matched_primary.insert(addr_p);
                matched_secondary.insert(addr_s);
                ++added;
                break;
            }
        }
    }
    return added;
}

} // namespace soff::diff
