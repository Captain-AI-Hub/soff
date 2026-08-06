void extract_pseudocode_features(func_t* function, soff::FunctionFeature& feature, HexRaysExportContext& hexrays)
{
    if (!hexrays.available) {
        return;
    }

    hexrays_failure_t failure;
    soff::perf::add_counter("export.hexrays.pseudocode.attempt");
    cfuncptr_t cfunc = decompile_func(function, &failure, DECOMP_NO_WAIT);
    if (cfunc == nullptr) {
        ++hexrays.pseudocode_failures;
        record_hexrays_failure(hexrays, failure);
        return;
    }

    AstPrimeVisitor ast_visitor;
    ast_visitor.apply_to(&cfunc->body, nullptr);
    feature.pseudocode_primes = std::move(ast_visitor.primes_hash);
    attach_pseudocode_comments(cfunc.operator->(), feature, hexrays);

    const strvec_t& lines = cfunc->get_pseudocode();
    std::string decompiler_prototype;
    bool skipped_declaration = false;
    for (int i = 0; i < lines.size(); ++i) {
        auto line = plain_pseudocode_line(lines[i]);
        if (std::string_view(line).rfind("//", 0) == 0) {
            continue;
        }
        if (!skipped_declaration) {
            decompiler_prototype = line;
            skipped_declaration = true;
            continue;
        }
        append_line(feature.pseudocode, line);
        ++feature.pseudocode_lines;
    }

    if (!decompiler_prototype.empty()) {
        feature.prototype = std::move(decompiler_prototype);
    }

    if (feature.pseudocode.empty()) {
        ++hexrays.pseudocode_failures;
        return;
    }

    feature.stripped_pseudocode = clean_pseudocode_text(feature.pseudocode);
    const auto hashes = koret_fuzzy_hashes(feature.pseudocode);
    feature.pseudocode_hash1 = hashes[0];
    feature.pseudocode_hash2 = hashes[1];
    feature.pseudocode_hash3 = hashes[2];
    ++hexrays.pseudocode_functions;
}

void append_unique(std::vector<std::string>& values, std::string value)
{
    if (value.empty()) {
        return;
    }
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(std::move(value));
    }
}

bool is_valid_constant(std::uint64_t value, ea_t ea)
{
    // Filter 1: too small (stack offsets, flags, small immediates)
    if (value < 0x1000) {
        return false;
    }
    // Filter 2: powers of 2 (alignment values)
    if (value != 0 && (value & (value - 1)) == 0) {
        return false;
    }
    // Filter 3: 0xFFFFFF00-style masks (all-ones with trailing zeros)
    const auto inverted = ~value;
    if (inverted != 0 && (inverted & (inverted - 1)) == 0) {
        return false;
    }
    // Filter 4: has data reference (it's an address, not a constant)
    if (is_mapped(static_cast<ea_t>(value))) {
        xrefblk_t xref;
        if (xref.first_to(static_cast<ea_t>(value), XREF_DATA)) {
            return false;
        }
    }
    return true;
}

void append_instruction_to_block(
    std::vector<soff::BasicBlock>& blocks,
    ea_t ea)
{
    const auto address = static_cast<soff::Address>(ea);
    for (auto& block : blocks) {
        if (address >= block.start && address < block.end) {
            block.instructions.push_back(address);
            return;
        }
    }
}

bool starts_with(const std::string& text, const char* prefix)
{
    const std::string_view view(text);
    const std::string_view prefix_view(prefix);
    return view.size() >= prefix_view.size()
        && view.substr(0, prefix_view.size()) == prefix_view;
}

std::string referenced_name(ea_t ea)
{
    if (ea == BADADDR) {
        return "";
    }

    qstring name;
    if (get_ea_name(&name, ea, GN_VISIBLE | GN_DEMANGLED | GN_SHORT) <= 0) {
        return "";
    }

    std::string text = to_string(name);
    const auto argument_list = text.find('(');
    if (argument_list != std::string::npos) {
        text.erase(argument_list);
    }
    if (starts_with(text, "sub_") || starts_with(text, "nullsub_")) {
        return "";
    }
    return text;
}

ea_t operand_target(const op_t& operand)
{
    switch (operand.type) {
    case o_mem:
    case o_far:
    case o_near:
    case o_displ:
        return operand.addr;
    case o_imm:
        return static_cast<ea_t>(operand.value);
    default:
        return BADADDR;
    }
}

void collect_operand_name(const insn_t& instruction, std::set<std::string>& names)
{
    const op_t* operands[] = {&instruction.ops[1], &instruction.ops[0]};
    for (const auto* operand : operands) {
        if (operand->type == o_void) {
            continue;
        }
        auto name = referenced_name(operand_target(*operand));
        if (!name.empty()) {
            names.insert(std::move(name));
            return;
        }
    }
}

void collect_switch_info(
    ea_t ea,
    std::vector<std::pair<std::size_t, std::vector<sval_t>>>& switches)
{
    switch_info_t switch_info;
    if (get_switch_info(&switch_info, ea) <= 0) {
        return;
    }

    casevec_t case_values;
    eavec_t targets;
    if (!calc_switch_cases(&case_values, &targets, ea, switch_info)) {
        return;
    }

    std::set<sval_t> unique_values;
    for (const auto& current_case : case_values) {
        for (const auto value : current_case) {
            unique_values.insert(value);
        }
    }
    if (unique_values.empty()) {
        return;
    }

    switches.push_back({
        static_cast<std::size_t>(switch_info.get_jtable_size()),
        std::vector<sval_t>(unique_values.begin(), unique_values.end()),
    });
}

std::string skip_reason_for(func_t* function, const std::string& name, const ExportOptions& options)
{
    if (function == nullptr) {
        return "invalid-function";
    }
    if (function->start_ea < options.min_ea || function->start_ea > options.max_ea) {
        return "outside-range";
    }
    if (options.exclude_library_thunk) {
        if ((function->flags & FUNC_LIB) != 0) {
            return "library";
        }
        if ((function->flags & FUNC_THUNK) != 0) {
            return "thunk";
        }
        if (starts_with(name, "nullsub_")) {
            return "nullsub";
        }
    }
    if (options.ignore_small_functions) {
        const auto size = function->end_ea - function->start_ea;
        if (size <= 6) {
            return "small-function";
        }
    }
    return "";
}

