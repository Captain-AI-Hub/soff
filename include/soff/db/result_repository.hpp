#pragma once

#include "soff/analysis/model.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace soff::db {

enum class ResultKind
{
    best,
    partial,
    unreliable,
    multimatch,
};

enum class UnmatchedKind
{
    primary,
    secondary,
};

struct ResultMatch
{
    ResultKind kind = ResultKind::best;
    int line = 0;
    Address primary = 0;
    std::string primary_name;
    Address secondary = 0;
    std::string secondary_name;
    double ratio = 0.0;
    int primary_nodes = 0;
    int secondary_nodes = 0;
    std::string description;
};

struct UnmatchedFunction
{
    UnmatchedKind kind = UnmatchedKind::primary;
    int line = 0;
    Address address = 0;
    std::string name;
};

struct HeuristicStat
{
    int line = 0;
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

struct DiffResultSet
{
    std::string main_db;
    std::string diff_db;
    std::string version = "3.4";
    std::string date;
    std::vector<ResultMatch> matches;
    std::vector<UnmatchedFunction> unmatched;
    std::vector<HeuristicStat> heuristic_stats;
};

struct ResultSummary
{
    std::size_t best = 0;
    std::size_t partial = 0;
    std::size_t unreliable = 0;
    std::size_t multimatch = 0;
    std::size_t unmatched_primary = 0;
    std::size_t unmatched_secondary = 0;
};

class ResultRepository
{
public:
    void create_schema(const std::filesystem::path& path) const;
    bool save(const DiffResultSet& results, const std::filesystem::path& path) const;
    DiffResultSet load(const std::filesystem::path& path) const;
    ResultSummary summarize(const std::filesystem::path& path) const;
};

std::string_view result_kind_name(ResultKind kind);
std::string_view unmatched_kind_name(UnmatchedKind kind);

} // namespace soff::db
