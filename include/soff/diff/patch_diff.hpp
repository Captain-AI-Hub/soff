#pragma once

#include "soff/core/hooks.hpp"
#include "soff/db/database.hpp"
#include "soff/db/result_repository.hpp"

#include <string>
#include <vector>

namespace soff::diff {

enum class VulnIndicator
{
    signed_unsigned_change,
    unsafe_function_pattern,
    size_check_added,
};

struct PatchDiffFinding
{
    Address primary = 0;
    std::string primary_name;
    Address secondary = 0;
    std::string secondary_name;
    double ratio = 0.0;
    std::string heuristic;
    VulnIndicator indicator = VulnIndicator::unsafe_function_pattern;
    std::string detail;
};

struct PatchDiffResult
{
    std::vector<PatchDiffFinding> findings;
};

bool detect_signed_unsigned_change(
    const std::string& asm_primary,
    const std::string& asm_secondary,
    std::string& detail);

bool detect_unsafe_function_pattern(
    const std::string& pseudo_primary,
    const std::string& pseudo_secondary,
    std::string& detail);

bool detect_size_check_added(
    const std::string& pseudo_primary,
    const std::string& pseudo_secondary,
    std::string& detail);

PatchDiffResult analyze_patch_diff(
    db::Database& database,
    const std::vector<db::ResultMatch>& matches);

class PatchDiffHook : public DiffHooks
{
public:
    MatchDecision on_match(const MatchContext& context) override;
    const PatchDiffResult& result() const noexcept { return result_; }
    void set_database(db::Database* db) { database_ = db; }

private:
    db::Database* database_ = nullptr;
    PatchDiffResult result_;
};

} // namespace soff::diff