std::uint64_t count_incoming_code_refs(ea_t ea)
{
    std::uint64_t count = 0;
    xrefblk_t xref;
    for (bool has_xref = xref.first_to(ea, XREF_CODE | XREF_NOFLOW);
         has_xref;
         has_xref = xref.next_to()) {
        if (xref.iscode) {
            ++count;
        }
    }
    return count;
}

bool is_hash_trimmed_operand(const op_t& operand)
{
    return operand.type == o_mem
        || operand.type == o_imm
        || operand.type == o_far
        || operand.type == o_near
        || operand.type == o_displ;
}

int comparable_instruction_size(const insn_t& instruction, int decoded_size)
{
    auto size = decoded_size;
    if (is_hash_trimmed_operand(instruction.ops[0])) {
        size -= instruction.ops[0].offb;
    }
    if (is_hash_trimmed_operand(instruction.ops[1])) {
        size -= instruction.ops[1].offb;
    }
    return size > 0 ? size : 1;
}

void append_bytes(std::vector<std::uint8_t>& output, ea_t ea, int size)
{
    for (auto offset = 0; offset < size; ++offset) {
        output.push_back(static_cast<std::uint8_t>(get_byte(ea + offset)));
    }
}

std::vector<std::vector<std::size_t>> adjacency_for_blocks(const std::vector<soff::BasicBlock>& blocks)
{
    std::unordered_map<soff::Address, std::size_t> index_by_start;
    for (std::size_t index = 0; index < blocks.size(); ++index) {
        index_by_start[blocks[index].start] = index;
    }

    std::vector<std::vector<std::size_t>> graph(blocks.size());
    for (std::size_t index = 0; index < blocks.size(); ++index) {
        for (const auto successor : blocks[index].successors) {
            const auto it = index_by_start.find(successor);
            if (it != index_by_start.end()) {
                graph[index].push_back(it->second);
            }
        }
    }
    return graph;
}

std::vector<std::vector<std::size_t>> strongly_connected_components(
    const std::vector<std::vector<std::size_t>>& graph)
{
    std::vector<int> index(graph.size(), -1);
    std::vector<int> lowlink(graph.size(), 0);
    std::vector<std::size_t> stack;
    std::vector<bool> on_stack(graph.size(), false);
    std::vector<std::vector<std::size_t>> components;
    int next_index = 0;

    std::function<void(std::size_t)> connect = [&](std::size_t node) {
        index[node] = next_index;
        lowlink[node] = next_index;
        ++next_index;
        stack.push_back(node);
        on_stack[node] = true;

        for (const auto successor : graph[node]) {
            if (index[successor] < 0) {
                connect(successor);
                lowlink[node] = std::min(lowlink[node], lowlink[successor]);
            } else if (on_stack[successor]) {
                lowlink[node] = std::min(lowlink[node], index[successor]);
            }
        }

        if (lowlink[node] == index[node]) {
            std::vector<std::size_t> component;
            while (!stack.empty()) {
                const auto successor = stack.back();
                stack.pop_back();
                on_stack[successor] = false;
                component.push_back(successor);
                if (successor == node) {
                    break;
                }
            }
            components.push_back(std::move(component));
        }
    };

    for (std::size_t node = 0; node < graph.size(); ++node) {
        if (index[node] < 0) {
            connect(node);
        }
    }
    return components;
}

std::vector<std::size_t> topological_sort(const std::vector<std::vector<std::size_t>>& graph)
{
    std::vector<std::size_t> incoming(graph.size(), 0);
    for (const auto& successors : graph) {
        for (const auto successor : successors) {
            ++incoming[successor];
        }
    }

    std::vector<std::size_t> ready;
    for (std::size_t node = 0; node < incoming.size(); ++node) {
        if (incoming[node] == 0) {
            ready.push_back(node);
        }
    }

    std::vector<std::size_t> result;
    while (!ready.empty()) {
        const auto node = ready.back();
        ready.pop_back();
        result.push_back(node);
        for (const auto successor : graph[node]) {
            --incoming[successor];
            if (incoming[successor] == 0) {
                ready.push_back(successor);
            }
        }
    }
    return result;
}

std::vector<std::vector<std::size_t>> robust_topological_sort(
    const std::vector<std::vector<std::size_t>>& graph,
    const std::vector<std::vector<std::size_t>>& components)
{
    std::vector<std::size_t> node_component(graph.size(), 0);
    for (std::size_t component_index = 0; component_index < components.size(); ++component_index) {
        for (const auto node : components[component_index]) {
            node_component[node] = component_index;
        }
    }

    std::vector<std::vector<std::size_t>> component_graph(components.size());
    for (std::size_t node = 0; node < graph.size(); ++node) {
        const auto source_component = node_component[node];
        for (const auto successor : graph[node]) {
            const auto target_component = node_component[successor];
            if (source_component != target_component) {
                component_graph[source_component].push_back(target_component);
            }
        }
    }

    std::vector<std::vector<std::size_t>> sorted_components;
    for (const auto component_index : topological_sort(component_graph)) {
        sorted_components.push_back(components[component_index]);
    }
    return sorted_components;
}

struct TopologyFeatures
{
    std::vector<std::vector<std::size_t>> graph;
    std::vector<std::vector<std::size_t>> components;
    std::vector<std::vector<std::size_t>> topological_components;
    std::uint64_t loops = 0;
    std::string strongly_connected_spp = "1";
};

TopologyFeatures topology_features_for_blocks(const std::vector<soff::BasicBlock>& blocks)
{
    TopologyFeatures features;
    features.graph = adjacency_for_blocks(blocks);
    features.components = strongly_connected_components(features.graph);
    features.topological_components = robust_topological_sort(features.graph, features.components);

    auto spp = std::string("1");
    for (const auto& component : features.components) {
        if (component.size() > 1) {
            ++features.loops;
            if (const auto prime = prime_at(component.size()); prime != 0) {
                spp = decimal_multiply(std::move(spp), prime);
            }
            continue;
        }
        const auto node = component.front();
        if (std::find(features.graph[node].begin(), features.graph[node].end(), node) != features.graph[node].end()) {
            ++features.loops;
        }
    }
    features.strongly_connected_spp = std::move(spp);
    return features;
}

