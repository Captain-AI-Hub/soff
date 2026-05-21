#include "soff/ui/line_diff.hpp"
#include <algorithm>

namespace soff::ui {

std::vector<DiffEntry> compute_line_diff(
    const std::vector<std::string>& left,
    const std::vector<std::string>& right,
    std::size_t max_lines)
{
    const auto m = left.size();
    const auto n = right.size();

    // Guard: if either side is too large, skip LCS
    if (m > max_lines || n > max_lines) {
        std::vector<DiffEntry> result;
        result.reserve(m + n);
        for (std::size_t i = 0; i < m; ++i) {
            result.push_back({DiffEntry::removed, i, 0});
        }
        for (std::size_t j = 0; j < n; ++j) {
            result.push_back({DiffEntry::added, 0, j});
        }
        return result;
    }

    if (m == 0 && n == 0) return {};
    if (m == 0) {
        std::vector<DiffEntry> result;
        for (std::size_t j = 0; j < n; ++j)
            result.push_back({DiffEntry::added, 0, j});
        return result;
    }
    if (n == 0) {
        std::vector<DiffEntry> result;
        for (std::size_t i = 0; i < m; ++i)
            result.push_back({DiffEntry::removed, i, 0});
        return result;
    }

    // LCS DP table
    std::vector<std::vector<int>> dp(m + 1, std::vector<int>(n + 1, 0));
    for (std::size_t i = 1; i <= m; ++i) {
        for (std::size_t j = 1; j <= n; ++j) {
            if (left[i - 1] == right[j - 1]) {
                dp[i][j] = dp[i - 1][j - 1] + 1;
            } else {
                dp[i][j] = std::max(dp[i - 1][j], dp[i][j - 1]);
            }
        }
    }

    // Backtrack
    std::vector<DiffEntry> diff;
    std::size_t i = m, j = n;
    while (i > 0 || j > 0) {
        if (i > 0 && j > 0 && left[i - 1] == right[j - 1]) {
            diff.push_back({DiffEntry::same, i - 1, j - 1});
            --i; --j;
        } else if (j > 0 && (i == 0 || dp[i][j - 1] >= dp[i - 1][j])) {
            diff.push_back({DiffEntry::added, 0, j - 1});
            --j;
        } else {
            diff.push_back({DiffEntry::removed, i - 1, 0});
            --i;
        }
    }
    std::reverse(diff.begin(), diff.end());
    return diff;
}

} // namespace soff::ui