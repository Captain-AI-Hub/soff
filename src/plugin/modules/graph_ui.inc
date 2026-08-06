bool write_text_file(const std::filesystem::path& path, const std::string& content)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    return file.good();
}

bool open_file_with_shell(const std::filesystem::path& path)
{
    const auto path_text = path.string();
#if defined(_WIN32)
    HINSTANCE result = ShellExecuteA(
        nullptr, "open", path_text.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<std::intptr_t>(result) > 32;
#elif defined(__APPLE__)
    return std::system(("open \"" + path_text + "\"").c_str()) == 0;
#else
    return std::system(("xdg-open \"" + path_text + "\"").c_str()) == 0;
#endif
}

bool save_or_open_match_html_diff(
    const std::filesystem::path& result_path,
    const soff::db::DiffResultSet& results,
    const soff::db::ResultMatch& match,
    bool open_after_save,
    PathCache<soff::ProgramSnapshot>* primary_snapshot_cache,
    PathCache<soff::ProgramSnapshot>* secondary_snapshot_cache)
{
    try {
        if (const auto error = validate_export_database(results.main_db, "Primary"); !error.empty()) {
            throw soff::Error(soff::ErrorCode::diff_failed, error);
        }
        if (const auto error = validate_export_database(results.diff_db, "Secondary"); !error.empty()) {
            throw soff::Error(soff::ErrorCode::diff_failed, error);
        }

        PathCache<soff::ProgramSnapshot> primary_uncached;
        PathCache<soff::ProgramSnapshot> secondary_uncached;
        const auto& primary_snapshot = load_snapshot_cached(
            primary_snapshot_cache != nullptr ? *primary_snapshot_cache : primary_uncached,
            results.main_db);
        const auto& secondary_snapshot = load_snapshot_cached(
            secondary_snapshot_cache != nullptr ? *secondary_snapshot_cache : secondary_uncached,
            results.diff_db);
        const auto* primary = find_function_by_address(primary_snapshot, match.primary);
        const auto* secondary = find_function_by_address(secondary_snapshot, match.secondary);
        if (primary == nullptr || secondary == nullptr) {
            throw soff::Error(soff::ErrorCode::function_not_found, "matched functions are missing from export snapshots");
        }

        auto document = soff::ui::build_function_diff_document(
            *primary,
            *secondary,
            match.ratio,
            match.description);
        document.primary_name = side_function_label(results.main_db, match.primary_name);
        document.secondary_name = side_function_label(results.diff_db, match.secondary_name);
        document.title = "Soff text diff: " + db_file_label(results.main_db) + " vs " + db_file_label(results.diff_db);
        const auto html = soff::ui::render_html_diff(document);

        std::filesystem::path output_path;
        if (open_after_save) {
            output_path = default_html_diff_path(result_path, match, true);
        } else {
            const auto default_path = default_html_diff_path(result_path, match, false).string();
            char* selected = ask_file(true, default_path.c_str(), "Save SOFF HTML diff");
            if (selected == nullptr || selected[0] == '\0') {
                return false;
            }
            output_path = selected;
        }

        if (!write_text_file(output_path, html)) {
            throw soff::Error(soff::ErrorCode::diff_failed, "failed to write HTML diff: " + output_path.string());
        }
        if (open_after_save) {
            if (!open_file_with_shell(output_path)) {
                throw soff::Error(soff::ErrorCode::diff_failed, "failed to open HTML diff: " + output_path.string());
            }
            msg("Soff: opened HTML diff %s\n", output_path.string().c_str());
        } else {
            info("Soff saved HTML diff:\n%s", output_path.string().c_str());
        }
        return true;
    } catch (const std::exception& error) {
        warning("Soff failed to create HTML diff:\n%s", error.what());
    } catch (...) {
        warning("Soff failed to create HTML diff with an unknown error");
    }
    return false;
}

enum class GraphDiffNodeSide
{
    primary,
    secondary,
};

enum class GraphDiffEdgeKind
{
    primary_cfg,
    secondary_cfg,
    paired_same,
    paired_changed,
    common_cfg,
    primary_only_cfg,
    secondary_only_cfg,
};

struct GraphDiffNode
{
    std::string text;
    bgcolor_t color = DEFCOLOR;
    GraphDiffNodeSide side = GraphDiffNodeSide::primary;
    soff::Address primary = 0;
    soff::Address secondary = 0;
    soff::Address primary_block_end = 0;
    soff::Address secondary_block_end = 0;
    std::vector<soff::Address> primary_instructions;
    std::vector<soff::Address> secondary_instructions;
};

struct GraphDiffEdge
{
    int source = 0;
    int target = 0;
    GraphDiffEdgeKind kind = GraphDiffEdgeKind::primary_cfg;
};

struct GraphDiffContext
{
    graph_id_t graph_id = 0;
    graph_viewer_t* viewer = nullptr;
    GraphDiffContext* peer = nullptr;
    std::string title;
    std::string main_db;
    std::string diff_db;
    soff::db::ResultMatch match;
    bool has_match = false;
    std::vector<GraphDiffNode> nodes;
    std::vector<GraphDiffEdge> edges;
    std::vector<int> peer_node_by_node;
    PathCache<soff::ProgramSnapshot> primary_snapshot_cache;
    PathCache<soff::ProgramSnapshot> secondary_snapshot_cache;
};

std::vector<std::unique_ptr<GraphDiffContext>>& graph_diff_contexts()
{
    static std::vector<std::unique_ptr<GraphDiffContext>> contexts;
    return contexts;
}

GraphDiffContext*& active_graph_diff_context()
{
    static GraphDiffContext* context = nullptr;
    return context;
}

int& active_graph_diff_node()
{
    static int node = -1;
    return node;
}

std::string address_hex(soff::Address address)
{
    if (address == 0) {
        return "-";
    }
    std::ostringstream out;
    out << "0x" << std::hex << address;
    return out.str();
}

std::unordered_map<soff::Address, std::string> instruction_text_by_address(
    const std::vector<soff::InstructionFeature>& instructions)
{
    std::unordered_map<soff::Address, std::string> result;
    for (const auto& instruction : instructions) {
        const auto text = !instruction.disassembly.empty()
            ? instruction.disassembly
            : instruction.mnemonic;
        result.emplace(instruction.address, text);
    }
    return result;
}

std::string block_summary_text(
    const soff::BasicBlock* block,
    const std::unordered_map<soff::Address, std::string>& instruction_texts)
{
    if (block == nullptr) {
        return "(missing)";
    }

    std::ostringstream out;
    bool first = true;
    for (const auto address : block->instructions) {
        if (!first) out << '\n';
        first = false;
        const auto found = instruction_texts.find(address);
        if (found != instruction_texts.end() && !found->second.empty()) {
            out << found->second;
        } else {
            out << address_hex(address);
        }
    }
    const auto text = out.str();
    return text.empty() ? "(no instructions)" : text;
}

std::vector<std::string> get_block_lines(
    const soff::BasicBlock* block,
    const std::unordered_map<soff::Address, std::string>& texts)
{
    std::vector<std::string> lines;
    if (block == nullptr) return lines;
    for (const auto address : block->instructions) {
        const auto found = texts.find(address);
        if (found != texts.end() && !found->second.empty()) {
            lines.push_back(found->second);
        } else {
            lines.push_back(address_hex(address));
        }
    }
    return lines;
}

std::string block_diff_text(
    const soff::BasicBlock* primary_block,
    const soff::BasicBlock* secondary_block,
    const std::unordered_map<soff::Address, std::string>& primary_texts,
    const std::unordered_map<soff::Address, std::string>& secondary_texts,
    const std::string& primary_file_label,
    const std::string& secondary_file_label,
    bool is_primary_side)
{
    if (primary_block == nullptr && secondary_block == nullptr) {
        return "(missing)";
    }

    // Color helpers for IDA graph node text
    auto color_line = [](const std::string& text) -> std::string {
        std::string result;
        result += '\x01';  // COLOR_ON
        result += '\x12';  // COLOR_ERROR
        result += text;
        result += '\x02';  // COLOR_OFF
        result += '\x12';  // COLOR_ERROR
        return result;
    };

    if (primary_block == nullptr) {
        const auto lines = get_block_lines(secondary_block, secondary_texts);
        std::ostringstream out;
        for (const auto& line : lines) {
            if (is_primary_side) {
                out << "\n";
            } else {
                out << color_line(line) << "\n";
            }
        }
        auto r = out.str();
        if (!r.empty() && r.back() == '\n') r.pop_back();
        return r;
    }
    if (secondary_block == nullptr) {
        const auto lines = get_block_lines(primary_block, primary_texts);
        std::ostringstream out;
        for (const auto& line : lines) {
            if (is_primary_side) {
                out << color_line(line) << "\n";
            } else {
                out << "\n";
            }
        }
        auto r = out.str();
        if (!r.empty() && r.back() == '\n') r.pop_back();
        return r;
    }

    const auto p_lines = get_block_lines(primary_block, primary_texts);
    const auto s_lines = get_block_lines(secondary_block, secondary_texts);

    const auto diff = soff::ui::compute_line_diff(p_lines, s_lines);

    // Aligned side-by-side: show own lines, blank for other side's lines
    std::ostringstream out;
    for (const auto& entry : diff) {
        if (entry.kind == soff::ui::DiffEntry::same) {
            out << p_lines[entry.left_index] << "\n";
        } else if (entry.kind == soff::ui::DiffEntry::removed) {
            if (is_primary_side) {
                out << color_line(p_lines[entry.left_index]) << "\n";
            } else {
                out << "\n";
            }
        } else {
            if (!is_primary_side) {
                out << color_line(s_lines[entry.right_index]) << "\n";
            } else {
                out << "\n";
            }
        }
    }
    auto result = out.str();
    if (!result.empty() && result.back() == '\n') {
        result.pop_back();
    }
    return result.empty() ? "(identical)" : result;
}

std::string successor_summary(const soff::BasicBlock* block)
{
    if (block == nullptr || block->successors.empty()) {
        return "-";
    }
    std::ostringstream out;
    for (std::size_t i = 0; i < block->successors.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << address_hex(block->successors[i]);
    }
    return out.str();
}

bool same_block_content(
    const soff::BasicBlock& primary,
    const soff::BasicBlock& secondary,
    const std::unordered_map<soff::Address, std::string>& primary_texts,
    const std::unordered_map<soff::Address, std::string>& secondary_texts)
{
    if (primary.instructions.size() != secondary.instructions.size()
        || primary.successors.size() != secondary.successors.size()) {
        return false;
    }
    for (std::size_t i = 0; i < primary.instructions.size(); ++i) {
        const auto primary_it = primary_texts.find(primary.instructions[i]);
        const auto secondary_it = secondary_texts.find(secondary.instructions[i]);
        const auto primary_text = primary_it == primary_texts.end() ? "" : primary_it->second;
        const auto secondary_text = secondary_it == secondary_texts.end() ? "" : secondary_it->second;
        if (primary_text != secondary_text) {
            return false;
        }
    }
    return true;
}

std::string block_signature(
    const soff::BasicBlock& block,
    const std::unordered_map<soff::Address, std::string>& instruction_texts)
{
    std::ostringstream out;
    for (const auto address : block.instructions) {
        const auto found = instruction_texts.find(address);
        if (found != instruction_texts.end() && !found->second.empty()) {
            out << trim_copy(found->second);
        } else {
            out << address_hex(address);
        }
        out << '\n';
    }
    return out.str();
}

struct GraphBlockPair
{
    std::size_t primary_index = static_cast<std::size_t>(-1);
    std::size_t secondary_index = static_cast<std::size_t>(-1);
    std::string reason;
    double score = 0.0;
};

bool has_primary_block(const GraphBlockPair& pair)
{
    return pair.primary_index != static_cast<std::size_t>(-1);
}

bool has_secondary_block(const GraphBlockPair& pair)
{
    return pair.secondary_index != static_cast<std::size_t>(-1);
}

std::vector<std::string> block_text_lines(
    const soff::BasicBlock& block,
    const std::unordered_map<soff::Address, std::string>& instruction_texts)
{
    std::vector<std::string> lines;
    lines.reserve(block.instructions.size());
    for (const auto address : block.instructions) {
        const auto found = instruction_texts.find(address);
        auto text = found != instruction_texts.end() && !found->second.empty()
            ? trim_copy(found->second)
            : address_hex(address);
        if (!text.empty()) {
            lines.push_back(std::move(text));
        }
    }
    return lines;
}

double sequence_similarity(const std::vector<std::string>& left, const std::vector<std::string>& right)
{
    if (left.empty() && right.empty()) {
        return 1.0;
    }
    if (left.empty() || right.empty()) {
        return 0.0;
    }
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
    return (2.0 * static_cast<double>(at(0, 0))) / static_cast<double>(rows + cols);
}

std::vector<std::size_t> predecessor_counts(const std::vector<soff::BasicBlock>& blocks)
{
    std::unordered_map<soff::Address, std::size_t> index_by_start;
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        index_by_start.emplace(blocks[i].start, i);
    }

    std::vector<std::size_t> counts(blocks.size(), 0);
    for (const auto& block : blocks) {
        for (const auto successor : block.successors) {
            const auto found = index_by_start.find(successor);
            if (found != index_by_start.end()) {
                ++counts[found->second];
            }
        }
    }
    return counts;
}