std::string md_index_for_blocks(
    const std::vector<soff::BasicBlock>& blocks,
    const TopologyFeatures& topology)
{
    if (blocks.empty()) {
        return "0";
    }

    std::map<soff::Address, std::uint64_t> indegree_by_start;
    std::map<soff::Address, std::uint64_t> outdegree_by_start;
    for (std::size_t index = 0; index < blocks.size(); ++index) {
        outdegree_by_start[blocks[index].start] = blocks[index].successors.size();
    }
    for (const auto& block : blocks) {
        for (const auto successor : block.successors) {
            ++indegree_by_start[successor];
        }
    }

    std::vector<std::size_t> topological_order(blocks.size(), 0);
    for (std::size_t order = 0; order < topology.topological_components.size(); ++order) {
        for (const auto node : topology.topological_components[order]) {
            if (node < topological_order.size()) {
                topological_order[node] = order;
            }
        }
    }

    long double md_index = 0.0L;
    const long double rt2 = std::sqrt(2.0L);
    const long double rt3 = std::sqrt(3.0L);
    const long double rt5 = std::sqrt(5.0L);
    const long double rt7 = std::sqrt(7.0L);
    for (std::size_t block_index = 0; block_index < blocks.size(); ++block_index) {
        const auto& block = blocks[block_index];
        const auto source_index = static_cast<long double>(topological_order[block_index]);
        const auto source_in = static_cast<long double>(indegree_by_start[block.start]);
        const auto source_out = static_cast<long double>(outdegree_by_start[block.start]);
        for (const auto successor : block.successors) {
            const auto dest_in = static_cast<long double>(indegree_by_start[successor]);
            const auto dest_out = static_cast<long double>(outdegree_by_start[successor]);
            const auto embedded = source_index + source_in * rt2 + source_out * rt3 + dest_in * rt5 + dest_out * rt7;
            if (embedded > 0.0L) {
                md_index += 1.0L / std::sqrt(embedded);
            }
        }
    }

    if (md_index <= 0.0L) {
        return "0";
    }

    std::ostringstream out;
    out << std::setprecision(21) << md_index;
    return out.str();
}

std::string kgh_graph_hash(
    const std::vector<soff::BasicBlock>& blocks,
    const TopologyFeatures& topology,
    std::uint64_t function_flags)
{
    constexpr std::uint64_t node_entry = 2;
    constexpr std::uint64_t node_exit = 3;
    constexpr std::uint64_t node_normal = 5;
    constexpr std::uint64_t edge_in_conditional = 7;
    constexpr std::uint64_t edge_out_conditional = 11;
    constexpr std::uint64_t feature_loop = 19;
    constexpr std::uint64_t feature_strongly_connected = 37;
    constexpr std::uint64_t feature_func_no_ret = 41;
    constexpr std::uint64_t feature_func_lib = 43;
    constexpr std::uint64_t feature_func_thunk = 47;

    std::map<soff::Address, std::size_t> predecessors;
    for (const auto& block : blocks) {
        predecessors.try_emplace(block.start, 0);
    }
    for (const auto& block : blocks) {
        for (const auto successor : block.successors) {
            ++predecessors[successor];
        }
    }

    std::string hash = "1";
    for (const auto& block : blocks) {
        const auto predecessor_count = predecessors[block.start];
        if (predecessor_count == 0) {
            hash = decimal_multiply(std::move(hash), node_entry);
        }
        if (block.successors.empty()) {
            hash = decimal_multiply(std::move(hash), node_exit);
        }
        hash = decimal_multiply(std::move(hash), node_normal);
        for (std::size_t i = 0; i < block.successors.size(); ++i) {
            hash = decimal_multiply(std::move(hash), edge_out_conditional);
        }
        for (std::size_t i = 0; i < predecessor_count; ++i) {
            hash = decimal_multiply(std::move(hash), edge_in_conditional);
        }
    }

    for (std::uint64_t i = 0; i < topology.loops; ++i) {
        hash = decimal_multiply(std::move(hash), feature_loop);
    }
    for (std::size_t i = 0; i < topology.components.size(); ++i) {
        hash = decimal_multiply(std::move(hash), feature_strongly_connected);
    }

    if ((function_flags & FUNC_NORET) != 0) {
        hash = decimal_multiply(std::move(hash), feature_func_no_ret);
    }
    if ((function_flags & FUNC_LIB) != 0) {
        hash = decimal_multiply(std::move(hash), feature_func_lib);
    }
    if ((function_flags & FUNC_THUNK) != 0) {
        hash = decimal_multiply(std::move(hash), feature_func_thunk);
    }
    return hash;
}

bool has_data_ref_from(ea_t ea)
{
    xrefblk_t xref;
    return xref.first_from(ea, XREF_DATA);
}

