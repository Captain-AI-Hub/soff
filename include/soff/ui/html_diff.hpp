#pragma once

#include "soff/analysis/model.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace soff::ui {

enum class DiffLineKind
{
    equal,
    changed,
    inserted,
    deleted,
};

struct HtmlDiffSection
{
    std::string title;
    std::string primary_text;
    std::string secondary_text;
};

struct HtmlDiffDocument
{
    std::string title;
    std::string primary_name;
    std::string secondary_name;
    Address primary_address = 0;
    Address secondary_address = 0;
    double ratio = 0.0;
    std::string description;
    std::vector<HtmlDiffSection> sections;
};

enum class GraphDiffKind
{
    native,
    microcode,
    pseudocode,
};

std::string html_escape(std::string_view text);
std::string render_html_diff(const HtmlDiffDocument& document);
std::string render_function_graph_diff_html(
    const FunctionFeature& primary,
    const FunctionFeature& secondary,
    GraphDiffKind kind,
    double ratio,
    std::string_view description);
std::string render_call_context_diff_html(
    const ProgramSnapshot& primary_snapshot,
    const ProgramSnapshot& secondary_snapshot,
    Address primary_address,
    Address secondary_address,
    double ratio,
    std::string_view description);

HtmlDiffDocument build_function_diff_document(
    const FunctionFeature& primary,
    const FunctionFeature& secondary,
    double ratio,
    std::string description);

} // namespace soff::ui
