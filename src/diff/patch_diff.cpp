#include "soff/diff/patch_diff.hpp"

#include <algorithm>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace soff::diff {
namespace {

const std::vector<std::string>& unsafe_patterns()
{
    static const std::vector<std::string> patterns = {
        "cpy", "printf", "strcat", "strncat", "gets", "mem",
        "system", "scanf", "alloc", "free", "strto",
        "ShellExecute", "WinExec", "LoadLibrary", "CreateProcess",
        "ProbeForWrite", "ProbeForRead", "UNC"
    };
    return patterns;
}

using SignMap = std::unordered_map<std::string, std::string>;

const SignMap& signed_unsigned_map()
{
    static const SignMap map = {
        {"jl", "jb"}, {"jb", "jl"},
        {"jle", "jbe"}, {"jbe", "jle"},
        {"jg", "ja"}, {"ja", "jg"},
        {"jge", "jae"}, {"jae", "jge"},
    };
    return map;
}

std::vector<std::string> split_lines(const std::string& text)
{
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(std::move(line));
    }
    return lines;
}

} // namespace

bool detect_signed_unsigned_change(
    const std::string& asm_primary,
    const std::string& asm_secondary,
    std::string& detail)
{
    if (asm_primary.empty() || asm_secondary.empty()) return false;

    const auto lines1 = split_lines(asm_primary);
    const auto lines2 = split_lines(asm_secondary);
    const auto& map = signed_unsigned_map();

    for (std::size_t i = 0; i < std::min(lines1.size(), lines2.size()); ++i) {
        const auto& l1 = lines1[i];
        const auto& l2 = lines2[i];
        if (l1 == l2) continue;

        auto mnem1 = l1.substr(0, l1.find(' '));
        auto mnem2 = l2.substr(0, l2.find(' '));
        std::transform(mnem1.begin(), mnem1.end(), mnem1.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::transform(mnem2.begin(), mnem2.end(), mnem2.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        const auto it = map.find(mnem1);
        if (it != map.end() && it->second == mnem2) {
            detail = mnem1 + " -> " + mnem2;
            return true;
        }
    }
    return false;
}

bool detect_unsafe_function_pattern(
    const std::string& pseudo_primary,
    const std::string& pseudo_secondary,
    std::string& detail)
{
    if (pseudo_primary.empty() || pseudo_secondary.empty()) return false;

    const auto lines1 = split_lines(pseudo_primary);
    const auto lines2 = split_lines(pseudo_secondary);

    for (const auto& line : lines2) {
        for (const auto& pattern : unsafe_patterns()) {
            if (line.find(pattern) != std::string::npos) {
                bool in_primary = false;
                for (const auto& pl : lines1) {
                    if (pl.find(pattern) != std::string::npos) {
                        in_primary = true;
                        break;
                    }
                }
                if (!in_primary) {
                    detail = "Pattern '" + pattern + "' added";
                    return true;
                }
            }
        }
    }
    return false;
}

bool detect_size_check_added(
    const std::string& pseudo_primary,
    const std::string& pseudo_secondary,
    std::string& detail)
{
    if (pseudo_secondary.empty()) return false;

    const auto lines2 = split_lines(pseudo_secondary);
    const auto lines1 = split_lines(pseudo_primary);

    for (const auto& line : lines2) {
        const auto trimmed = [&]() {
            auto s = line;
            const auto pos = s.find_first_not_of(" \t");
            return pos != std::string::npos ? s.substr(pos) : s;
        }();

        if (trimmed.substr(0, 3) != "if ") continue;

        bool has_comparison = false;
        for (const auto& cmp : {" < ", " > ", " <= ", " >= "}) {
            if (trimmed.find(cmp) != std::string::npos) {
                if (trimmed.find(std::string(cmp) + "0 ") == std::string::npos) {
                    has_comparison = true;
                    break;
                }
            }
        }
        if (!has_comparison) continue;

        bool in_primary = false;
        for (const auto& pl : lines1) {
            if (pl.find(trimmed) != std::string::npos) {
                in_primary = true;
                break;
            }
        }
        if (!in_primary) {
            detail = "Size check: " + trimmed;
            return true;
        }
    }
    return false;
}

PatchDiffResult analyze_patch_diff(
    db::Database& database,
    const std::vector<db::ResultMatch>& matches)
{
    PatchDiffResult result;

    for (const auto& match : matches) {
        if (match.ratio >= 1.0) continue;

        const auto primary_rows = database.query_rows(
            "select assembly, pseudocode from functions where address = '"
            + std::to_string(match.primary) + "' limit 1");
        const auto secondary_rows = database.query_rows(
            "select assembly, pseudocode from diff.functions where address = '"
            + std::to_string(match.secondary) + "' limit 1");

        if (primary_rows.empty() || secondary_rows.empty()
            || primary_rows.front().size() < 2
            || secondary_rows.front().size() < 2) {
            continue;
        }

        const auto& asm1 = primary_rows.front()[0];
        const auto& asm2 = secondary_rows.front()[0];
        const auto& pseudo1 = primary_rows.front()[1];
        const auto& pseudo2 = secondary_rows.front()[1];

        std::string detail;
        if (detect_signed_unsigned_change(asm1, asm2, detail)) {
            PatchDiffFinding finding;
            finding.primary = match.primary;
            finding.primary_name = match.primary_name;
            finding.secondary = match.secondary;
            finding.secondary_name = match.secondary_name;
            finding.ratio = match.ratio;
            finding.heuristic = match.description;
            finding.indicator = VulnIndicator::signed_unsigned_change;
            finding.detail = std::move(detail);
            result.findings.push_back(std::move(finding));
            continue;
        }

        if (detect_unsafe_function_pattern(pseudo1, pseudo2, detail)) {
            PatchDiffFinding finding;
            finding.primary = match.primary;
            finding.primary_name = match.primary_name;
            finding.secondary = match.secondary;
            finding.secondary_name = match.secondary_name;
            finding.ratio = match.ratio;
            finding.heuristic = match.description;
            finding.indicator = VulnIndicator::unsafe_function_pattern;
            finding.detail = std::move(detail);
            result.findings.push_back(std::move(finding));
            continue;
        }

        if (detect_size_check_added(pseudo1, pseudo2, detail)) {
            PatchDiffFinding finding;
            finding.primary = match.primary;
            finding.primary_name = match.primary_name;
            finding.secondary = match.secondary;
            finding.secondary_name = match.secondary_name;
            finding.ratio = match.ratio;
            finding.heuristic = match.description;
            finding.indicator = VulnIndicator::size_check_added;
            finding.detail = std::move(detail);
            result.findings.push_back(std::move(finding));
        }
    }

    return result;
}

MatchDecision PatchDiffHook::on_match(const MatchContext& context)
{
    if (context.ratio >= 1.0 || database_ == nullptr) {
        return {true, context.ratio};
    }

    const auto primary_rows = database_->query_rows(
        "select assembly, pseudocode from functions where address = '"
        + std::to_string(context.primary) + "' limit 1");
    const auto secondary_rows = database_->query_rows(
        "select assembly, pseudocode from diff.functions where address = '"
        + std::to_string(context.secondary) + "' limit 1");

    if (primary_rows.empty() || secondary_rows.empty()
        || primary_rows.front().size() < 2
        || secondary_rows.front().size() < 2) {
        return {true, context.ratio};
    }

    const auto& asm1 = primary_rows.front()[0];
    const auto& asm2 = secondary_rows.front()[0];
    const auto& pseudo1 = primary_rows.front()[1];
    const auto& pseudo2 = secondary_rows.front()[1];

    std::string detail;
    VulnIndicator indicator = VulnIndicator::unsafe_function_pattern;

    bool found = false;
    if (detect_signed_unsigned_change(asm1, asm2, detail)) {
        indicator = VulnIndicator::signed_unsigned_change;
        found = true;
    } else if (detect_unsafe_function_pattern(pseudo1, pseudo2, detail)) {
        indicator = VulnIndicator::unsafe_function_pattern;
        found = true;
    } else if (detect_size_check_added(pseudo1, pseudo2, detail)) {
        indicator = VulnIndicator::size_check_added;
        found = true;
    }

    if (found) {
        PatchDiffFinding finding;
        finding.primary = context.primary;
        finding.primary_name = context.primary_name;
        finding.secondary = context.secondary;
        finding.secondary_name = context.secondary_name;
        finding.ratio = context.ratio;
        finding.heuristic = context.description;
        finding.indicator = indicator;
        finding.detail = std::move(detail);
        result_.findings.push_back(std::move(finding));
    }

    return {true, context.ratio};
}

} // namespace soff::diff