soff::FunctionFeature read_function_feature(func_t* function, ea_t imagebase, HexRaysExportContext* hexrays, bool use_microcode = false, bool skip_trivial_decompile = false)
{
    soff::FunctionFeature feature;
    const asize_t function_size = std::max<asize_t>(function_reserve_min_size, function->size());
    const auto capped_function_size = static_cast<std::size_t>(std::min<asize_t>(function_size, function_reserve_max_size));
    const auto estimated_instructions = std::max<std::size_t>(1, capped_function_size / instruction_reserve_divisor);
    feature.instruction_details.reserve(estimated_instructions);
    feature.call_references.reserve(16);
    feature.constants.reserve(16);
    feature.blocks.reserve(capped_function_size);
    feature.assembly.reserve(estimated_instructions * assembly_reserve_multiplier);
    feature.stripped_assembly.reserve(estimated_instructions * stripped_assembly_reserve_multiplier);
    feature.mnemonics.reserve(estimated_instructions * mnemonic_reserve_multiplier);
    feature.address = static_cast<soff::Address>(function->start_ea);
    feature.rva = function->start_ea >= imagebase
        ? static_cast<soff::Address>(function->start_ea - imagebase)
        : static_cast<soff::Address>(function->start_ea);
    feature.segment_rva = feature.rva;
    feature.size = static_cast<std::uint64_t>(function->end_ea - function->start_ea);
    feature.function_flags = function->flags;

    qstring name;
    get_func_name(&name, function->start_ea);
    feature.name = to_string(name);
    if (feature.name.empty()) {
        std::ostringstream fallback;
        fallback << "sub_" << std::hex << feature.address;
        feature.name = fallback.str();
    }
    feature.names = feature.name;
    feature.mangled_function = feature.name;
    feature.prototype2 = attached_type_declaration(function->start_ea);
    feature.prototype = guessed_type_declaration(function->start_ea);
    if (feature.prototype.empty()) {
        feature.prototype = feature.prototype2;
    }
    if (feature.prototype2.empty()) {
        feature.prototype2 = type_declaration(function->start_ea);
    }

    qstring comment;
    if (get_func_cmt(&comment, function, false) > 0) {
        feature.comment = to_string(comment);
    }

    qflow_chart_t flow_chart("", function, function->start_ea, function->end_ea, 0);
    feature.node_count = static_cast<std::uint64_t>(flow_chart.size());
    for (auto block_index = 0; block_index < flow_chart.size(); ++block_index) {
        soff::BasicBlock block;
        block.start = static_cast<soff::Address>(flow_chart.blocks[block_index].start_ea);
        block.end = static_cast<soff::Address>(flow_chart.blocks[block_index].end_ea);
        for (auto successor_index = 0; successor_index < flow_chart.nsucc(block_index); ++successor_index) {
            const auto successor = flow_chart.succ(block_index, successor_index);
            block.successors.push_back(
                static_cast<soff::Address>(flow_chart.blocks[successor].start_ea));
        }
        feature.blocks.push_back(std::move(block));
        feature.edge_count += static_cast<std::uint64_t>(flow_chart.nsucc(block_index));
    }
    const auto topology = topology_features_for_blocks(feature.blocks);
    feature.strongly_connected = topology.components.size();
    feature.loops = topology.loops;
    feature.strongly_connected_spp = topology.strongly_connected_spp;
    feature.tarjan_topological_sort = json_component_array(topology.topological_components);
    auto kgh_hash = kgh_graph_hash(feature.blocks, topology, feature.function_flags);
    if (feature.node_count > 0) {
        const auto cyclomatic = static_cast<std::int64_t>(feature.edge_count)
            - static_cast<std::int64_t>(feature.node_count)
            + 2;
        feature.cyclomatic_complexity = static_cast<std::uint64_t>(std::max<std::int64_t>(0, cyclomatic));
        if (const auto prime = prime_at(static_cast<std::size_t>(feature.cyclomatic_complexity)); prime != 0) {
            feature.primes_value = std::to_string(prime);
        }
    }

    std::vector<std::uint8_t> bytes_hash_input;
    std::vector<std::uint8_t> function_hash_input;
    std::vector<soff::Address> assembly_addresses;
    std::set<std::string> referenced_names;
    std::vector<std::pair<std::size_t, std::vector<sval_t>>> switches;
    auto mnemonics_spp = std::string("1");
    const bool perf_enabled = soff::perf::enabled();
    std::uint64_t switch_info_attempts = 0;
    std::uint64_t string_xref_attempts = 0;

    func_item_iterator_t iterator(function);
    for (bool ok = iterator.first(); ok; ok = iterator.next_code()) {
        const ea_t ea = iterator.current();
        insn_t instruction;
        const int decoded_size = decode_insn(&instruction, ea);
        if (decoded_size <= 0) {
            continue;
        }

        qstring mnemonic;
        if (print_insn_mnem(&mnemonic, ea)) {
            const std::string mnemonic_text = to_string(mnemonic);
            const std::string disassembly = disassembly_line(ea);
            append_mnemonic(feature.mnemonics, mnemonic_text);
            append_line(feature.assembly, disassembly);
            append_line(feature.stripped_assembly, clean_assembly_line(disassembly));
            mnemonics_spp = decimal_multiply(std::move(mnemonics_spp), mnemonic_prime(mnemonic_text));

            soff::InstructionFeature instruction_feature;
            instruction_feature.address = static_cast<soff::Address>(ea);
            instruction_feature.disassembly = disassembly;
            instruction_feature.mnemonic = mnemonic_text;
            instruction_feature.operand_names = forced_operands_json(ea, instruction);
            feature.instruction_details.push_back(std::move(instruction_feature));
        }
        if (is_call_insn(instruction)) {
            kgh_hash = decimal_multiply(std::move(kgh_hash), 23);
        }
        if (has_data_ref_from(ea)) {
            kgh_hash = decimal_multiply(std::move(kgh_hash), 29);
        }
        collect_operand_name(instruction, referenced_names);
        if (perf_enabled) {
            ++switch_info_attempts;
        }
        collect_switch_info(ea, switches);
        append_instruction_to_block(feature.blocks, ea);
        assembly_addresses.push_back(static_cast<soff::Address>(ea));

        for (const auto& operand : instruction.ops) {
            if (operand.type == o_void) {
                break;
            }
            if (operand.type == o_imm) {
                const auto val = static_cast<std::uint64_t>(operand.value);
                if (is_valid_constant(val, ea)) {
                    append_unique(feature.constants, std::to_string(val));
                }
            }
            if (operand.type == o_displ) {
                const auto val = static_cast<std::uint64_t>(operand.addr);
                if (is_valid_constant(val, ea)) {
                    append_unique(feature.constants, std::to_string(val));
                }
            }
        }

        // Extract string constants from data references
        xrefblk_t data_xref;
        if (perf_enabled) {
            ++string_xref_attempts;
        }
        for (bool has = data_xref.first_from(ea, XREF_DATA);
             has;
             has = data_xref.next_from()) {
            if (data_xref.iscode) continue;
            const auto str_type = get_str_type(data_xref.to);
            if (str_type >= 0) {
                qstring str_content;
                if (get_strlit_contents(&str_content, data_xref.to, -1, str_type) > 0
                    && str_content.length() > 1 && str_content.length() < 256) {
                    append_unique(feature.constants, std::string(str_content.c_str()));
                }
            }
        }

        xrefblk_t xref;
        for (bool has_xref = xref.first_from(ea, XREF_CODE | XREF_NOFLOW);
             has_xref;
             has_xref = xref.next_from()) {
            if (!xref.iscode) {
                continue;
            }
            const func_t* referenced_function = get_func(xref.to);
            if (referenced_function == nullptr || referenced_function->start_ea != function->start_ea) {
                kgh_hash = decimal_multiply(std::move(kgh_hash), 31);
            }
            if (xref.type != fl_CN && xref.type != fl_CF) {
                continue;
            }
            feature.call_references.push_back({
                static_cast<soff::Address>(xref.to),
                xref.type == fl_CF ? "call far" : "call near",
            });
        }

        ++feature.instruction_count;
        const auto comparable_size = comparable_instruction_size(instruction, decoded_size);
        append_bytes(bytes_hash_input, ea, comparable_size);
        const auto item_size = static_cast<int>(get_item_size(ea));
        append_bytes(function_hash_input, ea, item_size > 0 ? item_size : decoded_size);
        for (auto offset = 0; offset < comparable_size; ++offset) {
            const auto byte = static_cast<std::uint8_t>(get_byte(ea + offset));
            feature.bytes_sum += byte;
        }
    }

    if (switch_info_attempts != 0) {
        soff::perf::add_counter("export.switch_info.attempt", switch_info_attempts);
    }
    if (string_xref_attempts != 0) {
        soff::perf::add_counter("export.string_xref.attempt", string_xref_attempts);
    }

    feature.indegree = count_incoming_code_refs(function->start_ea);
    feature.outdegree = feature.call_references.size();
    feature.names = json_string_array(referenced_names);
    feature.assembly_addrs = json_address_array(assembly_addresses);
    feature.switches = json_switches(switches);
    feature.mnemonics_spp = std::move(mnemonics_spp);
    feature.kgh_hash = std::move(kgh_hash);
    feature.md_index = md_index_for_blocks(feature.blocks, topology);
    feature.bytes_hash = md5_hex(bytes_hash_input);
    feature.function_hash = md5_hex(function_hash_input);

    // Source file from DWARF debug info
    {
        const char* src = get_sourcefile(function->start_ea);
        if (src != nullptr && src[0] != '\0') {
            feature.source_file = src;
        }
    }

    if (hexrays != nullptr && hexrays->requested
        && !(skip_trivial_decompile && feature.node_count <= 1)) {
        extract_pseudocode_features(function, feature, *hexrays);
        if (use_microcode) {
            extract_microcode_features(function, feature, *hexrays);
        }
    }
    return feature;
}