double closeness_score(std::size_t left, std::size_t right)
{
    const auto max_value = std::max(left, right);
    if (max_value == 0) {
        return 1.0;
    }
    const auto delta = left > right ? left - right : right - left;
    return 1.0 - (static_cast<double>(delta) / static_cast<double>(max_value + 1));
}

double block_pair_score(
    const soff::BasicBlock& primary,
    const soff::BasicBlock& secondary,
    std::size_t primary_index,
    std::size_t secondary_index,
    std::size_t primary_pred,
    std::size_t secondary_pred,
    const std::unordered_map<soff::Address, std::string>& primary_texts,
    const std::unordered_map<soff::Address, std::string>& secondary_texts)
{
    const auto text_score = sequence_similarity(
        block_text_lines(primary, primary_texts),
        block_text_lines(secondary, secondary_texts));
    const auto instruction_score = closeness_score(primary.instructions.size(), secondary.instructions.size());
    const auto successor_score = closeness_score(primary.successors.size(), secondary.successors.size());
    const auto predecessor_score = closeness_score(primary_pred, secondary_pred);
    const auto ordinal_score = closeness_score(primary_index, secondary_index);
    return (text_score * 0.60)
        + (instruction_score * 0.15)
        + (successor_score * 0.10)
        + (predecessor_score * 0.10)
        + (ordinal_score * 0.05);
}

std::vector<GraphBlockPair> pair_graph_blocks(
    const std::vector<soff::BasicBlock>& primary_blocks,
    const std::vector<soff::BasicBlock>& secondary_blocks,
    const std::unordered_map<soff::Address, std::string>& primary_texts,
    const std::unordered_map<soff::Address, std::string>& secondary_texts)
{
    std::vector<GraphBlockPair> pairs;
    std::vector<bool> used_secondary(secondary_blocks.size(), false);

    std::unordered_map<std::string, std::vector<std::size_t>> secondary_by_signature;
    for (std::size_t i = 0; i < secondary_blocks.size(); ++i) {
        const auto signature = block_signature(secondary_blocks[i], secondary_texts);
        if (!signature.empty()) {
            secondary_by_signature[signature].push_back(i);
        }
    }

    for (std::size_t primary_index = 0; primary_index < primary_blocks.size(); ++primary_index) {
        const auto signature = block_signature(primary_blocks[primary_index], primary_texts);
        auto found = secondary_by_signature.find(signature);
        if (found == secondary_by_signature.end()) {
            continue;
        }
        auto secondary = std::find_if(
            found->second.begin(),
            found->second.end(),
            [&used_secondary](std::size_t index) {
                return index < used_secondary.size() && !used_secondary[index];
            });
        if (secondary == found->second.end()) {
            continue;
        }
        used_secondary[*secondary] = true;
        pairs.push_back({primary_index, *secondary, "exact", 1.0});
    }

    std::vector<bool> used_primary(primary_blocks.size(), false);
    for (const auto& pair : pairs) {
        if (has_primary_block(pair)) {
            used_primary[pair.primary_index] = true;
        }
    }

    const auto primary_preds = predecessor_counts(primary_blocks);
    const auto secondary_preds = predecessor_counts(secondary_blocks);
    struct Candidate
    {
        std::size_t primary_index = 0;
        std::size_t secondary_index = 0;
        double score = 0.0;
    };
    std::vector<Candidate> candidates;
    for (std::size_t primary_index = 0; primary_index < primary_blocks.size(); ++primary_index) {
        if (used_primary[primary_index]) {
            continue;
        }
        for (std::size_t secondary_index = 0; secondary_index < secondary_blocks.size(); ++secondary_index) {
            if (used_secondary[secondary_index]) {
                continue;
            }
            const auto score = block_pair_score(
                primary_blocks[primary_index],
                secondary_blocks[secondary_index],
                primary_index,
                secondary_index,
                primary_index < primary_preds.size() ? primary_preds[primary_index] : 0,
                secondary_index < secondary_preds.size() ? secondary_preds[secondary_index] : 0,
                primary_texts,
                secondary_texts);
            if (score >= 0.5) {
                candidates.push_back({primary_index, secondary_index, score});
            }
        }
    }
    std::sort(
        candidates.begin(),
        candidates.end(),
        [](const Candidate& lhs, const Candidate& rhs) {
            if (lhs.score != rhs.score) {
                return lhs.score > rhs.score;
            }
            const auto lhs_delta = lhs.primary_index > lhs.secondary_index
                ? lhs.primary_index - lhs.secondary_index
                : lhs.secondary_index - lhs.primary_index;
            const auto rhs_delta = rhs.primary_index > rhs.secondary_index
                ? rhs.primary_index - rhs.secondary_index
                : rhs.secondary_index - rhs.primary_index;
            return lhs_delta < rhs_delta;
        });
    for (const auto& candidate : candidates) {
        if (used_primary[candidate.primary_index] || used_secondary[candidate.secondary_index]) {
            continue;
        }
        used_primary[candidate.primary_index] = true;
        used_secondary[candidate.secondary_index] = true;
        pairs.push_back({candidate.primary_index, candidate.secondary_index, "similar", candidate.score});
    }

    const auto count = std::max(primary_blocks.size(), secondary_blocks.size());
    for (std::size_t i = 0; i < count; ++i) {
        if (i < primary_blocks.size() && used_primary[i]) {
            continue;
        }
        if (i < primary_blocks.size() && i < secondary_blocks.size() && !used_secondary[i]) {
            const auto score = block_pair_score(
                primary_blocks[i],
                secondary_blocks[i],
                i,
                i,
                i < primary_preds.size() ? primary_preds[i] : 0,
                i < secondary_preds.size() ? secondary_preds[i] : 0,
                primary_texts,
                secondary_texts);
            if (score < 0.35) {
                used_primary[i] = true;
                pairs.push_back({i, static_cast<std::size_t>(-1), "primary-only", 0.0});
                continue;
            }
            used_primary[i] = true;
            used_secondary[i] = true;
            pairs.push_back({i, i, "ordinal", score});
            continue;
        }
        if (i < primary_blocks.size()) {
            used_primary[i] = true;
            pairs.push_back({i, static_cast<std::size_t>(-1), "primary-only", 0.0});
        }
    }

    for (std::size_t i = 0; i < secondary_blocks.size(); ++i) {
        if (!used_secondary[i]) {
            pairs.push_back({static_cast<std::size_t>(-1), i, "secondary-only", 0.0});
        }
    }

    std::stable_sort(
        pairs.begin(),
        pairs.end(),
        [](const GraphBlockPair& lhs, const GraphBlockPair& rhs) {
            const auto lhs_key = has_primary_block(lhs) ? lhs.primary_index : lhs.secondary_index;
            const auto rhs_key = has_primary_block(rhs) ? rhs.primary_index : rhs.secondary_index;
            return lhs_key < rhs_key;
        });
    return pairs;
}

