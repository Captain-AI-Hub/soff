#include "soff/diff/matching_assignment.hpp"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <vector>

void test_matching_assignment()
{
    {
        const std::vector<soff::diff::WeightedMatchCandidate> candidates{
            {1, 10, 0.90},
            {1, 20, 0.80},
            {2, 10, 0.85},
        };
        const auto resolution = soff::diff::resolve_weighted_matches(candidates);
        assert(resolution.multimatches.empty());
        assert(resolution.selected.size() == 2);
        assert(std::find(resolution.selected.begin(), resolution.selected.end(), 1) != resolution.selected.end());
        assert(std::find(resolution.selected.begin(), resolution.selected.end(), 2) != resolution.selected.end());
        std::cout << "assignment: maximum-cardinality weighted regression passed\n";
    }

    {
        const std::vector<soff::diff::WeightedMatchCandidate> candidates{
            {1, 10, 0.90},
            {1, 20, 0.90},
            {2, 10, 0.70},
        };
        const auto resolution = soff::diff::resolve_weighted_matches(candidates);
        assert(resolution.selected.empty());
        assert(resolution.multimatches.size() == 2);
        assert(resolution.multimatches[0] == 0);
        assert(resolution.multimatches[1] == 1);
        std::cout << "assignment: tied candidates preserved as multimatch\n";
    }

    {
        const std::vector<soff::diff::WeightedMatchCandidate> candidates{
            {1, 10, 0.60},
            {1, 10, 0.95},
            {2, 20, 0.80},
        };
        const auto resolution = soff::diff::resolve_weighted_matches(candidates);
        assert(resolution.multimatches.empty());
        assert(resolution.selected.size() == 2);
        assert(std::find(resolution.selected.begin(), resolution.selected.end(), 1) != resolution.selected.end());
        assert(std::find(resolution.selected.begin(), resolution.selected.end(), 2) != resolution.selected.end());
        std::cout << "assignment: duplicate pair keeps highest final score\n";
    }
}
