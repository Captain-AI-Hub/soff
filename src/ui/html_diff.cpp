#include "soff/ui/html_diff.hpp"

#include <algorithm>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>

namespace soff::ui {
namespace {

std::vector<std::string> split_lines(std::string_view text)
{
    std::vector<std::string> lines;
    std::string current;
    for (const char ch : text) {
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            lines.push_back(std::move(current));
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty() || (!text.empty() && text.back() == '\n')) {
        lines.push_back(std::move(current));
    }
    return lines;
}

struct DiffRow
{
    DiffLineKind kind = DiffLineKind::equal;
    std::string left;
    std::string right;
    std::size_t left_line = 0;
    std::size_t right_line = 0;
};

std::vector<DiffRow> line_diff(const std::vector<std::string>& left, const std::vector<std::string>& right)
{
    const auto rows = left.size();
    const auto cols = right.size();
    std::vector<std::size_t> lcs((rows + 1) * (cols + 1), 0);
    const auto at = [&](std::size_t row, std::size_t col) -> std::size_t& {
        return lcs[row * (cols + 1) + col];
    };

    for (std::size_t row = rows; row > 0; --row) {
        for (std::size_t col = cols; col > 0; --col) {
            if (left[row - 1] == right[col - 1]) {
                at(row - 1, col - 1) = at(row, col) + 1;
            } else {
                at(row - 1, col - 1) = std::max(at(row, col - 1), at(row - 1, col));
            }
        }
    }

    std::vector<DiffRow> diff;
    std::size_t left_index = 0;
    std::size_t right_index = 0;
    while (left_index < rows || right_index < cols) {
        if (left_index < rows && right_index < cols && left[left_index] == right[right_index]) {
            diff.push_back({
                DiffLineKind::equal,
                left[left_index],
                right[right_index],
                left_index + 1,
                right_index + 1,
            });
            ++left_index;
            ++right_index;
        } else if (left_index < rows && right_index < cols
            && (left_index + 1 == rows || right_index + 1 == cols || at(left_index + 1, right_index + 1) >= at(left_index + 1, right_index) + at(left_index, right_index + 1))) {
            diff.push_back({
                DiffLineKind::changed,
                left[left_index],
                right[right_index],
                left_index + 1,
                right_index + 1,
            });
            ++left_index;
            ++right_index;
        } else if (right_index < cols && (left_index == rows || at(left_index, right_index + 1) >= at(left_index + 1, right_index))) {
            diff.push_back({
                DiffLineKind::inserted,
                "",
                right[right_index],
                0,
                right_index + 1,
            });
            ++right_index;
        } else if (left_index < rows) {
            diff.push_back({
                DiffLineKind::deleted,
                left[left_index],
                "",
                left_index + 1,
                0,
            });
            ++left_index;
        }
    }
    return diff;
}

const char* kind_class(DiffLineKind kind)
{
    switch (kind) {
    case DiffLineKind::equal:
        return "eq";
    case DiffLineKind::changed:
        return "chg";
    case DiffLineKind::inserted:
        return "ins";
    case DiffLineKind::deleted:
        return "del";
    }
    return "eq";
}

std::string address_text(Address address)
{
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << address;
    return out.str();
}

void append_section(std::ostringstream& out, const HtmlDiffSection& section)
{
    const auto left = split_lines(section.primary_text);
    const auto right = split_lines(section.secondary_text);
    out << "<section>\n<h2>" << html_escape(section.title) << "</h2>\n";
    if (left.empty() && right.empty()) {
        out << "<p class=\"empty\">No data exported for this view.</p>\n</section>\n";
        return;
    }
    out << "<table class=\"diff\"><thead><tr>"
        << "<th class=\"ln\">#</th><th>Primary</th><th class=\"ln\">#</th><th>Secondary</th>"
        << "</tr></thead><tbody>\n";
    for (const auto& row : line_diff(left, right)) {
        out << "<tr class=\"" << kind_class(row.kind) << "\">";
        out << "<td class=\"ln\">" << (row.left_line == 0 ? "" : std::to_string(row.left_line)) << "</td>";
        out << "<td><pre>" << html_escape(row.left) << "</pre></td>";
        out << "<td class=\"ln\">" << (row.right_line == 0 ? "" : std::to_string(row.right_line)) << "</td>";
        out << "<td><pre>" << html_escape(row.right) << "</pre></td>";
        out << "</tr>\n";
    }
    out << "</tbody></table>\n</section>\n";
}

std::string ordinal_successors_text(const std::vector<int>& successors)
{
    if (successors.empty()) {
        return "-";
    }
    std::ostringstream out;
    for (std::size_t i = 0; i < successors.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << "#" << successors[i];
    }
    return out.str();
}

struct GraphNodeSummary
{
    Address start = 0;
    Address end = 0;
    std::size_t instructions = 0;
    std::vector<int> successors;
};

std::vector<GraphNodeSummary> summarize_blocks(const std::vector<BasicBlock>& blocks)
{
    std::map<Address, int> ordinals;
    for (std::size_t index = 0; index < blocks.size(); ++index) {
        ordinals[blocks[index].start] = static_cast<int>(index);
    }

    std::vector<GraphNodeSummary> summaries;
    summaries.reserve(blocks.size());
    for (const auto& block : blocks) {
        GraphNodeSummary summary;
        summary.start = block.start;
        summary.end = block.end;
        summary.instructions = block.instructions.size();
        for (const auto successor : block.successors) {
            const auto found = ordinals.find(successor);
            if (found != ordinals.end()) {
                summary.successors.push_back(found->second);
            }
        }
        std::sort(summary.successors.begin(), summary.successors.end());
        summaries.push_back(std::move(summary));
    }
    return summaries;
}

std::set<std::pair<int, int>> edge_set(const std::vector<GraphNodeSummary>& nodes)
{
    std::set<std::pair<int, int>> edges;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        for (const auto successor : nodes[index].successors) {
            edges.emplace(static_cast<int>(index), successor);
        }
    }
    return edges;
}

std::string edge_set_text(const std::set<std::pair<int, int>>& edges)
{
    if (edges.empty()) {
        return "-";
    }
    std::ostringstream out;
    bool first = true;
    for (const auto& [source, target] : edges) {
        if (!first) {
            out << ", ";
        }
        first = false;
        out << "#" << source << "->#" << target;
    }
    return out.str();
}

bool graph_node_equal(const GraphNodeSummary& left, const GraphNodeSummary& right)
{
    return left.instructions == right.instructions
        && left.successors == right.successors;
}

void append_graph_node_cell(std::ostringstream& out, const std::vector<GraphNodeSummary>& nodes, std::size_t index)
{
    if (index >= nodes.size()) {
        out << "<td class=\"missing\">-</td>";
        return;
    }
    const auto& node = nodes[index];
    out << "<td><div class=\"node-title\">"
        << "#" << index << " " << html_escape(address_text(node.start));
    if (node.end != 0) {
        out << " - " << html_escape(address_text(node.end));
    }
    out << "</div><div>instructions=" << node.instructions
        << "</div><div>successors=" << html_escape(ordinal_successors_text(node.successors))
        << "</div></td>";
}

const FunctionFeature* find_function_by_address_or_range(const ProgramSnapshot& snapshot, Address address)
{
    for (const auto& function : snapshot.functions) {
        if (function.address == address) {
            return &function;
        }
        if (function.size != 0 && address >= function.address && address < function.address + function.size) {
            return &function;
        }
    }
    return nullptr;
}

std::string function_label(const FunctionFeature* function, Address fallback_address)
{
    std::ostringstream out;
    if (function != nullptr && !function->name.empty()) {
        out << function->name << " ";
    }
    out << address_text(function != nullptr ? function->address : fallback_address);
    return out.str();
}

struct CallContextEntry
{
    Address address = 0;
    std::string name;
    std::string call_type;
};

std::vector<CallContextEntry> callees_for_function(const ProgramSnapshot& snapshot, const FunctionFeature& function)
{
    std::vector<CallContextEntry> entries;
    entries.reserve(function.call_references.size());
    for (const auto& call : function.call_references) {
        const auto* target = find_function_by_address_or_range(snapshot, call.address);
        entries.push_back({
            target != nullptr ? target->address : call.address,
            function_label(target, call.address),
            call.type,
        });
    }
    return entries;
}

std::vector<CallContextEntry> callers_for_function(const ProgramSnapshot& snapshot, const FunctionFeature& focus)
{
    std::vector<CallContextEntry> entries;
    for (const auto& function : snapshot.functions) {
        for (const auto& call : function.call_references) {
            const auto* target = find_function_by_address_or_range(snapshot, call.address);
            const bool matches = target != nullptr
                ? target->address == focus.address
                : call.address == focus.address;
            if (!matches) {
                continue;
            }
            entries.push_back({
                function.address,
                function_label(&function, function.address),
                call.type,
            });
            break;
        }
    }
    return entries;
}

bool context_entry_equal(const CallContextEntry& left, const CallContextEntry& right)
{
    return left.name == right.name && left.call_type == right.call_type;
}

void append_context_cell(std::ostringstream& out, const std::vector<CallContextEntry>& entries, std::size_t index)
{
    if (index >= entries.size()) {
        out << "<td class=\"missing\">-</td>";
        return;
    }
    const auto& entry = entries[index];
    out << "<td><div class=\"node-title\">" << html_escape(entry.name)
        << "</div><div>address=" << html_escape(address_text(entry.address))
        << "</div><div>type=" << html_escape(entry.call_type.empty() ? "-" : entry.call_type)
        << "</div></td>";
}

void append_context_table(
    std::ostringstream& out,
    std::string_view title,
    const std::vector<CallContextEntry>& primary_entries,
    const std::vector<CallContextEntry>& secondary_entries)
{
    out << "<section><h2>" << html_escape(title) << "</h2>";
    if (primary_entries.empty() && secondary_entries.empty()) {
        out << "<p class=\"missing\">No call context exported for this side.</p></section>\n";
        return;
    }
    out << "<table><thead><tr><th class=\"index\">#</th><th>Primary</th><th>Secondary</th></tr></thead><tbody>\n";
    const auto rows = std::max(primary_entries.size(), secondary_entries.size());
    for (std::size_t index = 0; index < rows; ++index) {
        DiffLineKind row_kind = DiffLineKind::changed;
        if (index >= primary_entries.size()) {
            row_kind = DiffLineKind::inserted;
        } else if (index >= secondary_entries.size()) {
            row_kind = DiffLineKind::deleted;
        } else if (context_entry_equal(primary_entries[index], secondary_entries[index])) {
            row_kind = DiffLineKind::equal;
        }
        out << "<tr class=\"" << kind_class(row_kind) << "\"><td class=\"index\">#" << index << "</td>";
        append_context_cell(out, primary_entries, index);
        append_context_cell(out, secondary_entries, index);
        out << "</tr>\n";
    }
    out << "</tbody></table></section>\n";
}

} // namespace

