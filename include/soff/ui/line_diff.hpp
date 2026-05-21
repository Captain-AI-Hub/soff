#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace soff::ui {

struct DiffEntry {
    enum Kind { same, removed, added };
    Kind kind;
    std::size_t left_index;
    std::size_t right_index;
};

/// Compute a line-level LCS diff between two sequences.
/// Returns aligned entries: 'same' has valid left_index and right_index,
/// 'removed' has valid left_index only, 'added' has valid right_index only.
/// If either side exceeds max_lines, falls back to marking all as removed+added.
std::vector<DiffEntry> compute_line_diff(
    const std::vector<std::string>& left,
    const std::vector<std::string>& right,
    std::size_t max_lines = 2000);

} // namespace soff::ui
