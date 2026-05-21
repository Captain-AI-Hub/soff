#pragma once

#include <string_view>

namespace soff::diff {

double line_lcs_ratio(std::string_view left, std::string_view right);
double sequence_matcher_quick_ratio(std::string_view left, std::string_view right);
double ast_prime_difference_ratio(std::string_view left, std::string_view right);
double candidate_text_ratio(
    std::string_view assembly_left,
    std::string_view assembly_right,
    std::string_view pseudo_left,
    std::string_view pseudo_right,
    std::string_view clean_assembly_left,
    std::string_view clean_assembly_right,
    std::string_view clean_pseudo_left,
    std::string_view clean_pseudo_right);

} // namespace soff::diff