std::string html_escape(std::string_view text)
{
    std::string escaped;
    escaped.reserve(text.size());
    for (const char ch : text) {
        switch (ch) {
        case '&':
            escaped += "&amp;";
            break;
        case '<':
            escaped += "&lt;";
            break;
        case '>':
            escaped += "&gt;";
            break;
        case '"':
            escaped += "&quot;";
            break;
        case '\'':
            escaped += "&#39;";
            break;
        default:
            escaped.push_back(ch);
            break;
        }
    }
    return escaped;
}

std::string render_html_diff(const HtmlDiffDocument& document)
{
    std::ostringstream out;
    out << "<!doctype html>\n<html><head><meta charset=\"utf-8\">\n"
        << "<title>" << html_escape(document.title) << "</title>\n"
        << "<style>"
        << "body{font-family:Segoe UI,Arial,sans-serif;margin:0;background:#f6f7f9;color:#1f2328;}"
        << "header{background:#20242c;color:#fff;padding:18px 24px;}"
        << "h1{font-size:20px;margin:0 0 8px 0;font-weight:600;}"
        << ".meta{font-size:13px;color:#cbd2dc;display:flex;gap:18px;flex-wrap:wrap;}"
        << "main{padding:18px 24px 28px;}"
        << "section{margin:0 0 20px;background:#fff;border:1px solid #d8dee4;}"
        << "h2{font-size:15px;margin:0;padding:10px 12px;background:#eef1f5;border-bottom:1px solid #d8dee4;}"
        << ".empty{margin:0;padding:12px;color:#667085;}"
        << "table.diff{border-collapse:collapse;width:100%;table-layout:fixed;font-family:Consolas,Menlo,monospace;font-size:12px;}"
        << "th{font-family:Segoe UI,Arial,sans-serif;text-align:left;background:#f6f8fa;border-bottom:1px solid #d8dee4;padding:6px;}"
        << "td{vertical-align:top;border-bottom:1px solid #edf0f3;padding:0;}"
        << "td pre{margin:0;padding:4px 8px;white-space:pre-wrap;word-break:break-word;}"
        << ".ln{width:54px;text-align:right;color:#667085;background:#f6f8fa;padding:4px 6px;}"
        << "tr.eq td{background:#fff;}"
        << "tr.chg td{background:#fff4ce;}"
        << "tr.ins td{background:#dafbe1;}"
        << "tr.del td{background:#ffebe9;}"
        << "</style></head><body>\n";
    out << "<header><h1>" << html_escape(document.title) << "</h1><div class=\"meta\">"
        << "<span>Primary: " << html_escape(document.primary_name) << " " << address_text(document.primary_address) << "</span>"
        << "<span>Secondary: " << html_escape(document.secondary_name) << " " << address_text(document.secondary_address) << "</span>"
        << "<span>Ratio: " << std::fixed << std::setprecision(6) << document.ratio << "</span>"
        << "<span>" << html_escape(document.description) << "</span>"
        << "</div></header>\n<main>\n";
    for (const auto& section : document.sections) {
        append_section(out, section);
    }
    out << "</main></body></html>\n";
    return out.str();
}