ExportResult build_ida_snapshot(
    const ExportOptions& options,
    const std::filesystem::path* crash_path = nullptr,
    const std::filesystem::path* output_path = nullptr)
{
    soff::perf::ScopedTimer timer("export.total");
    ExportResult result;
    auto& snapshot = result.snapshot;
    snapshot.architecture = to_string(inf_get_procname());

    char input_path[QMAXPATH] = {};
    if (get_input_file_path(input_path, sizeof(input_path)) > 0) {
        snapshot.input_path = input_path;
    }

    const auto imagebase = get_imagebase();
    const auto function_count = get_func_qty();
    result.stats.total_functions = function_count;
    snapshot.functions.reserve(function_count);
    const auto export_started = std::chrono::steady_clock::now();
    const bool incremental_save = output_path != nullptr;
    soff::SnapshotRepository repository;
    std::unique_ptr<soff::db::AtomicFileWriter> atomic_output;
    std::filesystem::path incremental_output_path;
    std::vector<soff::FunctionFeature> pending_functions;
    constexpr std::size_t export_batch_size = 512;
    if (incremental_save) {
        atomic_output = std::make_unique<soff::db::AtomicFileWriter>(
            *output_path, options.resume_existing_database);
        incremental_output_path = atomic_output->temporary_path();
        repository.begin_incremental_save(snapshot, incremental_output_path, false);
        pending_functions.reserve(export_batch_size);
    }
    auto existing_addresses = options.resume_existing_database && incremental_save
        ? load_exported_function_addresses(incremental_output_path)
        : std::unordered_set<soff::Address>{};
    if (!existing_addresses.empty()) {
        msg(
            "Soff: resume enabled; %zu function address(es) already exist in %s\n",
            existing_addresses.size(),
            output_path->string().c_str());
    }

    // Background writer thread for async SQLite commits
    struct AsyncWriter {
        soff::SnapshotRepository& repository;
        const std::filesystem::path& output_path;
        std::size_t reserve_size;
        std::mutex mutex;
        std::condition_variable cv;
        std::queue<std::vector<soff::FunctionFeature>> queue;
        bool done = false;
        std::size_t batch_commits = 0;
        std::exception_ptr error;

        AsyncWriter(soff::SnapshotRepository& repo, const std::filesystem::path& path, std::size_t batch_size)
            : repository(repo), output_path(path), reserve_size(batch_size) {}

        void run() {
            while (true) {
                std::vector<soff::FunctionFeature> batch;
                {
                    std::unique_lock lock(mutex);
                    cv.wait(lock, [&] { return !queue.empty() || done; });
                    if (queue.empty() && done) break;
                    batch = std::move(queue.front());
                    queue.pop();
                }
                try {
                    repository.append_functions(batch, output_path);
                    ++batch_commits;
                } catch (...) {
                    std::lock_guard lock(mutex);
                    error = std::current_exception();
                    break;
                }
            }
        }

        void submit(std::vector<soff::FunctionFeature>& pending) {
            if (pending.empty()) return;
            {
                std::lock_guard lock(mutex);
                queue.push(std::move(pending));
            }
            cv.notify_one();
            pending.clear();
            pending.reserve(reserve_size);
        }

        void finish() {
            {
                std::lock_guard lock(mutex);
                done = true;
            }
            cv.notify_one();
        }

        void check_error() {
            std::lock_guard lock(mutex);
            if (error) std::rethrow_exception(error);
        }
    };

    std::unique_ptr<AsyncWriter> writer;
    std::thread writer_thread;
    if (incremental_save) {
        writer = std::make_unique<AsyncWriter>(repository, incremental_output_path, export_batch_size);
        writer_thread = std::thread([&writer]() { writer->run(); });
    }

    const auto flush_pending_functions = [&]() {
        if (!incremental_save || pending_functions.empty()) {
            return;
        }
        if (writer) {
            writer->check_error();
            writer->submit(pending_functions);
        } else {
            repository.append_functions(pending_functions, incremental_output_path);
            pending_functions.clear();
        }
        ++result.stats.batch_commits;
    };

    HexRaysExportContext hexrays;
    hexrays.requested = options.use_decompiler;
    if (hexrays.requested) {
        hexrays.available = initialize_hexrays();
        msg(
            "Soff: Hex-Rays decompiler %s\n",
            hexrays.available ? "available; pseudocode export enabled" : "not available; pseudocode fields will be empty");
    }

    show_wait_box("Soff: exporting function features...");
    ea_t current_function_ea = BADADDR;
    std::string current_function_name;
    bool cancellation_requested = false;
    try {
        update_crash_marker(
            crash_path,
            0,
            function_count,
            result.stats.exported_functions,
            result.stats.skipped_functions,
            BADADDR,
            "",
            "start");
        for (std::size_t i = 0; i < function_count; ++i) {
            if (user_cancelled()) {
                cancellation_requested = true;
                update_crash_marker(
                    crash_path,
                    i,
                    function_count,
                    result.stats.exported_functions,
                    result.stats.skipped_functions,
                    current_function_ea,
                    current_function_name,
                    "cancelled");
                throw soff::Error(soff::ErrorCode::export_failed, "export cancelled by user");
            }
            if ((i % 8) == 0) {
                replace_wait_box(
                    "Soff: exporting function %zu/%zu (exported=%zu skipped=%zu pseudo=%zu micro=%zu)...",
                    i,
                    function_count,
                    result.stats.exported_functions,
                    result.stats.skipped_functions,
                    hexrays.pseudocode_functions,
                    hexrays.microcode_functions);
            }
            if (hexrays.available && i != 0 && (i % 1024) == 0) {
                clear_cached_cfuncs();
                ++hexrays.cache_clears;
            }

            func_t* function = getn_func(i);
            if (function == nullptr) {
                ++result.stats.skipped_functions;
                ++result.stats.skip_reasons["invalid-function"];
                continue;
            }
            qstring name;
            get_func_name(&name, function->start_ea);
            std::string function_name = to_string(name);
            if (function_name.empty()) {
                std::ostringstream fallback;
                fallback << "sub_" << std::hex << static_cast<soff::Address>(function->start_ea);
                function_name = fallback.str();
            }
            result.stats.last_function_index = i;
            result.stats.last_function_address = static_cast<soff::Address>(function->start_ea);
            result.stats.last_function_name = function_name;
            current_function_ea = function->start_ea;
            current_function_name = function_name;
            const auto skip_reason = skip_reason_for(function, function_name, options);
            if (!skip_reason.empty()) {
                ++result.stats.skipped_functions;
                ++result.stats.skip_reasons[skip_reason];
                continue;
            }
            if (existing_addresses.find(static_cast<soff::Address>(function->start_ea)) != existing_addresses.end()) {
                ++result.stats.exported_functions;
                ++result.stats.resumed_functions;
                continue;
            }
            // Export hook: before_export_function
            if (options.hooks != nullptr) {
                if (!options.hooks->before_export_function(
                        static_cast<soff::Address>(function->start_ea), function_name)) {
                    ++result.stats.skipped_functions;
                    ++result.stats.skip_reasons["hook-rejected"];
                    continue;
                }
            }
            soff::FunctionFeature feature;
            {
                soff::perf::ScopedTimer timer("export.read_function");
                feature = read_function_feature(function, imagebase, &hexrays, options.use_microcode, options.ignore_small_functions);
            }
            // Export hook: after_export_function
            if (options.hooks != nullptr) {
                options.hooks->after_export_function(feature);
            }
            if (incremental_save) {
                pending_functions.push_back(std::move(feature));
                if (pending_functions.size() >= export_batch_size) {
                    flush_pending_functions();
                }
            } else {
                snapshot.functions.push_back(std::move(feature));
            }
            ++result.stats.exported_functions;
        }
        flush_pending_functions();
        // Wait for background writer to finish
        if (writer) {
            writer->finish();
            writer_thread.join();
            writer->check_error();
            result.stats.batch_commits = writer->batch_commits;
        }
        update_crash_marker(
            crash_path, function_count - 1, function_count,
            result.stats.exported_functions, result.stats.skipped_functions,
            current_function_ea, current_function_name, "export-complete");
        hide_wait_box();
    } catch (...) {
        if (writer) {
            writer->finish();
            if (writer_thread.joinable()) writer_thread.join();
        }
        update_crash_marker(
            crash_path,
            result.stats.last_function_index,
            function_count,
            result.stats.exported_functions,
            result.stats.skipped_functions,
            current_function_ea,
            current_function_name,
            cancellation_requested ? "cancelled" : "failed");
        hide_wait_box();
        if (options.hooks != nullptr) {
            options.hooks->on_export_crash(
                static_cast<soff::Address>(current_function_ea), current_function_name);
        }
        throw;
    }
    result.stats.export_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - export_started).count();

    append_program_data(snapshot, "export.total_functions", "integer", std::to_string(result.stats.total_functions));
    append_program_data(snapshot, "export.exported_functions", "integer", std::to_string(result.stats.exported_functions));
    append_program_data(snapshot, "export.resumed_functions", "integer", std::to_string(result.stats.resumed_functions));
    append_program_data(snapshot, "export.skipped_functions", "integer", std::to_string(result.stats.skipped_functions));
    append_program_data(snapshot, "export.crash_file_preexisting", "boolean", options.crash_file_preexisting ? "true" : "false");
    append_program_data(snapshot, "export.crash_resume_supported", "boolean", incremental_save ? "true" : "false");
    append_program_data(snapshot, "export.crash_resume_used", "boolean", options.resume_existing_database ? "true" : "false");
    append_program_data(snapshot, "export.batch_commits", "integer", std::to_string(result.stats.batch_commits));
    append_program_data(snapshot, "export.previous_crash_marker", "text", options.previous_crash_marker);
    append_program_data(snapshot, "export.last_function_index", "integer", std::to_string(result.stats.last_function_index));
    append_program_data(snapshot, "export.last_function_address", "address", std::to_string(result.stats.last_function_address));
    append_program_data(snapshot, "export.last_function_name", "text", result.stats.last_function_name);
    append_program_data(snapshot, "export.decompiler", "boolean", options.use_decompiler ? "true" : "false");
    append_program_data(snapshot, "export.hexrays_available", "boolean", hexrays.available ? "true" : "false");
    append_program_data(snapshot, "export.pseudocode_functions", "integer", std::to_string(hexrays.pseudocode_functions));
    append_program_data(snapshot, "export.pseudocode_failures", "integer", std::to_string(hexrays.pseudocode_failures));
    append_program_data(snapshot, "export.pseudocode_comments", "integer", std::to_string(hexrays.pseudocode_comments));
    append_program_data(snapshot, "export.microcode_functions", "integer", std::to_string(hexrays.microcode_functions));
    append_program_data(snapshot, "export.microcode_failures", "integer", std::to_string(hexrays.microcode_failures));
    append_program_data(snapshot, "export.hexrays_cache_clears", "integer", std::to_string(hexrays.cache_clears));
    append_program_data(snapshot, "export.exclude_library_thunk", "boolean", options.exclude_library_thunk ? "true" : "false");
    append_program_data(snapshot, "export.ignore_small_functions", "boolean", options.ignore_small_functions ? "true" : "false");
    append_program_data(snapshot, "export.min_ea", "address", std::to_string(static_cast<soff::Address>(options.min_ea)));
    append_program_data(snapshot, "export.max_ea", "address", std::to_string(static_cast<soff::Address>(options.max_ea)));
    append_program_data(snapshot, "export.elapsed_seconds", "real", std::to_string(result.stats.export_seconds));
    const auto functions_per_second = result.stats.export_seconds > 0.0
        ? static_cast<double>(result.stats.exported_functions) / result.stats.export_seconds
        : 0.0;
    append_program_data(snapshot, "export.functions_per_second", "real", std::to_string(functions_per_second));
    for (const auto& [reason, count] : result.stats.skip_reasons) {
        append_program_data(
            snapshot,
            "export.skip_reason." + reason,
            "integer",
            std::to_string(count));
    }
    for (const auto& [failure, count] : hexrays.failure_codes) {
        append_program_data(
            snapshot,
            "export.hexrays_failure." + failure,
            "integer",
            std::to_string(count));
    }
    export_local_types(snapshot);

    // Compute callgraph primes after all functions are collected
    {
        boost::unordered_flat_set<soff::Address> callgraph_participants;
        for (const auto& f : snapshot.functions) {
            for (const auto& ref : f.call_references) {
                callgraph_participants.insert(f.address);
                callgraph_participants.insert(ref.address);
            }
        }
        std::string cg_primes = "1";
        std::string cg_all_primes = "1";
        for (const auto& f : snapshot.functions) {
            if (f.primes_value.empty() || f.primes_value == "0") continue;
            const auto prime_val = std::stoull(f.primes_value);
            if (prime_val <= 1) continue;
            cg_all_primes = decimal_multiply(std::move(cg_all_primes), prime_val);
            if (callgraph_participants.contains(f.address)) {
                cg_primes = decimal_multiply(std::move(cg_primes), prime_val);
            }
        }
        snapshot.callgraph_primes = std::move(cg_primes);
        snapshot.callgraph_all_primes = std::move(cg_all_primes);
    }

    if (incremental_save) {
        repository.replace_program_data(snapshot.program_data, incremental_output_path);
        repository.update_callgraph_primes(snapshot.callgraph_primes, snapshot.callgraph_all_primes, incremental_output_path);
        repository.save_compilation_units(incremental_output_path);
        repository.finalize_incremental_save(incremental_output_path);
        soff::db::checkpoint_and_validate_sqlite(incremental_output_path);
        atomic_output->commit();
        if (result.stats.resumed_functions != 0) {
            result.snapshot = repository.load(*output_path);
        }
    }

    return result;
}

