#pragma once

#include "soff/analysis/model.hpp"

#include <string>
#include <vector>

namespace soff {

enum class MatchQuality
{
    best,
    partial,
    unreliable,
};

struct FunctionMatch
{
    Address primary = 0;
    Address secondary = 0;
    double ratio = 0.0;
    MatchQuality quality = MatchQuality::unreliable;
    std::string heuristic;
};

class DiffMatcher
{
public:
    std::vector<FunctionMatch> match(
        const ProgramSnapshot& primary,
        const ProgramSnapshot& secondary) const;
};

} // namespace soff