std::string render_function_graph_diff_html(
    const FunctionFeature& primary,
    const FunctionFeature& secondary,
    GraphDiffKind kind,
    double ratio,
    std::string_view description)
{
    const auto& primary_blocks = kind == GraphDiffKind::native ? primary.blocks : primary.microcode_blocks;
    const auto& secondary_blocks = kind == GraphDiffKind::native ? secondary.blocks : secondary.microcode_blocks;
    const auto primary_nodes = summarize_blocks(primary_blocks);
    const auto secondary_nodes = summarize_blocks(secondary_blocks);
    const auto primary_edges = edge_set(primary_nodes);
    const auto secondary_edges = edge_set(secondary_nodes);
    const auto graph_title = kind == GraphDiffKind::native ? "Native CFG" : "Microcode CFG";

    std::ostringstream out;
    out << "<!doctype html>\n<html><head><meta charset=\"utf-8\">\n"
        << "<title>" << html_escape(graph_title) << " " << html_escape(primary.name) << " vs " << html_escape(secondary.name) << "</title>\n"
        << "<style>"
        << "body{font-family:Segoe UI,Arial,sans-serif;margin:0;background:#f6f7f9;color:#1f2328;}"
        << "header{background:#20242c;color:#fff;padding:18px 24px;}"
        << "h1{font-size:20px;margin:0 0 8px 0;font-weight:600;}"
        << ".meta{font-size:13px;color:#cbd2dc;display:flex;gap:18px;flex-wrap:wrap;}"
        << "main{padding:18px 24px 28px;}"
        << "section{margin:0 0 20px;background:#fff;border:1px solid #d8dee4;}"
        << "h2{font-size:15px;margin:0;padding:10px 12px;background:#eef1f5;border-bottom:1px solid #d8dee4;}"
        << "table{border-collapse:collapse;width:100%;table-layout:fixed;font-size:13px;}"
        << "th{text-align:left;background:#f6f8fa;border-bottom:1px solid #d8dee4;padding:7px;}"
        << "td{vertical-align:top;border-bottom:1px solid #edf0f3;padding:7px;word-break:break-word;}"
        << ".index{width:64px;text-align:right;color:#667085;background:#f6f8fa;}"
        << ".node-title{font-family:Consolas,Menlo,monospace;font-weight:600;margin-bottom:4px;}"
        << ".missing{color:#667085;text-align:center;background:#f6f8fa;}"
        << "tr.eq td{background:#fff;}"
        << "tr.chg td{background:#fff4ce;}"
        << "tr.ins td{background:#dafbe1;}"
        << "tr.del td{background:#ffebe9;}"
        << ".summary{display:grid;grid-template-columns:1fr 1fr;gap:12px;padding:12px;}"
        << ".summary div{background:#f6f8fa;border:1px solid #d8dee4;padding:8px;}"
        << "</style></head><body>\n";
    out << "<header><h1>" << html_escape(graph_title) << "</h1><div class=\"meta\">"
        << "<span>Primary: " << html_escape(primary.name) << " " << address_text(primary.address) << "</span>"
        << "<span>Secondary: " << html_escape(secondary.name) << " " << address_text(secondary.address) << "</span>"
        << "<span>Ratio: " << std::fixed << std::setprecision(6) << ratio << "</span>"
        << "<span>" << html_escape(description) << "</span>"
        << "</div></header>\n<main>\n";

    out << "<section><h2>Summary</h2><div class=\"summary\">"
        << "<div><b>Primary</b><br>nodes=" << primary_nodes.size()
        << "<br>edges=" << primary_edges.size()
        << "<br>edges: " << html_escape(edge_set_text(primary_edges)) << "</div>"
        << "<div><b>Secondary</b><br>nodes=" << secondary_nodes.size()
        << "<br>edges=" << secondary_edges.size()
        << "<br>edges: " << html_escape(edge_set_text(secondary_edges)) << "</div>"
        << "</div></section>\n";

    out << "<section><h2>Blocks</h2>";
    if (primary_nodes.empty() && secondary_nodes.empty()) {
        out << "<p class=\"missing\">No graph blocks exported for this view.</p>";
    } else {
        out << "<table><thead><tr><th class=\"index\">#</th><th>Primary block</th><th>Secondary block</th></tr></thead><tbody>\n";
        const auto rows = std::max(primary_nodes.size(), secondary_nodes.size());
        for (std::size_t index = 0; index < rows; ++index) {
            DiffLineKind row_kind = DiffLineKind::changed;
            if (index >= primary_nodes.size()) {
                row_kind = DiffLineKind::inserted;
            } else if (index >= secondary_nodes.size()) {
                row_kind = DiffLineKind::deleted;
            } else if (graph_node_equal(primary_nodes[index], secondary_nodes[index])) {
                row_kind = DiffLineKind::equal;
            }
            out << "<tr class=\"" << kind_class(row_kind) << "\"><td class=\"index\">#" << index << "</td>";
            append_graph_node_cell(out, primary_nodes, index);
            append_graph_node_cell(out, secondary_nodes, index);
            out << "</tr>\n";
        }
        out << "</tbody></table>";
    }
    out << "</section>\n</main></body></html>\n";
    return out.str();
}

