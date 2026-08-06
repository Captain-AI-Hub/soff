#include "soff/diff/matching_assignment.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace soff::diff {
namespace {

struct UniqueEdge
{
    Address primary = 0;
    Address secondary = 0;
    double ratio = 0.0;
    std::size_t original_index = 0;
};

std::vector<std::size_t> hungarian_select(const std::vector<UniqueEdge>& edges)
{
    std::vector<Address> primary_nodes;
    std::vector<Address> secondary_nodes;
    for (const auto& edge : edges) {
        primary_nodes.push_back(edge.primary);
        secondary_nodes.push_back(edge.secondary);
    }
    std::sort(primary_nodes.begin(), primary_nodes.end());
    primary_nodes.erase(std::unique(primary_nodes.begin(), primary_nodes.end()), primary_nodes.end());
    std::sort(secondary_nodes.begin(), secondary_nodes.end());
    secondary_nodes.erase(std::unique(secondary_nodes.begin(), secondary_nodes.end()), secondary_nodes.end());

    const auto primary_count = primary_nodes.size();
    const auto secondary_count = secondary_nodes.size();
    const auto size = primary_count + secondary_count;
    if (size == 0) {
        return {};
    }

    std::unordered_map<Address, std::size_t> primary_index;
    std::unordered_map<Address, std::size_t> secondary_index;
    for (std::size_t i = 0; i < primary_nodes.size(); ++i) primary_index.emplace(primary_nodes[i], i);
    for (std::size_t i = 0; i < secondary_nodes.size(); ++i) secondary_index.emplace(secondary_nodes[i], i);

    constexpr double forbidden = 1.0e12;
    const double cardinality_bonus = static_cast<double>(size + 1);
    std::vector<std::vector<double>> cost(size + 1, std::vector<double>(size + 1, 0.0));
    for (std::size_t row = 1; row <= primary_count; ++row) {
        for (std::size_t column = 1; column <= secondary_count; ++column) {
            cost[row][column] = forbidden;
        }
    }

    std::map<std::pair<std::size_t, std::size_t>, std::size_t> edge_by_position;
    for (std::size_t edge_index = 0; edge_index < edges.size(); ++edge_index) {
        const auto row = primary_index.at(edges[edge_index].primary) + 1;
        const auto column = secondary_index.at(edges[edge_index].secondary) + 1;
        cost[row][column] = -(cardinality_bonus + edges[edge_index].ratio);
        edge_by_position[{row, column}] = edge_index;
    }

    std::vector<double> row_potential(size + 1, 0.0);
    std::vector<double> column_potential(size + 1, 0.0);
    std::vector<std::size_t> matched_row(size + 1, 0);
    std::vector<std::size_t> previous_column(size + 1, 0);

    for (std::size_t row = 1; row <= size; ++row) {
        matched_row[0] = row;
        std::size_t column = 0;
        std::vector<double> minimum(size + 1, std::numeric_limits<double>::infinity());
        std::vector<bool> used(size + 1, false);
        do {
            used[column] = true;
            const auto current_row = matched_row[column];
            double delta = std::numeric_limits<double>::infinity();
            std::size_t next_column = 0;
            for (std::size_t candidate_column = 1; candidate_column <= size; ++candidate_column) {
                if (used[candidate_column]) continue;
                const auto reduced = cost[current_row][candidate_column]
                    - row_potential[current_row] - column_potential[candidate_column];
                if (reduced < minimum[candidate_column]) {
                    minimum[candidate_column] = reduced;
                    previous_column[candidate_column] = column;
                }
                if (minimum[candidate_column] < delta) {
                    delta = minimum[candidate_column];
                    next_column = candidate_column;
                }
            }
            for (std::size_t candidate_column = 0; candidate_column <= size; ++candidate_column) {
                if (used[candidate_column]) {
                    row_potential[matched_row[candidate_column]] += delta;
                    column_potential[candidate_column] -= delta;
                } else {
                    minimum[candidate_column] -= delta;
                }
            }
            column = next_column;
        } while (matched_row[column] != 0);

        do {
            const auto previous = previous_column[column];
            matched_row[column] = matched_row[previous];
            column = previous;
        } while (column != 0);
    }

    std::vector<std::size_t> selected;
    for (std::size_t column = 1; column <= secondary_count; ++column) {
        const auto row = matched_row[column];
        if (row == 0 || row > primary_count) continue;
        const auto edge = edge_by_position.find({row, column});
        if (edge != edge_by_position.end()) {
            selected.push_back(edge->second);
        }
    }
    return selected;
}

std::vector<std::size_t> greedy_select(const std::vector<UniqueEdge>& edges)
{
    std::vector<std::size_t> order(edges.size());
    for (std::size_t i = 0; i < edges.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](const auto left, const auto right) {
        if (edges[left].ratio != edges[right].ratio) return edges[left].ratio > edges[right].ratio;
        if (edges[left].primary != edges[right].primary) return edges[left].primary < edges[right].primary;
        return edges[left].secondary < edges[right].secondary;
    });

