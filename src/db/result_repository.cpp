#include "soff/db/result_repository.hpp"

#include "soff/db/database.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace soff::db {
namespace {

std::string address_to_text(Address address)
{
    return std::to_string(address);
}

Address parse_address(const std::string& text, bool bare_hex = false)
{
    Address address = 0;
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    if (bare_hex && text.rfind("0x", 0) != 0 && text.rfind("0X", 0) != 0) {
        const auto hex_result = std::from_chars(begin, end, address, 16);
        if (hex_result.ec == std::errc{} && hex_result.ptr == end) {
            return address;
        }
    }

    const auto result = std::from_chars(begin, end, address, 10);
    if (result.ec == std::errc{} && result.ptr == end) {
        return address;
    }

    const auto looks_like_hex_without_prefix = std::all_of(text.begin(), text.end(), [](unsigned char ch) {
        return std::isxdigit(ch) != 0;
    });
    if (looks_like_hex_without_prefix) {
        const auto hex_result = std::from_chars(begin, end, address, 16);
        if (hex_result.ec == std::errc{} && hex_result.ptr == end) {
            return address;
        }
    }

    std::size_t consumed = 0;
    address = static_cast<Address>(std::stoull(text, &consumed, 0));
    if (consumed != text.size()) {
        throw std::runtime_error("invalid address text: " + text);
    }
    return address;
}

ResultKind parse_result_kind(const std::string& text)
{
    if (text == "best") {
        return ResultKind::best;
    }
    if (text == "partial") {
        return ResultKind::partial;
    }
    if (text == "unreliable") {
        return ResultKind::unreliable;
    }
    if (text == "multimatch") {
        return ResultKind::multimatch;
    }
    throw std::runtime_error("unknown result kind: " + text);
}

UnmatchedKind parse_unmatched_kind(const std::string& text)
{
    if (text == "primary") {
        return UnmatchedKind::primary;
    }
    if (text == "secondary") {
        return UnmatchedKind::secondary;
    }
    throw std::runtime_error("unknown unmatched kind: " + text);
}

std::string now_text()
{
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif
    std::ostringstream stream;
    stream << std::put_time(&tm, "%a %b %d %H:%M:%S %Y");
    return stream.str();
}

bool has_table(Database& database, std::string_view table_name)
{
    const auto sql = "select count(*) from sqlite_master where type = 'table' and name = '" + std::string(table_name) + "'";
    return database.query_int(sql) > 0;
}

} // namespace

std::string_view result_kind_name(ResultKind kind)
{
    switch (kind) {
    case ResultKind::best:
        return "best";
    case ResultKind::partial:
        return "partial";
    case ResultKind::unreliable:
        return "unreliable";
    case ResultKind::multimatch:
        return "multimatch";
    }
    return "unknown";
}

std::string_view unmatched_kind_name(UnmatchedKind kind)
{
    switch (kind) {
    case UnmatchedKind::primary:
        return "primary";
    case UnmatchedKind::secondary:
        return "secondary";
    }
    return "unknown";
}

void ResultRepository::create_schema(const std::filesystem::path& path) const
{
    Database database;
    database.open(path);
    database.execute("create table if not exists config (main_db text, diff_db text, version text, date text)");
    database.execute(
        "create table if not exists results ("
        "type text, line integer, address text, name text, address2 text, name2 text, "
        "ratio real, nodes1 integer, nodes2 integer, description text)");
    database.execute("create unique index if not exists uq_results on results(address, address2)");
    database.execute("create table if not exists unmatched (type text, line integer, address text, name text)");
    database.execute(
        "create table if not exists heuristic_stats ("
        "line integer, name text, candidates integer, accepted integer, rejected integer, skipped integer, "
        "multimatches integer, row_limit_hit integer, timeout_hit integer, cancelled integer)");
}

bool ResultRepository::save(const DiffResultSet& results, const std::filesystem::path& path) const
{
    if (std::filesystem::exists(path)) {
        std::filesystem::remove(path);
    }

    create_schema(path);

    Database database;
    database.open(path);
    Transaction transaction(database);

    const std::string date = results.date.empty() ? now_text() : results.date;
    database.execute("insert into config values (?, ?, ?, ?)", {results.main_db, results.diff_db, results.version, date});

    for (const auto& match : results.matches) {
        database.execute(
            "insert or ignore into results values (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
            {
                std::string(result_kind_name(match.kind)),
                std::to_string(match.line),
                address_to_text(match.primary),
                match.primary_name,
                address_to_text(match.secondary),
                match.secondary_name,
                std::to_string(match.ratio),
                std::to_string(match.primary_nodes),
                std::to_string(match.secondary_nodes),
                match.description,
            });
    }

    for (const auto& unmatched : results.unmatched) {
        database.execute(
            "insert into unmatched values (?, ?, ?, ?)",
            {
                std::string(unmatched_kind_name(unmatched.kind)),
                std::to_string(unmatched.line),
                address_to_text(unmatched.address),
                unmatched.name,
            });
    }

    for (const auto& stats : results.heuristic_stats) {
        database.execute(
            "insert into heuristic_stats values (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
            {
                std::to_string(stats.line),
                stats.name,
                std::to_string(stats.candidates),
                std::to_string(stats.accepted),
                std::to_string(stats.rejected),
                std::to_string(stats.skipped),
                std::to_string(stats.multimatches),
                stats.row_limit_hit ? "1" : "0",
                stats.timeout_hit ? "1" : "0",
                stats.cancelled ? "1" : "0",
            });
    }

    transaction.commit();
    return true;
}

