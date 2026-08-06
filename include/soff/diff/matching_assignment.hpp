#pragma once

#include "soff/analysis/model.hpp"

#include <cstddef>
#include <vector>

namespace soff::diff {

struct WeightedMatchCandidate
{
    Address primary = 0;
    Address secondary = 0;
    double ratio = 0.0;
};

struct WeightedMatchResolution
{
    std::vector<std::size_t> selected;
    std::vector<std::size_t> multimatches;
    std::size_t greedy_fallback_components = 0;
};

WeightedMatchResolution resolve_weighted_matches(
    const std::vector<WeightedMatchCandidate>& candidates,
    std::size_t exact_component_node_limit = 64,
    double tie_epsilon = 1e-7);

} // namespace soff::diff