    std::unordered_set<Address> used_primary;
    std::unordered_set<Address> used_secondary;
    std::vector<std::size_t> selected;
    for (const auto index : order) {
        const auto& edge = edges[index];
        if (used_primary.count(edge.primary) || used_secondary.count(edge.secondary)) continue;
        used_primary.insert(edge.primary);
        used_secondary.insert(edge.secondary);
        selected.push_back(index);
    }
    return selected;
}

} // namespace

WeightedMatchResolution resolve_weighted_matches(
    const std::vector<WeightedMatchCandidate>& candidates,
    std::size_t exact_component_node_limit,
    double tie_epsilon)
{
    WeightedMatchResolution result;
    if (candidates.empty()) return result;

    std::map<std::pair<Address, Address>, UniqueEdge> unique_by_pair;
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        const auto& candidate = candidates[index];
        const auto key = std::make_pair(candidate.primary, candidate.secondary);
        const auto existing = unique_by_pair.find(key);
        if (existing == unique_by_pair.end() || candidate.ratio > existing->second.ratio) {
            unique_by_pair[key] = {candidate.primary, candidate.secondary, candidate.ratio, index};
        }
    }

    std::vector<UniqueEdge> edges;
    edges.reserve(unique_by_pair.size());
    for (const auto& [key, edge] : unique_by_pair) {
        (void)key;
        edges.push_back(edge);
    }

    std::unordered_map<Address, std::vector<std::size_t>> by_primary;
    std::unordered_map<Address, std::vector<std::size_t>> by_secondary;
    for (std::size_t i = 0; i < edges.size(); ++i) {
        by_primary[edges[i].primary].push_back(i);
        by_secondary[edges[i].secondary].push_back(i);
    }

    std::vector<bool> visited(edges.size(), false);
    for (std::size_t root = 0; root < edges.size(); ++root) {
        if (visited[root]) continue;
        std::queue<std::size_t> queue;
        std::vector<std::size_t> component;
        queue.push(root);
        visited[root] = true;
        while (!queue.empty()) {
            const auto edge_index = queue.front();
            queue.pop();
            component.push_back(edge_index);
            const auto& edge = edges[edge_index];
            for (const auto adjacent : by_primary[edge.primary]) {
                if (!visited[adjacent]) {
                    visited[adjacent] = true;
                    queue.push(adjacent);
                }
            }
            for (const auto adjacent : by_secondary[edge.secondary]) {
                if (!visited[adjacent]) {
                    visited[adjacent] = true;
                    queue.push(adjacent);
                }
            }
        }

        std::unordered_map<Address, double> max_primary;
        std::unordered_map<Address, double> max_secondary;
        for (const auto edge_index : component) {
            const auto& edge = edges[edge_index];
            max_primary[edge.primary] = std::max(max_primary[edge.primary], edge.ratio);
            max_secondary[edge.secondary] = std::max(max_secondary[edge.secondary], edge.ratio);
        }
        std::unordered_map<Address, std::size_t> top_primary_count;
        std::unordered_map<Address, std::size_t> top_secondary_count;
        for (const auto edge_index : component) {
            const auto& edge = edges[edge_index];
            if (std::abs(edge.ratio - max_primary[edge.primary]) <= tie_epsilon) ++top_primary_count[edge.primary];
            if (std::abs(edge.ratio - max_secondary[edge.secondary]) <= tie_epsilon) ++top_secondary_count[edge.secondary];
        }

        std::unordered_set<Address> blocked_primary;
        std::unordered_set<Address> blocked_secondary;
        for (const auto edge_index : component) {
            const auto& edge = edges[edge_index];
            const bool tied_primary = top_primary_count[edge.primary] > 1
                && std::abs(edge.ratio - max_primary[edge.primary]) <= tie_epsilon;
            const bool tied_secondary = top_secondary_count[edge.secondary] > 1
                && std::abs(edge.ratio - max_secondary[edge.secondary]) <= tie_epsilon;
            if (tied_primary || tied_secondary) {
                result.multimatches.push_back(edge.original_index);
                blocked_primary.insert(edge.primary);
                blocked_secondary.insert(edge.secondary);
            }
        }

        std::vector<UniqueEdge> assignable;
        std::unordered_set<Address> primary_nodes;
        std::unordered_set<Address> secondary_nodes;
        for (const auto edge_index : component) {
            const auto& edge = edges[edge_index];
            if (blocked_primary.count(edge.primary) || blocked_secondary.count(edge.secondary)) continue;
            assignable.push_back(edge);
            primary_nodes.insert(edge.primary);
            secondary_nodes.insert(edge.secondary);
        }
        if (assignable.empty()) continue;

        const bool exact = primary_nodes.size() <= exact_component_node_limit
            && secondary_nodes.size() <= exact_component_node_limit;
        const auto selected_local = exact
            ? hungarian_select(assignable)
            : greedy_select(assignable);
        if (!exact) ++result.greedy_fallback_components;
        for (const auto local_index : selected_local) {
            result.selected.push_back(assignable[local_index].original_index);
        }
    }

    std::sort(result.selected.begin(), result.selected.end());
    std::sort(result.multimatches.begin(), result.multimatches.end());
    result.multimatches.erase(
        std::unique(result.multimatches.begin(), result.multimatches.end()),
        result.multimatches.end());
    return result;
}

} // namespace soff::diff