std::string export_summary_text(const ExportResult& result, const std::filesystem::path& output_path)
{
    const auto field_count = result.snapshot.functions.size();
    std::size_t names_nonempty = 0;
    std::size_t prototypes = 0;
    std::size_t md_index_nonzero = 0;
    std::size_t switches_nonempty = 0;
    std::size_t strongly_connected_nonzero = 0;
    std::size_t loops_nonzero = 0;
    std::size_t pseudocode_nonempty = 0;
    std::size_t clean_pseudo_nonempty = 0;
    std::size_t pseudocode_primes_nonempty = 0;
    std::size_t microcode_nonempty = 0;
    std::size_t clean_microcode_nonempty = 0;
    std::size_t microcode_spp_nontrivial = 0;
    for (const auto& function : result.snapshot.functions) {
        if (!function.names.empty() && function.names != "[]") {
            ++names_nonempty;
        }
        if (!function.prototype.empty()) {
            ++prototypes;
        }
        if (!function.md_index.empty() && function.md_index != "0") {
            ++md_index_nonzero;
        }
        if (!function.switches.empty() && function.switches != "[]") {
            ++switches_nonempty;
        }
        if (function.strongly_connected > 0) {
            ++strongly_connected_nonzero;
        }
        if (function.loops > 0) {
            ++loops_nonzero;
        }
        if (!function.pseudocode.empty()) {
            ++pseudocode_nonempty;
        }
        if (!function.stripped_pseudocode.empty()) {
            ++clean_pseudo_nonempty;
        }
        if (!function.pseudocode_primes.empty()) {
            ++pseudocode_primes_nonempty;
        }
        if (!function.microcode.empty()) {
            ++microcode_nonempty;
        }
        if (!function.stripped_microcode.empty()) {
            ++clean_microcode_nonempty;
        }
        if (!function.microcode_spp.empty() && function.microcode_spp != "1") {
            ++microcode_spp_nontrivial;
        }
    }

    std::ostringstream out;
    out << "Output: " << output_path.string() << '\n'
        << "Functions: " << result.stats.exported_functions << "/" << result.stats.total_functions
        << " exported, skipped=" << result.stats.skipped_functions
        << ", resumed=" << result.stats.resumed_functions
        << ", batches=" << result.stats.batch_commits
        << ", seconds=" << std::fixed << std::setprecision(3) << result.stats.export_seconds << '\n'
        << "Field fill: names=" << names_nonempty << "/" << field_count
        << " prototype=" << prototypes << "/" << field_count
        << " md_index=" << md_index_nonzero << "/" << field_count
        << " switches=" << switches_nonempty << "/" << field_count
        << " scc=" << strongly_connected_nonzero << "/" << field_count
        << " loops=" << loops_nonzero << "/" << field_count
        << " pseudo=" << pseudocode_nonempty << "/" << field_count
        << " clean_pseudo=" << clean_pseudo_nonempty << "/" << field_count
        << " pseudocode_primes=" << pseudocode_primes_nonempty << "/" << field_count
        << " microcode=" << microcode_nonempty << "/" << field_count
        << " clean_microcode=" << clean_microcode_nonempty << "/" << field_count
        << " microcode_spp=" << microcode_spp_nontrivial << "/" << field_count << '\n';
    if (!result.stats.skip_reasons.empty()) {
        out << "Skip reasons:";
        for (const auto& [reason, count] : result.stats.skip_reasons) {
            out << " " << reason << "=" << count;
        }
        out << '\n';
    }
    out << "Crash marker: cleared after successful export";
    return out.str();
}