bgcolor_t graph_node_color(bool same, bool primary_only, bool secondary_only)
{
    if (primary_only || secondary_only) {
        return static_cast<bgcolor_t>(0xCCCCFF);
    }
    return same ? static_cast<bgcolor_t>(0xFFFFFF) : static_cast<bgcolor_t>(0xCCFFFF);
}

bgcolor_t graph_edge_color(GraphDiffEdgeKind kind)
{
    switch (kind) {
    case GraphDiffEdgeKind::primary_cfg:
        return static_cast<bgcolor_t>(0xD55E5E);
    case GraphDiffEdgeKind::secondary_cfg:
        return static_cast<bgcolor_t>(0x4F7FD9);
    case GraphDiffEdgeKind::paired_same:
        return static_cast<bgcolor_t>(0x4A9A47);
    case GraphDiffEdgeKind::paired_changed:
        return static_cast<bgcolor_t>(0xC69A22);
    case GraphDiffEdgeKind::common_cfg:
        return static_cast<bgcolor_t>(0x4A9A47);
    case GraphDiffEdgeKind::primary_only_cfg:
        return static_cast<bgcolor_t>(0xD55E5E);
    case GraphDiffEdgeKind::secondary_only_cfg:
        return static_cast<bgcolor_t>(0x4F7FD9);
    }
    return DEFCOLOR;
}

GraphDiffContext build_graph_diff_context(
    const soff::FunctionFeature& primary,
    const soff::FunctionFeature& secondary,
    soff::ui::GraphDiffKind kind,
    double ratio,
    const std::string& description,
    const std::string& primary_file_label,
    const std::string& secondary_file_label)
{
    const auto& primary_blocks = kind == soff::ui::GraphDiffKind::native
        ? primary.blocks
        : primary.microcode_blocks;
    const auto& secondary_blocks = kind == soff::ui::GraphDiffKind::native
        ? secondary.blocks
        : secondary.microcode_blocks;
    const auto primary_texts = instruction_text_by_address(
        kind == soff::ui::GraphDiffKind::native
            ? primary.instruction_details
            : primary.microcode_instruction_details);
    const auto secondary_texts = instruction_text_by_address(
        kind == soff::ui::GraphDiffKind::native
            ? secondary.instruction_details
            : secondary.microcode_instruction_details);

    GraphDiffContext context;
    std::ostringstream title;
    title << "Soff"
        << (kind == soff::ui::GraphDiffKind::native ? "Function Graph Diff: " : "Microcode Graph Diff: ")
        << primary.name << " vs " << secondary.name;
    context.title = title.str();

    const auto pairs = pair_graph_blocks(primary_blocks, secondary_blocks, primary_texts, secondary_texts);
    context.nodes.reserve(primary_blocks.size() + secondary_blocks.size());
    std::unordered_map<soff::Address, int> primary_index;
    std::unordered_map<soff::Address, int> secondary_index;

    for (std::size_t pair_index = 0; pair_index < pairs.size(); ++pair_index) {
        const auto& pair = pairs[pair_index];
        const auto* primary_block = has_primary_block(pair) ? &primary_blocks[pair.primary_index] : nullptr;
        const auto* secondary_block = has_secondary_block(pair) ? &secondary_blocks[pair.secondary_index] : nullptr;
        const bool same = primary_block != nullptr
            && secondary_block != nullptr
            && same_block_content(*primary_block, *secondary_block, primary_texts, secondary_texts);
        const auto color = graph_node_color(same, secondary_block == nullptr, primary_block == nullptr);

        int primary_node = -1;
        int secondary_node = -1;
        if (primary_block != nullptr) {
            primary_node = static_cast<int>(context.nodes.size());
            primary_index.emplace(primary_block->start, primary_node);
            std::ostringstream text;
            text << address_hex(primary_block->start) << ":\n"
                << block_diff_text(primary_block, secondary_block, primary_texts, secondary_texts, primary_file_label, secondary_file_label, true);
            context.nodes.push_back({
                text.str(),
                color,
                GraphDiffNodeSide::primary,
                primary_block->start,
                secondary_block == nullptr ? 0 : secondary_block->start,
                primary_block->end,
                secondary_block == nullptr ? 0 : secondary_block->end,
                primary_block->instructions,
                secondary_block == nullptr ? std::vector<soff::Address>{} : secondary_block->instructions,
            });
        }

        if (secondary_block != nullptr) {
            secondary_node = static_cast<int>(context.nodes.size());
            secondary_index.emplace(secondary_block->start, secondary_node);
            std::ostringstream text;
            text << address_hex(secondary_block->start) << ":\n"
                << block_diff_text(primary_block, secondary_block, primary_texts, secondary_texts, primary_file_label, secondary_file_label, false);
            context.nodes.push_back({
                text.str(),
                color,
                GraphDiffNodeSide::secondary,
                primary_block == nullptr ? 0 : primary_block->start,
                secondary_block->start,
                primary_block == nullptr ? 0 : primary_block->end,
                secondary_block->end,
                primary_block == nullptr ? std::vector<soff::Address>{} : primary_block->instructions,
                secondary_block->instructions,
            });
        }

        if (primary_node >= 0 && secondary_node >= 0) {
            context.edges.push_back({
                primary_node,
                secondary_node,
                same ? GraphDiffEdgeKind::paired_same : GraphDiffEdgeKind::paired_changed,
            });
        }
    }

    std::set<std::pair<int, int>> primary_edges;
    for (std::size_t i = 0; i < primary_blocks.size(); ++i) {
        const auto parent = primary_index.find(primary_blocks[i].start);
        if (parent == primary_index.end()) {
            continue;
        }
        for (const auto successor : primary_blocks[i].successors) {
            const auto found = primary_index.find(successor);
            if (found != primary_index.end() && primary_edges.emplace(parent->second, found->second).second) {
                context.edges.push_back({parent->second, found->second, GraphDiffEdgeKind::primary_cfg});
            }
        }
    }
    std::set<std::pair<int, int>> secondary_edges;
    for (std::size_t i = 0; i < secondary_blocks.size(); ++i) {
        const auto parent = secondary_index.find(secondary_blocks[i].start);
        if (parent == secondary_index.end()) {
            continue;
        }
        for (const auto successor : secondary_blocks[i].successors) {
            const auto found = secondary_index.find(successor);
            if (found != secondary_index.end() && secondary_edges.emplace(parent->second, found->second).second) {
                context.edges.push_back({parent->second, found->second, GraphDiffEdgeKind::secondary_cfg});
            }
        }
    }

    return context;
}

