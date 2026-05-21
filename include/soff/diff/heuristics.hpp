#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace soff::diff {

enum class HeuristicCategory
{
    best,
    partial,
    experimental,
    unreliable,
};

enum class RatioMode
{
    no_false_positives,
    ratio,
    ratio_with_minimum,
    trusted_ratio_with_minimum,
};

enum HeuristicFlag : unsigned
{
    heuristic_flag_none = 0,
    heuristic_flag_unreliable = 1u << 0,
    heuristic_flag_slow = 1u << 1,
    heuristic_flag_same_cpu = 1u << 2,
};

struct HeuristicDefinition
{
    std::string_view name;
    HeuristicCategory category;
    RatioMode ratio_mode;
    unsigned flags = heuristic_flag_none;
    double minimum_ratio = 0.0;
    std::string_view sql;
};

std::string_view category_name(HeuristicCategory category);
std::string_view ratio_mode_name(RatioMode mode);
bool has_flag(const HeuristicDefinition& heuristic, HeuristicFlag flag);
bool supports_same_cpu_only(const HeuristicDefinition& heuristic);
bool is_slow(const HeuristicDefinition& heuristic);
bool is_unreliable(const HeuristicDefinition& heuristic);
std::vector<std::string> required_fields(const HeuristicDefinition& heuristic);
const std::vector<HeuristicDefinition>& builtin_heuristics();
std::vector<std::string> validate_builtin_heuristics();

} // namespace soff::diff
