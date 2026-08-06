#include "soff/diff/bb_matching.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace soff::diff {
namespace {

std::string asm_type_for(BasicBlockKind kind)
{
    return kind == BasicBlockKind::native ? "" : "microcode";
}

std::vector<BasicBlockInfo> load_basic_blocks(
    db::Database& database,
    Address function_address,
    const std::string& schema,
    BasicBlockKind kind)
{
    const auto prefix = schema.empty() ? std::string("") : schema + ".";
    const auto asm_type = asm_type_for(kind);

    auto function_statement = database.prepare(
        "select id from " + prefix + "functions where address = ? limit 1");
    function_statement.bind(1, std::to_string(function_address));
    if (!function_statement.step()) return {};
    const auto function_id = function_statement.column_text(0);

    auto block_statement = database.prepare(
        "select bb.id, coalesce(bb.address, '') from " + prefix + "function_bblocks fb "
        "join " + prefix + "basic_blocks bb on bb.id = fb.basic_block_id "
        "where fb.function_id = ? and coalesce(fb.asm_type, '') = ? "
        "and coalesce(bb.asm_type, '') = ? order by coalesce(bb.num, 0), bb.id");
    block_statement.bind(1, function_id);
    block_statement.bind(2, asm_type);
    block_statement.bind(3, asm_type);

    struct BlockRow
    {
        std::string id;
        BasicBlockInfo block;
    };
    std::vector<BlockRow> rows;
    std::unordered_map<Address, std::size_t> index_by_address;
    while (block_statement.step()) {
        const auto address_text = block_statement.column_text(1);
        if (address_text.empty()) continue;
        BlockRow row;
        row.id = block_statement.column_text(0);
        row.block.start = std::stoull(address_text);
        index_by_address[row.block.start] = rows.size();
        rows.push_back(std::move(row));
    }

    for (auto& row : rows) {
        auto instruction_statement = database.prepare(
            "select coalesce(i.mnemonic, '') from " + prefix + "bb_instructions bi "
            "join " + prefix + "instructions i on i.id = bi.instruction_id "
            "where bi.basic_block_id = ? and coalesce(i.asm_type, '') = ? order by bi.id");
        instruction_statement.bind(1, row.id);
        instruction_statement.bind(2, asm_type);
        while (instruction_statement.step()) {
            if (!row.block.mnemonics.empty()) row.block.mnemonics += ',';
            row.block.mnemonics += instruction_statement.column_text(0);
            ++row.block.instruction_count;
        }

        auto successor_statement = database.prepare(
            "select child.address from " + prefix + "bb_relations r "
            "join " + prefix + "basic_blocks child on child.id = r.child_id "
            "where r.parent_id = ? and coalesce(child.asm_type, '') = ? order by r.id");
        successor_statement.bind(1, row.id);
        successor_statement.bind(2, asm_type);
        while (successor_statement.step()) {
            const auto address_text = successor_statement.column_text(0);
            if (!address_text.empty()) row.block.successors.push_back(std::stoull(address_text));
        }
        row.block.out_degree = row.block.successors.size();
    }

    std::unordered_map<Address, std::size_t> in_degree;
    for (const auto& row : rows) {
        for (const auto successor : row.block.successors) {
            if (index_by_address.find(successor) != index_by_address.end()) ++in_degree[successor];
        }
    }

    std::vector<BasicBlockInfo> blocks;
    blocks.reserve(rows.size());
    for (auto& row : rows) {
        row.block.in_degree = in_degree[row.block.start];
        row.block.is_entry = row.block.in_degree == 0;
        row.block.is_exit = row.block.out_degree == 0;
        blocks.push_back(std::move(row.block));
    }
    return blocks;
}

std::size_t edge_count(const std::vector<BasicBlockInfo>& blocks)
{
    std::size_t count = 0;
    for (const auto& block : blocks) count += block.successors.size();
    return count;
}

std::size_t preserved_edge_count(
    const std::vector<BasicBlockInfo>& primary,
    const std::vector<BasicBlockInfo>& secondary,
    const std::vector<BasicBlockMatch>& matches)
{
    std::unordered_map<Address, Address> matched_address;
    for (const auto& match : matches) matched_address[match.primary_start] = match.secondary_start;

    std::unordered_map<Address, std::unordered_set<Address>> secondary_edges;
    for (const auto& block : secondary) {
        auto& edges = secondary_edges[block.start];
        edges.insert(block.successors.begin(), block.successors.end());
    }

    std::size_t preserved = 0;
    for (const auto& block : primary) {
        const auto mapped_parent = matched_address.find(block.start);
        if (mapped_parent == matched_address.end()) continue;
        const auto secondary_parent = secondary_edges.find(mapped_parent->second);
        if (secondary_parent == secondary_edges.end()) continue;
        for (const auto successor : block.successors) {
            const auto mapped_child = matched_address.find(successor);
            if (mapped_child != matched_address.end()
                && secondary_parent->second.count(mapped_child->second) != 0) {
                ++preserved;
            }
        }
    }
    return preserved;
}

} // namespace