ExportResult export_current_idb(const std::filesystem::path& output_path, ExportOptions options)
{
    const std::filesystem::path crash_path = output_path.string() + "-crash";
    options.crash_file_preexisting = std::filesystem::exists(crash_path);
    if (options.crash_file_preexisting) {
        msg("Soff: previous export crash marker exists: %s\n", crash_path.string().c_str());
        options.previous_crash_marker = read_text_file(crash_path);
        const int choice = ask_buttons(
            "~D~elete marker and export",
            "~R~esume existing DB",
            "~C~ancel",
            ASKBTN_YES,
            "Soff found a previous crash marker:\n%s\n\n%s",
            crash_path.string().c_str(),
            options.previous_crash_marker.empty() ? "(marker is unreadable or empty)" : options.previous_crash_marker.c_str());
        if (choice == ASKBTN_CANCEL) {
            throw soff::Error(soff::ErrorCode::export_failed, "export cancelled because a previous crash marker exists");
        }
        if (choice == ASKBTN_YES) {
            std::error_code marker_remove_error;
            std::filesystem::remove(crash_path, marker_remove_error);
            options.crash_file_preexisting = false;
            options.previous_crash_marker.clear();
        }
        if (choice == ASKBTN_NO) {
            options.resume_existing_database = true;
        }
    }
    write_crash_marker(crash_path, "soff export in progress\nphase=start\n");

    const auto result = build_ida_snapshot(options, &crash_path, &output_path);
    std::error_code remove_error;
    std::filesystem::remove(crash_path, remove_error);
    msg(
        "Soff: exported %zu/%zu functions to %s, skipped=%zu resumed=%zu batches=%zu\n",
        result.stats.exported_functions,
        result.stats.total_functions,
        output_path.string().c_str(),
        result.stats.skipped_functions,
        result.stats.resumed_functions,
        result.stats.batch_commits);
    for (const auto& [reason, count] : result.stats.skip_reasons) {
        msg("Soff: skipped %zu function(s), reason=%s\n", count, reason.c_str());
    }
    return result;
}