GraphDiffContext build_single_graph_context(
    const soff::FunctionFeature& primary,
    const soff::FunctionFeature& secondary,
    soff::ui::GraphDiffKind kind,
    bool primary_side,
    const std::string& primary_label,
    const std::string& secondary_label,
    const std::string& primary_file_label,
    const std::string& secondary_file_label,
    double ratio,
    const std::string& description)
{
    const bool use_native_blocks = kind != soff::ui::GraphDiffKind::microcode;
    const auto& primary_blocks = use_native_blocks
        ? primary.blocks
        : primary.microcode_blocks;
    const auto& secondary_blocks = use_native_blocks
        ? secondary.blocks
        : secondary.microcode_blocks;

    const auto primary_texts = instruction_text_by_address(
        kind == soff::ui::GraphDiffKind::native
            ? primary.instruction_details
            : primary.microcode_instruction_details);
    const auto secondary_texts = instruction_text_by_address(
        kind == soff::ui::GraphDiffKind::native
            ? secondary.instruction_details
            : secondary.microcode_instruction_details);

    GraphDiffContext context;
    std::ostringstream title;
    const char* kind_label = kind == soff::ui::GraphDiffKind::native
        ? "Assembly Graph: " : "Microcode Graph: ";
    title << kind_label
        << (primary_side ? primary_label : secondary_label);
    context.title = title.str();

    const auto pairs = pair_graph_blocks(primary_blocks, secondary_blocks, primary_texts, secondary_texts);
    const auto& blocks = primary_side ? primary_blocks : secondary_blocks;
    const auto& texts = primary_side ? primary_texts : secondary_texts;
    const auto& shown_label = primary_side ? primary_label : secondary_label;

    std::unordered_map<soff::Address, soff::Address> primary_to_secondary;
    std::unordered_map<soff::Address, soff::Address> secondary_to_primary;
    for (const auto& pair : pairs) {
        if (!has_primary_block(pair) || !has_secondary_block(pair)) {
            continue;
        }
        primary_to_secondary.emplace(primary_blocks[pair.primary_index].start, secondary_blocks[pair.secondary_index].start);
        secondary_to_primary.emplace(secondary_blocks[pair.secondary_index].start, primary_blocks[pair.primary_index].start);
    }
    std::set<std::pair<soff::Address, soff::Address>> primary_edges_by_address;
    for (const auto& block : primary_blocks) {
        for (const auto successor : block.successors) {
            primary_edges_by_address.emplace(block.start, successor);
        }
    }
    std::set<std::pair<soff::Address, soff::Address>> secondary_edges_by_address;
    for (const auto& block : secondary_blocks) {
        for (const auto successor : block.successors) {
            secondary_edges_by_address.emplace(block.start, successor);
        }
    }

    std::unordered_map<soff::Address, int> block_index;
    for (const auto& pair : pairs) {
        const auto* primary_block = has_primary_block(pair) ? &primary_blocks[pair.primary_index] : nullptr;
        const auto* secondary_block = has_secondary_block(pair) ? &secondary_blocks[pair.secondary_index] : nullptr;
        const auto* shown_block = primary_side ? primary_block : secondary_block;
        if (shown_block == nullptr) {
            continue;
        }

        const bool same = primary_block != nullptr
            && secondary_block != nullptr
            && same_block_content(*primary_block, *secondary_block, primary_texts, secondary_texts);
        const bool primary_only = primary_side && secondary_block == nullptr;
        const bool secondary_only = !primary_side && primary_block == nullptr;
        const auto color = graph_node_color(same, primary_only, secondary_only);

        const auto node_index = static_cast<int>(context.nodes.size());
        block_index.emplace(shown_block->start, node_index);
        std::ostringstream text;
        text << address_hex(shown_block->start) << ":\n"
            << block_diff_text(primary_block, secondary_block, primary_texts, secondary_texts, primary_file_label, secondary_file_label, primary_side);
        context.nodes.push_back({
            text.str(),
            color,
            primary_side ? GraphDiffNodeSide::primary : GraphDiffNodeSide::secondary,
            primary_side ? shown_block->start : 0,
            primary_side ? (secondary_block == nullptr ? 0 : secondary_block->start) : shown_block->start,
            primary_block == nullptr ? 0 : primary_block->end,
            secondary_block == nullptr ? 0 : secondary_block->end,
            primary_block == nullptr ? std::vector<soff::Address>{} : primary_block->instructions,
            secondary_block == nullptr ? std::vector<soff::Address>{} : secondary_block->instructions,
        });
    }

    std::set<std::pair<int, int>> edges;
    for (const auto& block : blocks) {
        const auto parent = block_index.find(block.start);
        if (parent == block_index.end()) {
            continue;
        }
        for (const auto successor : block.successors) {
            const auto found = block_index.find(successor);
            if (found != block_index.end() && edges.emplace(parent->second, found->second).second) {
                auto edge_kind = primary_side
                    ? GraphDiffEdgeKind::primary_only_cfg
                    : GraphDiffEdgeKind::secondary_only_cfg;
                if (primary_side) {
                    const auto paired_source = primary_to_secondary.find(block.start);
                    const auto paired_target = primary_to_secondary.find(successor);
                    if (paired_source != primary_to_secondary.end()
                        && paired_target != primary_to_secondary.end()
                        && secondary_edges_by_address.find({paired_source->second, paired_target->second}) != secondary_edges_by_address.end()) {
                        edge_kind = GraphDiffEdgeKind::common_cfg;
                    }
                } else {
                    const auto paired_source = secondary_to_primary.find(block.start);
                    const auto paired_target = secondary_to_primary.find(successor);
                    if (paired_source != secondary_to_primary.end()
                        && paired_target != secondary_to_primary.end()
                        && primary_edges_by_address.find({paired_source->second, paired_target->second}) != primary_edges_by_address.end()) {
                        edge_kind = GraphDiffEdgeKind::common_cfg;
                    }
                }
                context.edges.push_back({parent->second, found->second, edge_kind});
            }
        }
    }
    return context;
}

graph_id_t next_graph_diff_id()
{
    static std::uint64_t counter = 0;
    netnode id;
    qstring node_name;
    node_name.sprnt("$ soff graph diff %llu", static_cast<unsigned long long>(++counter));
    id.create(node_name.c_str());
    return static_cast<graph_id_t>(static_cast<nodeidx_t>(id));
}

void sync_peer_graph_node(GraphDiffContext& context, int node)
{
    if (node < 0
        || static_cast<std::size_t>(node) >= context.peer_node_by_node.size()
        || context.peer == nullptr
        || context.peer->viewer == nullptr) {
        return;
    }
    const auto peer_node = context.peer_node_by_node[static_cast<std::size_t>(node)];
    if (peer_node < 0) {
        return;
    }
    viewer_center_on(context.peer->viewer, peer_node);
    refresh_viewer(context.peer->viewer);
}

ssize_t idaapi soff_graph_diff_callback(void* user_data, int code, va_list va)
{
    auto& context = *static_cast<GraphDiffContext*>(user_data);
    switch (code) {
    case grcode_user_refresh:
    {
        auto* graph = va_arg(va, interactive_graph_t*);
        graph->clear();
        graph->resize(static_cast<int>(context.nodes.size()));
        for (const auto& edge : context.edges) {
            if (edge.source >= 0
                && edge.target >= 0
                && edge.source < graph->size()
                && edge.target < graph->size()) {
                edge_info_t info;
                info.color = graph_edge_color(edge.kind);
                info.width = edge.kind == GraphDiffEdgeKind::paired_changed ? 2 : 1;
                graph->add_edge(edge.source, edge.target, &info);
            }
        }
        for (std::size_t i = 0; i < context.nodes.size(); ++i) {
            node_info_t info;
            info.bg_color = context.nodes[i].color;
            info.frame_color = context.nodes[i].color;
            info.flags = NIFF_SHOW_CONTENTS;
            if (context.nodes[i].primary != 0) {
                info.ea = static_cast<ea_t>(context.nodes[i].primary);
            }
            info.text = context.nodes[i].text.c_str();
            set_node_info(graph->gid, static_cast<int>(i), info, info.get_flags_for_valid());
        }
        graph->create_digraph_layout();
        return true;
    }
    case grcode_clicked:
    {
        qnotused(va_arg(va, graph_viewer_t*));
        const auto* selection = va_arg(va, selection_item_t*);
        qnotused(va_arg(va, graph_item_t*));
        if (selection != nullptr && selection->is_node) {
            active_graph_diff_context() = &context;
            active_graph_diff_node() = selection->node;
            sync_peer_graph_node(context, selection->node);
        }
        return false;
    }
    case grcode_user_text:
    {
        qnotused(va_arg(va, interactive_graph_t*));
        const int node = va_arg(va, int);
        const char** text = va_arg(va, const char**);
        bgcolor_t* color = va_arg(va, bgcolor_t*);
        if (node < 0 || static_cast<std::size_t>(node) >= context.nodes.size()) {
            return false;
        }
        *text = context.nodes[static_cast<std::size_t>(node)].text.c_str();
        if (color != nullptr) {
            *color = context.nodes[static_cast<std::size_t>(node)].color;
        }
        return true;
    }
    case grcode_dblclicked:
    {
        qnotused(va_arg(va, graph_viewer_t*));
        const auto* selection = va_arg(va, selection_item_t*);
        if (selection == nullptr || !selection->is_node) {
            return false;
        }
        const auto node = selection->node;
        if (node < 0 || static_cast<std::size_t>(node) >= context.nodes.size()) {
            return false;
        }
        const auto primary = context.nodes[static_cast<std::size_t>(node)].primary;
        if (primary != 0 && is_mapped(static_cast<ea_t>(primary))) {
            jumpto(static_cast<ea_t>(primary));
            return true;
        }
        return false;
    }
    case grcode_gotfocus:
        qnotused(va_arg(va, graph_viewer_t*));
        active_graph_diff_context() = &context;
        active_graph_diff_node() = context.viewer != nullptr ? viewer_get_curnode(context.viewer) : -1;
        return false;
    case grcode_user_hint:
    {
        qnotused(va_arg(va, interactive_graph_t*));
        const int node = va_argi(va, int);
        qnotused(va_argi(va, int));
        qnotused(va_argi(va, int));
        char** hint = va_arg(va, char**);
        if (node >= 0 && static_cast<std::size_t>(node) < context.nodes.size()) {
            const auto& graph_node = context.nodes[static_cast<std::size_t>(node)];
            std::ostringstream text;
            text << (graph_node.side == GraphDiffNodeSide::primary ? "Primary node" : "Secondary node")
                << "\nPrimary: " << address_hex(graph_node.primary)
                << "\nSecondary DB: " << address_hex(graph_node.secondary)
                << "\nDouble-click jumps to the primary IDB address when mapped.";
            *hint = qstrdup(text.str().c_str());
            return true;
        }
        return false;
    }
    default:
        return false;
    }
}