BasicBlockMatchResult match_basic_blocks(
    db::Database& database,
    Address primary_function,
    Address secondary_function,
    BasicBlockKind kind)
{
    BasicBlockMatchResult result;
    auto primary_bbs = load_basic_blocks(database, primary_function, "", kind);
    auto secondary_bbs = load_basic_blocks(database, secondary_function, "diff", kind);
    result.primary_blocks = primary_bbs.size();
    result.secondary_blocks = secondary_bbs.size();
    result.primary_edges = edge_count(primary_bbs);
    result.secondary_edges = edge_count(secondary_bbs);
    if (primary_bbs.empty() || secondary_bbs.empty()) return result;

    std::unordered_set<Address> matched_primary;
    std::unordered_set<Address> matched_secondary;
    auto try_match = [&](Address primary, Address secondary, double confidence, const char* method) {
        if (matched_primary.count(primary) || matched_secondary.count(secondary)) return;
        matched_primary.insert(primary);
        matched_secondary.insert(secondary);
        result.matches.push_back({primary, secondary, confidence, method});
    };

    {
        std::unordered_map<std::string, std::vector<std::size_t>> secondary_by_mnemonics;
        for (std::size_t i = 0; i < secondary_bbs.size(); ++i) {
            if (secondary_bbs[i].instruction_count >= 2) {
                secondary_by_mnemonics[secondary_bbs[i].mnemonics].push_back(i);
            }
        }
        for (const auto& primary : primary_bbs) {
            if (primary.instruction_count < 2) continue;
            const auto found = secondary_by_mnemonics.find(primary.mnemonics);
            if (found != secondary_by_mnemonics.end() && found->second.size() == 1) {
                const auto& secondary = secondary_bbs[found->second.front()];
                try_match(primary.start, secondary.start, 1.0, "mnemonics");
            }
        }
    }

    for (const auto& primary : primary_bbs) {
        if (!primary.is_entry) continue;
        for (const auto& secondary : secondary_bbs) {
            if (!secondary.is_entry) continue;
            try_match(primary.start, secondary.start, 0.8, "entry");
            break;
        }
    }

    for (const auto& primary : primary_bbs) {
        if (!primary.is_exit) continue;
        for (const auto& secondary : secondary_bbs) {
            if (!secondary.is_exit) continue;
            try_match(primary.start, secondary.start, 0.6, "exit");
            break;
        }
    }

    for (const auto& primary : primary_bbs) {
        if (matched_primary.count(primary.start)) continue;
        const BasicBlockInfo* best = nullptr;
        int best_score = -1;
        for (const auto& secondary : secondary_bbs) {
            if (matched_secondary.count(secondary.start)) continue;
            int score = 0;
            if (primary.instruction_count == secondary.instruction_count) score += 3;
            if (primary.in_degree == secondary.in_degree) score += 1;
            if (primary.out_degree == secondary.out_degree) score += 1;
            if (score > best_score) {
                best_score = score;
                best = &secondary;
            }
        }
        if (best != nullptr && best_score >= 3) {
            try_match(primary.start, best->start, 0.4, "degree+count");
        }
    }

    result.preserved_edges = preserved_edge_count(primary_bbs, secondary_bbs, result.matches);
    return result;
}

double BasicBlockMatchResult::similarity() const
{
    if (primary_blocks == 0 && secondary_blocks == 0) return 1.0;
    if (primary_blocks == 0 || secondary_blocks == 0 || matches.empty()) return 0.0;

    double confidence_sum = 0.0;
    for (const auto& match : matches) confidence_sum += std::clamp(match.confidence, 0.0, 1.0);
    const double average_blocks = (static_cast<double>(primary_blocks) + secondary_blocks) / 2.0;
    const double weighted_coverage = std::clamp(confidence_sum / average_blocks, 0.0, 1.0);

    double edge_similarity = 0.0;
    if (primary_edges == 0 && secondary_edges == 0) {
        edge_similarity = 1.0;
    } else if (primary_edges != 0 && secondary_edges != 0) {
        edge_similarity = static_cast<double>(preserved_edges)
            / static_cast<double>(std::max(primary_edges, secondary_edges));
    }
    return std::clamp(0.8 * weighted_coverage + 0.2 * edge_similarity, 0.0, 1.0);
}

double structural_similarity(const BasicBlockMatchResult& bb_result)
{
    return bb_result.similarity();
}

} // namespace soff::diff