std::string render_call_context_diff_html(
    const ProgramSnapshot& primary_snapshot,
    const ProgramSnapshot& secondary_snapshot,
    Address primary_address,
    Address secondary_address,
    double ratio,
    std::string_view description)
{
    const auto* primary = find_function_by_address_or_range(primary_snapshot, primary_address);
    const auto* secondary = find_function_by_address_or_range(secondary_snapshot, secondary_address);
    if (primary == nullptr || secondary == nullptr) {
        return "<!doctype html><html><body><p>Matched functions are missing from export snapshots.</p></body></html>\n";
    }
    const auto primary_callers = callers_for_function(primary_snapshot, *primary);
    const auto secondary_callers = callers_for_function(secondary_snapshot, *secondary);
    const auto primary_callees = callees_for_function(primary_snapshot, *primary);
    const auto secondary_callees = callees_for_function(secondary_snapshot, *secondary);

    std::ostringstream out;
    out << "<!doctype html>\n<html><head><meta charset=\"utf-8\">\n"
        << "<title>Call Context " << html_escape(primary->name) << " vs " << html_escape(secondary->name) << "</title>\n"
        << "<style>"
        << "body{font-family:Segoe UI,Arial,sans-serif;margin:0;background:#f6f7f9;color:#1f2328;}"
        << "header{background:#20242c;color:#fff;padding:18px 24px;}"
        << "h1{font-size:20px;margin:0 0 8px 0;font-weight:600;}"
        << ".meta{font-size:13px;color:#cbd2dc;display:flex;gap:18px;flex-wrap:wrap;}"
        << "main{padding:18px 24px 28px;}"
        << "section{margin:0 0 20px;background:#fff;border:1px solid #d8dee4;}"
        << "h2{font-size:15px;margin:0;padding:10px 12px;background:#eef1f5;border-bottom:1px solid #d8dee4;}"
        << "table{border-collapse:collapse;width:100%;table-layout:fixed;font-size:13px;}"
        << "th{text-align:left;background:#f6f8fa;border-bottom:1px solid #d8dee4;padding:7px;}"
        << "td{vertical-align:top;border-bottom:1px solid #edf0f3;padding:7px;word-break:break-word;}"
        << ".index{width:64px;text-align:right;color:#667085;background:#f6f8fa;}"
        << ".node-title{font-family:Consolas,Menlo,monospace;font-weight:600;margin-bottom:4px;}"
        << ".missing{color:#667085;text-align:center;background:#f6f8fa;padding:10px;}"
        << "tr.eq td{background:#fff;}"
        << "tr.chg td{background:#fff4ce;}"
        << "tr.ins td{background:#dafbe1;}"
        << "tr.del td{background:#ffebe9;}"
        << ".summary{display:grid;grid-template-columns:1fr 1fr;gap:12px;padding:12px;}"
        << ".summary div{background:#f6f8fa;border:1px solid #d8dee4;padding:8px;}"
        << "</style></head><body>\n";
    out << "<header><h1>Call Context</h1><div class=\"meta\">"
        << "<span>Primary: " << html_escape(primary->name) << " " << address_text(primary->address) << "</span>"
        << "<span>Secondary: " << html_escape(secondary->name) << " " << address_text(secondary->address) << "</span>"
        << "<span>Ratio: " << std::fixed << std::setprecision(6) << ratio << "</span>"
        << "<span>" << html_escape(description) << "</span>"
        << "</div></header>\n<main>\n";
    out << "<section><h2>Summary</h2><div class=\"summary\">"
        << "<div><b>Primary</b><br>callers=" << primary_callers.size()
        << "<br>callees=" << primary_callees.size() << "</div>"
        << "<div><b>Secondary</b><br>callers=" << secondary_callers.size()
        << "<br>callees=" << secondary_callees.size() << "</div>"
        << "</div></section>\n";
    append_context_table(out, "Callers", primary_callers, secondary_callers);
    append_context_table(out, "Callees", primary_callees, secondary_callees);
    out << "</main></body></html>\n";
    return out.str();
}

HtmlDiffDocument build_function_diff_document(
    const FunctionFeature& primary,
    const FunctionFeature& secondary,
    double ratio,
    std::string description)
{
    HtmlDiffDocument document;
    document.title = primary.name + " vs " + secondary.name;
    document.primary_name = primary.name;
    document.secondary_name = secondary.name;
    document.primary_address = primary.address;
    document.secondary_address = secondary.address;
    document.ratio = ratio;
    document.description = std::move(description);
    document.sections.push_back({"Assembly", primary.assembly, secondary.assembly});
    document.sections.push_back({"Clean Assembly", primary.stripped_assembly, secondary.stripped_assembly});
    document.sections.push_back({"Pseudocode", primary.pseudocode, secondary.pseudocode});
    document.sections.push_back({"Clean Pseudocode", primary.stripped_pseudocode, secondary.stripped_pseudocode});
    document.sections.push_back({"Microcode", primary.microcode, secondary.microcode});
    document.sections.push_back({"Clean Microcode", primary.stripped_microcode, secondary.stripped_microcode});
    return document;
}

} // namespace soff::ui