GraphDiffContext* open_graph_diff_context(GraphDiffContext context, uint32 display_options)
{
    auto owned_context = std::make_unique<GraphDiffContext>(std::move(context));
    owned_context->graph_id = next_graph_diff_id();
    auto* raw_context = owned_context.get();
    graph_diff_contexts().push_back(std::move(owned_context));

    auto* viewer = create_graph_viewer(
        raw_context->title.c_str(),
        raw_context->graph_id,
        soff_graph_diff_callback,
        raw_context,
        0);
    if (viewer == nullptr) {
        throw soff::Error(soff::ErrorCode::viewer_failed, "failed to create IDA graph viewer");
    }
    raw_context->viewer = viewer;
    raw_context->peer_node_by_node.assign(raw_context->nodes.size(), -1);
    display_widget(viewer, display_options);
    attach_action_to_popup(viewer, nullptr, graph_sync_action_name, "Soff/");
    attach_action_to_popup(viewer, nullptr, graph_jump_action_name, "Soff/");
    attach_action_to_popup(viewer, nullptr, graph_text_action_name, "Soff/");
    attach_action_to_popup(viewer, nullptr, graph_import_action_name, "Soff/");
    refresh_viewer(viewer);
    viewer_fit_window(viewer);
    return raw_context;
}

void link_graph_diff_peers(GraphDiffContext& primary_context, GraphDiffContext& secondary_context)
{
    primary_context.peer = &secondary_context;
    secondary_context.peer = &primary_context;
    primary_context.peer_node_by_node.assign(primary_context.nodes.size(), -1);
    secondary_context.peer_node_by_node.assign(secondary_context.nodes.size(), -1);

    std::map<std::pair<soff::Address, soff::Address>, int> primary_nodes;
    for (std::size_t i = 0; i < primary_context.nodes.size(); ++i) {
        const auto& node = primary_context.nodes[i];
        if (node.primary != 0 && node.secondary != 0) {
            primary_nodes.emplace(std::make_pair(node.primary, node.secondary), static_cast<int>(i));
        }
    }
    for (std::size_t i = 0; i < secondary_context.nodes.size(); ++i) {
        const auto& node = secondary_context.nodes[i];
        if (node.primary == 0 || node.secondary == 0) {
            continue;
        }
        const auto found = primary_nodes.find({node.primary, node.secondary});
        if (found == primary_nodes.end()) {
            continue;
        }
        primary_context.peer_node_by_node[static_cast<std::size_t>(found->second)] = static_cast<int>(i);
        secondary_context.peer_node_by_node[i] = found->second;
    }
}

bool show_ida_match_graph_diff(
    const soff::db::DiffResultSet& results,
    const soff::db::ResultMatch& match,
    soff::ui::GraphDiffKind kind,
    PathCache<soff::ProgramSnapshot>* primary_snapshot_cache,
    PathCache<soff::ProgramSnapshot>* secondary_snapshot_cache)
{
    try {
        if (const auto error = validate_export_database(results.main_db, "Primary"); !error.empty()) {
            throw soff::Error(soff::ErrorCode::diff_failed, error);
        }
        if (const auto error = validate_export_database(results.diff_db, "Secondary"); !error.empty()) {
            throw soff::Error(soff::ErrorCode::diff_failed, error);
        }

        PathCache<soff::ProgramSnapshot> primary_uncached;
        PathCache<soff::ProgramSnapshot> secondary_uncached;
        auto& primary_cache = primary_snapshot_cache != nullptr ? *primary_snapshot_cache : primary_uncached;
        auto& secondary_cache = secondary_snapshot_cache != nullptr ? *secondary_snapshot_cache : secondary_uncached;
        const auto& primary_snapshot = load_snapshot_cached(primary_cache, results.main_db);
        const auto& secondary_snapshot = load_snapshot_cached(secondary_cache, results.diff_db);
        const auto* primary = find_function_by_address(primary_snapshot, match.primary);
        const auto* secondary = find_function_by_address(secondary_snapshot, match.secondary);
        if (primary == nullptr || secondary == nullptr) {
            throw soff::Error(soff::ErrorCode::function_not_found, "matched functions are missing from export snapshots");
        }

        auto selected_kind = kind;
        const bool use_native = kind == soff::ui::GraphDiffKind::native
            || kind == soff::ui::GraphDiffKind::pseudocode;
        const auto has_primary_graph = use_native
            ? !primary->blocks.empty()
            : !primary->microcode_blocks.empty();
        const auto has_secondary_graph = use_native
            ? !secondary->blocks.empty()
            : !secondary->microcode_blocks.empty();
        if (!has_primary_graph && !has_secondary_graph) {
            const bool native_available = !primary->blocks.empty() || !secondary->blocks.empty();
            if (kind == soff::ui::GraphDiffKind::microcode && native_available) {
                selected_kind = soff::ui::GraphDiffKind::native;
                msg("Soff: microcode graph blocks are missing for this match; falling back to native function graph. Re-export both databases with Use decompiler enabled for microcode graph diff.\n");
            } else {
                throw soff::Error(soff::ErrorCode::diff_failed, kind == soff::ui::GraphDiffKind::native
                        ? "no native graph blocks were exported for this match"
                        : "no microcode graph blocks were exported for this match; re-export both databases with Use decompiler enabled");
            }
        }

        const auto primary_label = side_function_label(results.main_db, match.primary_name);
        const auto secondary_label = side_function_label(results.diff_db, match.secondary_name);
        const auto primary_file = db_file_label(results.main_db);
        const auto secondary_file = db_file_label(results.diff_db);

        auto* primary_context = open_graph_diff_context(
            build_single_graph_context(*primary, *secondary, selected_kind, true, primary_label, secondary_label, primary_file, secondary_file, match.ratio, match.description),
            WOPN_DP_LEFT | WOPN_RESTORE | WOPN_PERSIST);
        auto* secondary_context = open_graph_diff_context(
            build_single_graph_context(*primary, *secondary, selected_kind, false, primary_label, secondary_label, primary_file, secondary_file, match.ratio, match.description),
            WOPN_DP_RIGHT | WOPN_RESTORE | WOPN_PERSIST);
        if (primary_context != nullptr && secondary_context != nullptr) {
            primary_context->main_db = results.main_db;
            primary_context->diff_db = results.diff_db;
            primary_context->match = match;
            primary_context->has_match = true;
            primary_context->primary_snapshot_cache = primary_cache;
            primary_context->secondary_snapshot_cache = secondary_cache;
            secondary_context->main_db = results.main_db;
            secondary_context->diff_db = results.diff_db;
            secondary_context->match = match;
            secondary_context->has_match = true;
            secondary_context->primary_snapshot_cache = primary_cache;
            secondary_context->secondary_snapshot_cache = secondary_cache;
            link_graph_diff_peers(*primary_context, *secondary_context);
        }
        if (primary_context != nullptr && primary_context->viewer != nullptr) {
            activate_widget(primary_context->viewer, true);
        }
        msg("Soff: opened side-by-side IDA graph diff for %s -> %s\n", match.primary_name.c_str(), match.secondary_name.c_str());
        return true;
    } catch (const std::exception& error) {
        warning("Soff failed to create IDA graph diff:\n%s", error.what());
    } catch (...) {
        warning("Soff failed to create IDA graph diff with an unknown error");
    }
    return false;
}