DiffResultSet ResultRepository::load(const std::filesystem::path& path) const
{
    Database database;
    database.open(path);

    DiffResultSet results;
    const auto config_rows = database.query_rows("select main_db, diff_db, version, date from config limit 1");
    if (!config_rows.empty()) {
        results.main_db = config_rows[0][0];
        results.diff_db = config_rows[0][1];
        results.version = config_rows[0][2];
        results.date = config_rows[0][3];
    }
    const auto match_rows = database.query_rows(
        "select type, line, address, name, address2, name2, ratio, nodes1, nodes2, description "
        "from results order by rowid");
    results.matches.reserve(match_rows.size());
    for (const auto& row : match_rows) {
        ResultMatch match;
        match.kind = parse_result_kind(row[0]);
        match.line = std::stoi(row[1]);
        match.primary = parse_address(row[2]);
        match.primary_name = row[3];
        match.secondary = parse_address(row[4]);
        match.secondary_name = row[5];
        match.ratio = std::stod(row[6]);
        match.primary_nodes = std::stoi(row[7]);
        match.secondary_nodes = std::stoi(row[8]);
        match.description = row[9];
        results.matches.push_back(std::move(match));
    }

    const auto unmatched_rows = database.query_rows("select type, line, address, name from unmatched order by rowid");
    results.unmatched.reserve(unmatched_rows.size());
    for (const auto& row : unmatched_rows) {
        UnmatchedFunction unmatched;
        unmatched.kind = parse_unmatched_kind(row[0]);
        unmatched.line = std::stoi(row[1]);
        unmatched.address = parse_address(row[2]);
        unmatched.name = row[3];
        results.unmatched.push_back(std::move(unmatched));
    }

    if (has_table(database, "heuristic_stats")) {
        const auto stats_rows = database.query_rows(
            "select line, name, candidates, accepted, rejected, skipped, multimatches, "
            "row_limit_hit, timeout_hit, cancelled from heuristic_stats order by line");
        results.heuristic_stats.reserve(stats_rows.size());
        for (const auto& row : stats_rows) {
            HeuristicStat stats;
            stats.line = std::stoi(row[0]);
            stats.name = row[1];
            stats.candidates = static_cast<std::size_t>(std::stoull(row[2]));
            stats.accepted = static_cast<std::size_t>(std::stoull(row[3]));
            stats.rejected = static_cast<std::size_t>(std::stoull(row[4]));
            stats.skipped = static_cast<std::size_t>(std::stoull(row[5]));
            stats.multimatches = static_cast<std::size_t>(std::stoull(row[6]));
            stats.row_limit_hit = row[7] == "1";
            stats.timeout_hit = row[8] == "1";
            stats.cancelled = row[9] == "1";
            results.heuristic_stats.push_back(std::move(stats));
        }
    }

    return results;
}

ResultSummary ResultRepository::summarize(const std::filesystem::path& path) const
{
    Database database;
    database.open(path);

    ResultSummary summary;
    const auto result_rows = database.query_rows("select type, count(*) from results group by type");
    for (const auto& row : result_rows) {
        const auto kind = parse_result_kind(row[0]);
        const auto count = static_cast<std::size_t>(std::stoull(row[1]));
        switch (kind) {
        case ResultKind::best:
            summary.best = count;
            break;
        case ResultKind::partial:
            summary.partial = count;
            break;
        case ResultKind::unreliable:
            summary.unreliable = count;
            break;
        case ResultKind::multimatch:
            summary.multimatch = count;
            break;
        }
    }

    const auto unmatched_rows = database.query_rows("select type, count(*) from unmatched group by type");
    for (const auto& row : unmatched_rows) {
        const auto kind = parse_unmatched_kind(row[0]);
        const auto count = static_cast<std::size_t>(std::stoull(row[1]));
        switch (kind) {
        case UnmatchedKind::primary:
            summary.unmatched_primary = count;
            break;
        case UnmatchedKind::secondary:
            summary.unmatched_secondary = count;
            break;
        }
    }

    return summary;
}

} // namespace soff::db