soff::diff::DiffSessionSummary diff_databases(const DiffUiOptions& options)
{
    show_wait_box("Soff: loading databases...");

    if (const auto error = validate_export_database(options.main_db, "Primary"); !error.empty()) {
        hide_wait_box();
        throw soff::Error(soff::ErrorCode::diff_failed, error);
    }
    if (user_cancelled()) { hide_wait_box(); throw soff::Error(soff::ErrorCode::diff_failed, "cancelled"); }
    replace_wait_box("Soff: validating secondary database...");
    if (const auto error = validate_export_database(options.diff_db, "Secondary"); !error.empty()) {
        hide_wait_box();
        throw soff::Error(soff::ErrorCode::diff_failed, error);
    }
    if (user_cancelled()) { hide_wait_box(); throw soff::Error(soff::ErrorCode::diff_failed, "cancelled"); }
    replace_wait_box("Soff: preparing diff session...");

    struct DiffProgressHooks : soff::DiffHooks
    {
        std::size_t heuristic_index = 0;
        std::size_t total_heuristics = 0;
        std::size_t total_matches = 0;
        bool cancelled = false;

        std::optional<std::string> on_launch_heuristic(
            std::string_view name,
            std::string_view sql) override
        {
            if (user_cancelled()) {
                cancelled = true;
                return std::nullopt;
            }
            ++heuristic_index;
            replace_wait_box(
                "Soff: diffing heuristic %zu/%zu  matches=%zu\n%.*s",
                heuristic_index,
                total_heuristics,
                total_matches,
                static_cast<int>(std::min(name.size(), std::size_t(60))),
                name.data());
            return std::string(sql);
        }

        soff::MatchDecision on_match(const soff::MatchContext& context) override
        {
            ++total_matches;
            return {true, context.ratio};
        }
    };

    DiffProgressHooks progress_hooks;
    {
        const auto& base = soff::diff::builtin_heuristics();
        progress_hooks.total_heuristics = base.size();
    }

    soff::diff::DiffSessionOptions diff_options;
    diff_options.sql.enable_slow = options.slow;
    diff_options.sql.enable_unreliable = options.unreliable;
    diff_options.sql.enable_experimental = options.experimental;
    diff_options.sql.max_processed_rows = options.max_rows;
    diff_options.sql.timeout_seconds = options.timeout_seconds;
    diff_options.hooks = &progress_hooks;

    replace_wait_box("Soff: running heuristics (loading function data)...");
    soff::diff::DiffSessionSummary summary;
    try {
        summary = soff::diff::DiffSession{diff_options}.run_all(
            options.main_db,
            options.diff_db,
            options.result_db);
    } catch (...) {
        hide_wait_box();
        throw;
    }
    hide_wait_box();

    if (progress_hooks.cancelled) {
        throw soff::Error(soff::ErrorCode::diff_failed, "diff cancelled by user");
    }

    msg(
        "Soff: diff complete out=%s heuristics=%zu best=%zu partial=%zu unreliable=%zu multimatch=%zu unmatched=%zu/%zu\n",
        options.result_db.c_str(),
        summary.heuristics,
        summary.results.best,
        summary.results.partial,
        summary.results.unreliable,
        summary.results.multimatch,
        summary.results.unmatched_primary,
        summary.results.unmatched_secondary);
    return summary;
}

std::string hex_address(soff::Address address)
{
    if (address == 0) {
        return "";
    }
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << address;
    return out.str();
}