bool save_or_open_match_graph_diff(
    const std::filesystem::path& result_path,
    const soff::db::DiffResultSet& results,
    const soff::db::ResultMatch& match,
    PathCache<soff::ProgramSnapshot>* primary_snapshot_cache,
    PathCache<soff::ProgramSnapshot>* secondary_snapshot_cache)
{
    try {
        const int graph_choice = ask_buttons(
            "~N~ative CFG",
            "~M~icrocode CFG",
            "~C~ancel",
            ASKBTN_YES,
            "Choose graph diff kind");
        if (graph_choice != ASKBTN_YES && graph_choice != ASKBTN_NO) {
            return false;
        }
        const auto kind = graph_choice == ASKBTN_YES
            ? soff::ui::GraphDiffKind::native
            : soff::ui::GraphDiffKind::microcode;

        if (const auto error = validate_export_database(results.main_db, "Primary"); !error.empty()) {
            throw soff::Error(soff::ErrorCode::diff_failed, error);
        }
        if (const auto error = validate_export_database(results.diff_db, "Secondary"); !error.empty()) {
            throw soff::Error(soff::ErrorCode::diff_failed, error);
        }

        PathCache<soff::ProgramSnapshot> primary_uncached;
        PathCache<soff::ProgramSnapshot> secondary_uncached;
        const auto& primary_snapshot = load_snapshot_cached(
            primary_snapshot_cache != nullptr ? *primary_snapshot_cache : primary_uncached,
            results.main_db);
        const auto& secondary_snapshot = load_snapshot_cached(
            secondary_snapshot_cache != nullptr ? *secondary_snapshot_cache : secondary_uncached,
            results.diff_db);
        const auto* primary = find_function_by_address(primary_snapshot, match.primary);
        const auto* secondary = find_function_by_address(secondary_snapshot, match.secondary);
        if (primary == nullptr || secondary == nullptr) {
            throw soff::Error(soff::ErrorCode::function_not_found, "matched functions are missing from export snapshots");
        }

        const auto has_primary_graph = kind == soff::ui::GraphDiffKind::native
            ? !primary->blocks.empty()
            : !primary->microcode_blocks.empty();
        const auto has_secondary_graph = kind == soff::ui::GraphDiffKind::native
            ? !secondary->blocks.empty()
            : !secondary->microcode_blocks.empty();
        if (!has_primary_graph && !has_secondary_graph) {
            throw soff::Error(soff::ErrorCode::diff_failed, kind == soff::ui::GraphDiffKind::native
                    ? "no native graph blocks were exported for this match"
                    : "no microcode graph blocks were exported for this match");
        }

        auto primary_feature = *primary;
        auto secondary_feature = *secondary;
        primary_feature.name = side_function_label(results.main_db, match.primary_name);
        secondary_feature.name = side_function_label(results.diff_db, match.secondary_name);
        const auto html = soff::ui::render_function_graph_diff_html(
            primary_feature,
            secondary_feature,
            kind,
            match.ratio,
            match.description);
        const auto output_path = default_graph_diff_path(result_path, match, kind, true);
        if (!write_text_file(output_path, html)) {
            throw soff::Error(soff::ErrorCode::diff_failed, "failed to write graph diff: " + output_path.string());
        }
        if (!open_file_with_shell(output_path)) {
            throw soff::Error(soff::ErrorCode::diff_failed, "failed to open graph diff: " + output_path.string());
        }
        msg("Soff: opened graph diff %s\n", output_path.string().c_str());
        return true;
    } catch (const std::exception& error) {
        warning("Soff failed to create graph diff:\n%s", error.what());
    } catch (...) {
        warning("Soff failed to create graph diff with an unknown error");
    }
    return false;
}

bool save_or_open_match_call_context(
    const std::filesystem::path& result_path,
    const soff::db::DiffResultSet& results,
    const soff::db::ResultMatch& match,
    PathCache<soff::ProgramSnapshot>* primary_snapshot_cache,
    PathCache<soff::ProgramSnapshot>* secondary_snapshot_cache)
{
    try {
        if (const auto error = validate_export_database(results.main_db, "Primary"); !error.empty()) {
            throw soff::Error(soff::ErrorCode::diff_failed, error);
        }
        if (const auto error = validate_export_database(results.diff_db, "Secondary"); !error.empty()) {
            throw soff::Error(soff::ErrorCode::diff_failed, error);
        }

        PathCache<soff::ProgramSnapshot> primary_uncached;
        PathCache<soff::ProgramSnapshot> secondary_uncached;
        const auto& primary_snapshot = load_snapshot_cached(
            primary_snapshot_cache != nullptr ? *primary_snapshot_cache : primary_uncached,
            results.main_db);
        const auto& secondary_snapshot = load_snapshot_cached(
            secondary_snapshot_cache != nullptr ? *secondary_snapshot_cache : secondary_uncached,
            results.diff_db);
        const auto* primary = find_function_by_address(primary_snapshot, match.primary);
        const auto* secondary = find_function_by_address(secondary_snapshot, match.secondary);
        if (primary == nullptr || secondary == nullptr) {
            throw soff::Error(soff::ErrorCode::function_not_found, "matched functions are missing from export snapshots");
        }
        if (primary->call_references.empty() && secondary->call_references.empty()) {
            bool has_primary_caller = false;
            bool has_secondary_caller = false;
            for (const auto& function : primary_snapshot.functions) {
                has_primary_caller = has_primary_caller || std::any_of(
                    function.call_references.begin(),
                    function.call_references.end(),
                    [primary](const soff::CallReference& call) {
                        return call.address == primary->address
                            || (primary->size != 0 && call.address >= primary->address && call.address < primary->address + primary->size);
                    });
            }
            for (const auto& function : secondary_snapshot.functions) {
                has_secondary_caller = has_secondary_caller || std::any_of(
                    function.call_references.begin(),
                    function.call_references.end(),
                    [secondary](const soff::CallReference& call) {
                        return call.address == secondary->address
                            || (secondary->size != 0 && call.address >= secondary->address && call.address < secondary->address + secondary->size);
                    });
            }
            if (!has_primary_caller && !has_secondary_caller) {
                throw soff::Error(soff::ErrorCode::diff_failed, "no call context was exported for this match");
            }
        }

        const auto html = soff::ui::render_call_context_diff_html(
            primary_snapshot,
            secondary_snapshot,
            match.primary,
            match.secondary,
            match.ratio,
            match.description);
        const auto output_path = default_call_context_path(result_path, match, true);
        if (!write_text_file(output_path, html)) {
            throw soff::Error(soff::ErrorCode::diff_failed, "failed to write call context graph: " + output_path.string());
        }
        if (!open_file_with_shell(output_path)) {
            throw soff::Error(soff::ErrorCode::diff_failed, "failed to open call context graph: " + output_path.string());
        }
        msg("Soff: opened call context graph %s\n", output_path.string().c_str());
        return true;
    } catch (const std::exception& error) {
        warning("Soff failed to create call context graph:\n%s", error.what());
    } catch (...) {
        warning("Soff failed to create call context graph with an unknown error");
    }
    return false;
}

const char* text_diff_kind_name(TextDiffKind kind)
{
    switch (kind) {
    case TextDiffKind::assembly:
        return "assembly";
    case TextDiffKind::pseudocode:
        return "pseudocode";
    case TextDiffKind::microcode:
        return "microcode";
    }
    return "text";
}

std::string text_diff_text(const soff::FunctionFeature& function, TextDiffKind kind)
{
    switch (kind) {
    case TextDiffKind::assembly:
        return function.assembly.empty() ? function.stripped_assembly : function.assembly;
    case TextDiffKind::pseudocode:
        return function.pseudocode.empty() ? function.stripped_pseudocode : function.pseudocode;
    case TextDiffKind::microcode:
        return function.microcode.empty() ? function.stripped_microcode : function.microcode;
    }
    return {};
}

struct TextDiffRow
{
    soff::ui::DiffLineKind kind = soff::ui::DiffLineKind::equal;
    std::string left;
    std::string right;
    std::size_t left_line = 0;
    std::size_t right_line = 0;
};

std::vector<std::string> split_text_lines(std::string_view text)
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

std::vector<TextDiffRow> build_text_diff_rows(const std::vector<std::string>& left, const std::vector<std::string>& right)
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

    std::vector<TextDiffRow> diff;
    std::size_t left_index = 0;
    std::size_t right_index = 0;
    while (left_index < rows || right_index < cols) {
        if (left_index < rows && right_index < cols && left[left_index] == right[right_index]) {
            diff.push_back({soff::ui::DiffLineKind::equal, left[left_index], right[right_index], left_index + 1, right_index + 1});
            ++left_index;
            ++right_index;
        } else if (left_index < rows && right_index < cols
            && (left_index + 1 == rows || right_index + 1 == cols || at(left_index + 1, right_index + 1) >= at(left_index + 1, right_index) + at(left_index, right_index + 1))) {
            diff.push_back({soff::ui::DiffLineKind::changed, left[left_index], right[right_index], left_index + 1, right_index + 1});
            ++left_index;
            ++right_index;
        } else if (right_index < cols && (left_index == rows || at(left_index, right_index + 1) >= at(left_index + 1, right_index))) {
            diff.push_back({soff::ui::DiffLineKind::inserted, "", right[right_index], 0, right_index + 1});
            ++right_index;
        } else if (left_index < rows) {
            diff.push_back({soff::ui::DiffLineKind::deleted, left[left_index], "", left_index + 1, 0});
            ++left_index;
        }
    }
    return diff;
}

