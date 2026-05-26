#include "soff/diff/bb_matching.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace soff::diff {

namespace {

std::vector<BasicBlockInfo> load_basic_blocks(
    db::Database& database,
    Address function_address,
    const std::string& schema = "")
{
    const auto prefix = schema.empty() ? std::string("") : schema + ".";

    // Get function row id
    const auto func_rows = database.query_rows(
        "SELECT id FROM " + prefix + "functions WHERE address = '" +
        std::to_string(function_address) + "' LIMIT 1");
    if (func_rows.empty()) return {};
    const auto func_id = func_rows[0][0];

    // Get all basic blocks for this function
    const auto bb_rows = database.query_rows(
        "SELECT bb.id, bb.address FROM " + prefix + "function_bblocks fb "
        "JOIN " + prefix + "basic_blocks bb ON bb.id = fb.basic_block_id "
        "WHERE fb.function_id = " + func_id);

    std::vector<BasicBlockInfo> blocks;
    blocks.reserve(bb_rows.size());

    for (const auto& row : bb_rows) {
        BasicBlockInfo info;
        info.start = std::stoull(row[1]);

        const auto& bb_id = row[0];

        // Count instructions in this BB
        const auto instr_rows = database.query_rows(
            "SELECT i.mnemonic FROM " + prefix + "bb_instructions bi "
            "JOIN " + prefix + "instructions i ON i.id = bi.instruction_id "
            "WHERE bi.basic_block_id = " + bb_id);

        info.instruction_count = instr_rows.size();
        std::string mnemonics;
        for (const auto& ir : instr_rows) {
            if (!mnemonics.empty()) mnemonics += ',';
            mnemonics += ir[0];
        }
        info.mnemonics = std::move(mnemonics);

        // Get successors
        const auto succ_rows = database.query_rows(
            "SELECT child.address FROM " + prefix + "bb_relations r "
            "JOIN " + prefix + "basic_blocks child ON child.id = r.child_id "
            "WHERE r.parent_id = " + bb_id);
        for (const auto& sr : succ_rows) {
            info.successors.push_back(std::stoull(sr[0]));
        }
        info.out_degree = info.successors.size();
        blocks.push_back(std::move(info));
    }

    // Compute in-degree and mark entry/exit
    std::unordered_map<Address, std::size_t> in_deg;
    for (const auto& b : blocks) {
        for (auto s : b.successors) ++in_deg[s];
    }
    for (auto& b : blocks) {
        b.in_degree = in_deg[b.start];
        b.is_entry = (b.in_degree == 0);
        b.is_exit = (b.out_degree == 0);
    }
    return blocks;
}

} // anonymous namespace

// --- BB Matching Pipeline ---

BasicBlockMatchResult match_basic_blocks(
    db::Database& database,
    Address primary_function,
    Address secondary_function)
{
    BasicBlockMatchResult result;
    auto primary_bbs = load_basic_blocks(database, primary_function);
    auto secondary_bbs = load_basic_blocks(database, secondary_function, "diff");
    result.primary_blocks = primary_bbs.size();
    result.secondary_blocks = secondary_bbs.size();
    if (primary_bbs.empty() || secondary_bbs.empty()) return result;

    std::unordered_set<Address> matched_primary, matched_secondary;

    auto try_match = [&](Address p, Address s, double conf, const char* method) {
        if (matched_primary.count(p) || matched_secondary.count(s)) return;
        matched_primary.insert(p);
        matched_secondary.insert(s);
        result.matches.push_back({p, s, conf, method});
    };

    // Step 1: Match by identical mnemonic sequence (highest confidence)
    {
        std::unordered_map<std::string, std::vector<std::size_t>> sec_by_mnemonics;
        for (std::size_t i = 0; i < secondary_bbs.size(); ++i) {
            if (secondary_bbs[i].instruction_count >= 2) {
                sec_by_mnemonics[secondary_bbs[i].mnemonics].push_back(i);
            }
        }
        for (const auto& pb : primary_bbs) {
            if (pb.instruction_count < 2) continue;
            auto it = sec_by_mnemonics.find(pb.mnemonics);
            if (it != sec_by_mnemonics.end() && it->second.size() == 1) {
                auto& sb = secondary_bbs[it->second[0]];
                try_match(pb.start, sb.start, 1.0, "mnemonics");
            }
        }
    }

    // Step 2: Match entry points
    for (const auto& pb : primary_bbs) {
        if (!pb.is_entry) continue;
        for (const auto& sb : secondary_bbs) {
            if (!sb.is_entry) continue;
            try_match(pb.start, sb.start, 0.8, "entry");
            break;
        }
    }

    // Step 3: Match exit points
    for (const auto& pb : primary_bbs) {
        if (!pb.is_exit) continue;
        for (const auto& sb : secondary_bbs) {
            if (!sb.is_exit) continue;
            try_match(pb.start, sb.start, 0.6, "exit");
            break;
        }
    }

    // Step 4: Match by instruction count + degree (disambiguation)
    for (const auto& pb : primary_bbs) {
        if (matched_primary.count(pb.start)) continue;
        const BasicBlockInfo* best = nullptr;
        int best_score = -1;
        for (const auto& sb : secondary_bbs) {
            if (matched_secondary.count(sb.start)) continue;
            int score = 0;
            if (pb.instruction_count == sb.instruction_count) score += 3;
            if (pb.in_degree == sb.in_degree) score += 1;
            if (pb.out_degree == sb.out_degree) score += 1;
            if (score > best_score) { best_score = score; best = &sb; }
        }
        if (best && best_score >= 3) {
            try_match(pb.start, best->start, 0.4, "degree+count");
        }
    }

    return result;
}

double BasicBlockMatchResult::similarity() const
{
    if (primary_blocks == 0 && secondary_blocks == 0) return 1.0;
    if (primary_blocks == 0 || secondary_blocks == 0) return 0.0;
    const double avg = (static_cast<double>(primary_blocks) + secondary_blocks) / 2.0;
    return static_cast<double>(matches.size()) / avg;
}

double structural_similarity(const BasicBlockMatchResult& bb_result)
{
    return bb_result.similarity();
}

} // namespace soff::diff