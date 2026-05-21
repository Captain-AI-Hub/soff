#include "soff/diff/ratio.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace soff::diff {
namespace {

std::vector<std::string_view> split_lines(std::string_view text)
{
    std::vector<std::string_view> lines;
    while (!text.empty()) {
        const auto pos = text.find('\n');
        if (pos == std::string_view::npos) {
            lines.push_back(text);
            break;
        }
        lines.push_back(text.substr(0, pos));
        text.remove_prefix(pos + 1);
    }
    return lines;
}

double ratio_or_zero(std::string_view left, std::string_view right)
{
    if (left.empty() || right.empty()) {
        return 0.0;
    }
    return sequence_matcher_quick_ratio(left, right);
}

std::string normalize_decimal(std::string_view text)
{
    if (text.empty()) {
        throw std::runtime_error("empty integer");
    }
    std::string value;
    bool seen_non_zero = false;
    for (const auto ch : text) {
        if (ch < '0' || ch > '9') {
            throw std::runtime_error("invalid decimal integer");
        }
        if (ch != '0' || seen_non_zero) {
            value.push_back(ch);
            seen_non_zero = true;
        }
    }
    return value.empty() ? "0" : value;
}

bool decimal_less_than_uint(const std::string& value, unsigned rhs)
{
    const auto rhs_text = std::to_string(rhs);
    if (value.size() != rhs_text.size()) {
        return value.size() < rhs_text.size();
    }
    return value < rhs_text;
}

unsigned decimal_mod_uint(const std::string& value, unsigned divisor)
{
    unsigned remainder = 0;
    for (const auto ch : value) {
        remainder = (remainder * 10U + static_cast<unsigned>(ch - '0')) % divisor;
    }
    return remainder;
}

std::string decimal_div_uint(const std::string& value, unsigned divisor)
{
    std::string quotient;
    unsigned remainder = 0;
    bool seen_non_zero = false;
    for (const auto ch : value) {
        const auto current = remainder * 10U + static_cast<unsigned>(ch - '0');
        const auto digit = current / divisor;
        remainder = current % divisor;
        if (digit != 0 || seen_non_zero) {
            quotient.push_back(static_cast<char>('0' + digit));
            seen_non_zero = true;
        }
    }
    return quotient.empty() ? "0" : quotient;
}

std::vector<unsigned> primes_below(unsigned limit)
{
    std::vector<bool> composite(limit + 1, false);
    std::vector<unsigned> primes;
    for (unsigned i = 2; i < limit; ++i) {
        if (composite[i]) {
            continue;
        }
        primes.push_back(i);
        if (i * i <= limit) {
            for (unsigned j = i * i; j < limit; j += i) {
                composite[j] = true;
            }
        }
    }
    return primes;
}

std::unordered_map<std::string, unsigned> factorization(std::string value)
{
    std::unordered_map<std::string, unsigned> factors;
    value = normalize_decimal(value);
    if (decimal_less_than_uint(value, 2)) {
        return factors;
    }

    static const auto small_primes = primes_below(10000);
    for (const auto prime : small_primes) {
        if (decimal_less_than_uint(value, prime * prime)) {
            break;
        }
        while (decimal_mod_uint(value, prime) == 0) {
            ++factors[std::to_string(prime)];
            value = decimal_div_uint(value, prime);
        }
    }

    if (!decimal_less_than_uint(value, 2)) {
        ++factors[value];
    }
    return factors;
}

double rounded_7(double value)
{
    return std::round(value * 10000000.0) / 10000000.0;
}

} // namespace

double line_lcs_ratio(std::string_view left, std::string_view right)
{
    if (left.empty() || right.empty()) {
        return 0.0;
    }
    if (left == right) {
        return 1.0;
    }

    const auto left_lines = split_lines(left);
    const auto right_lines = split_lines(right);
    if (left_lines.empty() || right_lines.empty()) {
        return 0.0;
    }

    std::vector<std::size_t> previous(right_lines.size() + 1, 0);
    std::vector<std::size_t> current(right_lines.size() + 1, 0);
    for (std::size_t i = 0; i < left_lines.size(); ++i) {
        for (std::size_t j = 0; j < right_lines.size(); ++j) {
            if (left_lines[i] == right_lines[j]) {
                current[j + 1] = previous[j] + 1;
            } else {
                current[j + 1] = std::max(previous[j + 1], current[j]);
            }
        }
        std::swap(previous, current);
        std::fill(current.begin(), current.end(), 0);
    }

    const auto lcs = previous.back();
    return (2.0 * static_cast<double>(lcs)) / static_cast<double>(left_lines.size() + right_lines.size());
}

double sequence_matcher_quick_ratio(std::string_view left, std::string_view right)
{
    if (left.empty() || right.empty()) {
        return 0.0;
    }
    if (left == right) {
        return 1.0;
    }

    const auto left_lines = split_lines(left);
    const auto right_lines = split_lines(right);
    if (left_lines.empty() || right_lines.empty()) {
        return 0.0;
    }

    std::unordered_map<std::string_view, std::size_t> available;
    for (const auto line : right_lines) {
        ++available[line];
    }

    std::size_t matches = 0;
    for (const auto line : left_lines) {
        auto it = available.find(line);
        if (it == available.end() || it->second == 0) {
            continue;
        }
        --it->second;
        ++matches;
    }

    return (2.0 * static_cast<double>(matches)) / static_cast<double>(left_lines.size() + right_lines.size());
}

double ast_prime_difference_ratio(std::string_view left, std::string_view right)
{
    if (left.empty() || right.empty()) {
        return 0.0;
    }
    if (left == right) {
        return 1.0;
    }

    const auto left_factors = factorization(std::string(left));
    const auto right_factors = factorization(std::string(right));

    unsigned diff_total = 0;
    unsigned left_total = 0;
    unsigned right_total = 0;
    for (const auto& [factor, count] : left_factors) {
        left_total += count;
        const auto right_it = right_factors.find(factor);
        const auto right_count = right_it != right_factors.end() ? right_it->second : 0U;
        diff_total += count > right_count ? count - right_count : right_count - count;
    }
    for (const auto& [factor, count] : right_factors) {
        right_total += count;
        if (left_factors.find(factor) == left_factors.end()) {
            diff_total += count;
        }
    }

    const auto total = std::max(left_total, right_total);
    if (total == 0) {
        return 0.0;
    }
    return 1.0 - (static_cast<double>(diff_total) / static_cast<double>(total));
}

double candidate_text_ratio(
    std::string_view assembly_left,
    std::string_view assembly_right,
    std::string_view pseudo_left,
    std::string_view pseudo_right,
    std::string_view clean_assembly_left,
    std::string_view clean_assembly_right,
    std::string_view clean_pseudo_left,
    std::string_view clean_pseudo_right)
{
    return rounded_7(std::max({
        ratio_or_zero(clean_assembly_left, clean_assembly_right),
        ratio_or_zero(clean_pseudo_left, clean_pseudo_right),
        ratio_or_zero(assembly_left, assembly_right),
        ratio_or_zero(pseudo_left, pseudo_right),
    }));
}

} // namespace soff::diff