const char* text_diff_marker(soff::ui::DiffLineKind kind)
{
    switch (kind) {
    case soff::ui::DiffLineKind::equal:
        return "  ";
    case soff::ui::DiffLineKind::changed:
        return "!=";
    case soff::ui::DiffLineKind::inserted:
        return "++";
    case soff::ui::DiffLineKind::deleted:
        return "--";
    }
    return "  ";
}

std::vector<std::string> address_search_terms(soff::Address address)
{
    if (address == 0) {
        return {};
    }
    auto lower = address_hex(address);
    auto upper = lower;
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    std::ostringstream bare;
    bare << std::hex << address;
    auto bare_text = bare.str();
    auto bare_upper = bare_text;
    std::transform(bare_upper.begin(), bare_upper.end(), bare_upper.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return {lower, upper, bare_text, bare_upper};
}

bool contains_any_term(const std::string& text, const std::vector<std::string>& terms)
{
    for (const auto& term : terms) {
        if (!term.empty() && text.find(term) != std::string::npos) {
            return true;
        }
    }
    return false;
}

struct TextDiffViewerContext
{
    std::string title;
    strvec_t lines;
};

void idaapi close_text_diff_viewer(TWidget*, void* context)
{
    delete static_cast<TextDiffViewerContext*>(context);
}

bool show_text_diff_viewer(
    std::string title,
    std::string_view primary_name,
    std::string_view secondary_name,
    std::string_view primary_text,
    std::string_view secondary_text,
    soff::Address focus_primary = 0,
    soff::Address focus_secondary = 0)
{
    auto context = std::make_unique<TextDiffViewerContext>();
    context->title = std::move(title);
    context->lines.push_back(simpleline_t(("Soff internal text diff: " + context->title).c_str()));
    context->lines.push_back(simpleline_t(("Primary:   " + std::string(primary_name)).c_str()));
    context->lines.push_back(simpleline_t(("Secondary: " + std::string(secondary_name)).c_str()));
    if (focus_primary != 0 || focus_secondary != 0) {
        std::ostringstream focus;
        focus << "Focus block: primary=" << address_hex(focus_primary)
            << " secondary=" << address_hex(focus_secondary);
        context->lines.push_back(simpleline_t(focus.str().c_str()));
    }
    context->lines.push_back(simpleline_t("Legend: != changed, ++ secondary-only, -- primary-only"));
    context->lines.push_back(simpleline_t(""));
    context->lines.push_back(simpleline_t("     L#      R#      Primary | Secondary"));

    const auto primary_lines = split_text_lines(primary_text);
    const auto secondary_lines = split_text_lines(secondary_text);
    int focus_line = -1;
    const auto primary_focus_terms = address_search_terms(focus_primary);
    const auto secondary_focus_terms = address_search_terms(focus_secondary);
    for (const auto& row : build_text_diff_rows(primary_lines, secondary_lines)) {
        std::ostringstream line;
        line << text_diff_marker(row.kind) << " "
            << std::setw(6) << (row.left_line == 0 ? std::string("-") : std::to_string(row.left_line))
            << " "
            << std::setw(6) << (row.right_line == 0 ? std::string("-") : std::to_string(row.right_line))
            << "  "
            << row.left
            << "  |  "
            << row.right;
        if (focus_line < 0
            && (contains_any_term(row.left, primary_focus_terms)
                || contains_any_term(row.right, secondary_focus_terms))) {
            focus_line = static_cast<int>(context->lines.size());
        }
        context->lines.push_back(simpleline_t(line.str().c_str()));
    }

    if (context->lines.empty()) {
        context->lines.push_back(simpleline_t("(empty diff)"));
    }

    simpleline_place_t min_place(0);
    simpleline_place_t first_place(focus_line >= 0 ? focus_line : 0);
    simpleline_place_t last_place(static_cast<int>(context->lines.size() - 1));
    static const custom_viewer_handlers_t handlers(
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        close_text_diff_viewer);
    auto* raw_context = context.get();
    TWidget* viewer = create_custom_viewer(
        raw_context->title.c_str(),
        &min_place,
        &last_place,
        &first_place,
        nullptr,
        &raw_context->lines,
        &handlers,
        raw_context);
    if (viewer == nullptr) {
        return false;
    }
    context.release();
    display_widget(viewer, WOPN_DP_TAB | WOPN_RESTORE | WOPN_PERSIST);
    return true;
}

bool show_linear_asm_diff(
    const soff::db::DiffResultSet& results,
    const soff::db::ResultMatch& match,
    PathCache<soff::ProgramSnapshot>* primary_snapshot_cache,
    PathCache<soff::ProgramSnapshot>* secondary_snapshot_cache)
{
    try {
        if (const auto error = validate_export_database(results.main_db, "Primary"); !error.empty()) {
            throw soff::Error(soff::ErrorCode::diff_failed, error);
        }
        if (const auto error = validate_export_database(results.diff_db, "Secondary"); !error.empty()) {
            throw soff::Error(soff::ErrorCode::diff_failed, error);
        }

        PathCache<soff::ProgramSnapshot> primary_uncached;
        PathCache<soff::ProgramSnapshot> secondary_uncached;
        const auto& primary_snapshot = load_snapshot_cached(
            primary_snapshot_cache != nullptr ? *primary_snapshot_cache : primary_uncached,
            results.main_db);
        const auto& secondary_snapshot = load_snapshot_cached(
            secondary_snapshot_cache != nullptr ? *secondary_snapshot_cache : secondary_uncached,
            results.diff_db);
        const auto* primary = find_function_by_address(primary_snapshot, match.primary);
        const auto* secondary = find_function_by_address(secondary_snapshot, match.secondary);
        if (primary == nullptr || secondary == nullptr) {
            throw soff::Error(soff::ErrorCode::function_not_found, "matched functions are missing from export snapshots");
        }

        // Build IDA-listing-style lines from instruction details
        auto build_listing = [](const soff::FunctionFeature& func) {
            std::vector<std::string> lines;
            for (const auto& ins : func.instruction_details) {
                std::ostringstream line;
                line << ".text:" << std::uppercase << std::hex
                    << std::setfill('0') << std::setw(16) << ins.address
                    << " " << ins.disassembly;
                lines.push_back(line.str());
            }
            return lines;
        };

        const auto p_lines = build_listing(*primary);
        const auto s_lines = build_listing(*secondary);

        const auto diff = soff::ui::compute_line_diff(p_lines, s_lines);

        // Build left/right viewer lines with color
        auto left_ctx = std::make_unique<TextDiffViewerContext>();
        auto right_ctx = std::make_unique<TextDiffViewerContext>();
        const auto primary_file = db_file_label(results.main_db);
        const auto secondary_file = db_file_label(results.diff_db);
        left_ctx->title = "Linear Asm: " + primary_file + " / " + match.primary_name;
        right_ctx->title = "Linear Asm: " + secondary_file + " / " + match.secondary_name;

        std::string color_on;
        color_on += '\x01';
        color_on += '\x12';
        std::string color_off;
        color_off += '\x02';
        color_off += '\x12';

        for (const auto& d : diff) {
            if (d.kind == soff::ui::DiffEntry::same) {
                left_ctx->lines.push_back(simpleline_t(p_lines[d.left_index].c_str()));
                right_ctx->lines.push_back(simpleline_t(s_lines[d.right_index].c_str()));
            } else if (d.kind == soff::ui::DiffEntry::removed) {
                auto colored = color_on + p_lines[d.left_index] + color_off;
                left_ctx->lines.push_back(simpleline_t(colored.c_str()));
                right_ctx->lines.push_back(simpleline_t(""));
            } else {
                left_ctx->lines.push_back(simpleline_t(""));
                auto colored = color_on + s_lines[d.right_index] + color_off;
                right_ctx->lines.push_back(simpleline_t(colored.c_str()));
            }
        }

        if (left_ctx->lines.empty()) {
            left_ctx->lines.push_back(simpleline_t("(empty)"));
            right_ctx->lines.push_back(simpleline_t("(empty)"));
        }

        // Create two custom viewers docked left/right
        static const custom_viewer_handlers_t handlers(
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
            close_text_diff_viewer);

        simpleline_place_t l_min(0), l_max(static_cast<int>(left_ctx->lines.size() - 1)), l_first(0);
        auto* left_raw = left_ctx.get();
        TWidget* left_viewer = create_custom_viewer(
            left_raw->title.c_str(), &l_min, &l_max, &l_first,
            nullptr, &left_raw->lines, &handlers, left_raw);

        simpleline_place_t r_min(0), r_max(static_cast<int>(right_ctx->lines.size() - 1)), r_first(0);
        auto* right_raw = right_ctx.get();
        TWidget* right_viewer = create_custom_viewer(
            right_raw->title.c_str(), &r_min, &r_max, &r_first,
            nullptr, &right_raw->lines, &handlers, right_raw);

        if (left_viewer == nullptr || right_viewer == nullptr) {
            throw soff::Error(soff::ErrorCode::viewer_failed, "failed to create viewers");
        }
        left_ctx.release();
        right_ctx.release();
        display_widget(left_viewer, WOPN_DP_LEFT | WOPN_RESTORE | WOPN_PERSIST);
        display_widget(right_viewer, WOPN_DP_RIGHT | WOPN_RESTORE | WOPN_PERSIST);
        msg("Soff: opened linear assembly diff for %s -> %s\n", match.primary_name.c_str(), match.secondary_name.c_str());
        return true;
    } catch (const std::exception& error) {
        warning("Soff failed to create linear assembly diff:\n%s", error.what());
    }
    return false;
}

bool show_linear_pseudo_diff(
    const soff::db::DiffResultSet& results,
    const soff::db::ResultMatch& match,
    PathCache<soff::ProgramSnapshot>* primary_snapshot_cache,
    PathCache<soff::ProgramSnapshot>* secondary_snapshot_cache)
{
    try {
        if (const auto error = validate_export_database(results.main_db, "Primary"); !error.empty()) {
            throw soff::Error(soff::ErrorCode::diff_failed, error);
        }
        if (const auto error = validate_export_database(results.diff_db, "Secondary"); !error.empty()) {
            throw soff::Error(soff::ErrorCode::diff_failed, error);
        }

        PathCache<soff::ProgramSnapshot> primary_uncached;
        PathCache<soff::ProgramSnapshot> secondary_uncached;
        const auto& primary_snapshot = load_snapshot_cached(
            primary_snapshot_cache != nullptr ? *primary_snapshot_cache : primary_uncached,
            results.main_db);
        const auto& secondary_snapshot = load_snapshot_cached(
            secondary_snapshot_cache != nullptr ? *secondary_snapshot_cache : secondary_uncached,
            results.diff_db);
        const auto* primary = find_function_by_address(primary_snapshot, match.primary);
        const auto* secondary = find_function_by_address(secondary_snapshot, match.secondary);
        if (primary == nullptr || secondary == nullptr) {
            throw soff::Error(soff::ErrorCode::function_not_found, "matched functions are missing from export snapshots");
        }

        auto split_lines = [](const std::string& text) {
            std::vector<std::string> lines;
            std::istringstream stream(text);
            std::string line;
            while (std::getline(stream, line)) {
                lines.push_back(line);
            }
            return lines;
        };

        const auto& p_pseudo = primary->pseudocode.empty()
            ? primary->stripped_pseudocode : primary->pseudocode;
        const auto& s_pseudo = secondary->pseudocode.empty()
            ? secondary->stripped_pseudocode : secondary->pseudocode;
        if (p_pseudo.empty() && s_pseudo.empty()) {
            throw soff::Error(soff::ErrorCode::diff_failed, 
                "no pseudocode exported; re-export with decompiler enabled");
        }

        auto p_lines = split_lines(p_pseudo);
        auto s_lines = split_lines(s_pseudo);

        // Prepend function signature
        if (!primary->prototype.empty()) {
            p_lines.insert(p_lines.begin(), primary->prototype);
        }
        if (!secondary->prototype.empty()) {
            s_lines.insert(s_lines.begin(), secondary->prototype);
        }

        const auto diff = soff::ui::compute_line_diff(p_lines, s_lines);

        auto left_ctx = std::make_unique<TextDiffViewerContext>();
        auto right_ctx = std::make_unique<TextDiffViewerContext>();
        const auto primary_file = db_file_label(results.main_db);
        const auto secondary_file = db_file_label(results.diff_db);
        left_ctx->title = "Pseudocode: " + primary_file + " / "
            + match.primary_name;
        right_ctx->title = "Pseudocode: " + secondary_file + " / "
            + match.secondary_name;

        std::string color_on;  color_on  += '\x01'; color_on  += '\x12';
        std::string color_off; color_off += '\x02'; color_off += '\x12';

        for (const auto& d : diff) {
            if (d.kind == soff::ui::DiffEntry::same) {
                left_ctx->lines.push_back(
                    simpleline_t(p_lines[d.left_index].c_str()));
                right_ctx->lines.push_back(
                    simpleline_t(s_lines[d.right_index].c_str()));
            } else if (d.kind == soff::ui::DiffEntry::removed) {
                auto c = color_on + p_lines[d.left_index] + color_off;
                left_ctx->lines.push_back(simpleline_t(c.c_str()));
                right_ctx->lines.push_back(simpleline_t(""));
            } else {
                left_ctx->lines.push_back(simpleline_t(""));
                auto c = color_on + s_lines[d.right_index] + color_off;
                right_ctx->lines.push_back(simpleline_t(c.c_str()));
            }
        }

        if (left_ctx->lines.empty()) {
            left_ctx->lines.push_back(simpleline_t("(empty)"));
            right_ctx->lines.push_back(simpleline_t("(empty)"));
        }

        static const custom_viewer_handlers_t handlers(
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
            close_text_diff_viewer);

        simpleline_place_t l_min(0);
        simpleline_place_t l_max(
            static_cast<int>(left_ctx->lines.size() - 1));
        simpleline_place_t l_first(0);
        auto* left_raw = left_ctx.get();
        TWidget* left_v = create_custom_viewer(
            left_raw->title.c_str(), &l_min, &l_max, &l_first,
            nullptr, &left_raw->lines, &handlers, left_raw);

        simpleline_place_t r_min(0);
        simpleline_place_t r_max(
            static_cast<int>(right_ctx->lines.size() - 1));
        simpleline_place_t r_first(0);
        auto* right_raw = right_ctx.get();
        TWidget* right_v = create_custom_viewer(
            right_raw->title.c_str(), &r_min, &r_max, &r_first,
            nullptr, &right_raw->lines, &handlers, right_raw);

        if (left_v == nullptr || right_v == nullptr) {
            throw soff::Error(soff::ErrorCode::viewer_failed, "failed to create viewers");
        }
        left_ctx.release();
        right_ctx.release();
        display_widget(left_v, WOPN_DP_LEFT | WOPN_RESTORE | WOPN_PERSIST);
        display_widget(right_v, WOPN_DP_RIGHT | WOPN_RESTORE | WOPN_PERSIST);
        msg("Soff: opened pseudocode diff for %s -> %s\n",
            match.primary_name.c_str(), match.secondary_name.c_str());
        return true;
    } catch (const std::exception& error) {
        warning("Soff failed to create pseudocode diff:\n%s", error.what());
    }
    return false;
}

bool show_ida_match_text_diff(
    const soff::db::DiffResultSet& results,
    const soff::db::ResultMatch& match,
    TextDiffKind kind,
    soff::Address focus_primary,
    soff::Address focus_secondary,
    PathCache<soff::ProgramSnapshot>* primary_snapshot_cache,
    PathCache<soff::ProgramSnapshot>* secondary_snapshot_cache)
{
    try {
        if (const auto error = validate_export_database(results.main_db, "Primary"); !error.empty()) {
            throw soff::Error(soff::ErrorCode::diff_failed, error);
        }
        if (const auto error = validate_export_database(results.diff_db, "Secondary"); !error.empty()) {
            throw soff::Error(soff::ErrorCode::diff_failed, error);
        }

        PathCache<soff::ProgramSnapshot> primary_uncached;
        PathCache<soff::ProgramSnapshot> secondary_uncached;
        const auto& primary_snapshot = load_snapshot_cached(
            primary_snapshot_cache != nullptr ? *primary_snapshot_cache : primary_uncached,
            results.main_db);
        const auto& secondary_snapshot = load_snapshot_cached(
            secondary_snapshot_cache != nullptr ? *secondary_snapshot_cache : secondary_uncached,
            results.diff_db);
        const auto* primary = find_function_by_address(primary_snapshot, match.primary);
        const auto* secondary = find_function_by_address(secondary_snapshot, match.secondary);
        if (primary == nullptr || secondary == nullptr) {
            throw soff::Error(soff::ErrorCode::function_not_found, "matched functions are missing from export snapshots");
        }

        const auto primary_text = text_diff_text(*primary, kind);
        const auto secondary_text = text_diff_text(*secondary, kind);
        if (primary_text.empty() && secondary_text.empty()) {
            throw soff::Error(soff::ErrorCode::diff_failed, std::string("no ") + text_diff_kind_name(kind) + " text was exported for this match");
        }

        std::ostringstream title;
        title << "Soff" << text_diff_kind_name(kind) << " diff: "
            << db_file_label(results.main_db) << " vs " << db_file_label(results.diff_db);
        if (!show_text_diff_viewer(
                title.str(),
                side_function_label(results.main_db, match.primary_name),
                side_function_label(results.diff_db, match.secondary_name),
                primary_text,
                secondary_text,
                focus_primary,
                focus_secondary)) {
            throw soff::Error(soff::ErrorCode::viewer_failed, "failed to create viewer");
        }
        msg("Soff: opened IDA %s text diff for %s -> %s\n", text_diff_kind_name(kind), match.primary_name.c_str(), match.secondary_name.c_str());
        return true;
    } catch (const std::exception& error) {
        warning("Soff failed to create IDA text diff:\n%s", error.what());
    } catch (...) {
        warning("Soff failed to create IDA text diff with an unknown error");
    }
    return false;
}

