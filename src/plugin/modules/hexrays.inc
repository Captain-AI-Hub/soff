struct AstPrimeVisitor : public ctree_visitor_t
{
    AstPrimeVisitor()
        : ctree_visitor_t(CV_FAST)
    {
    }

    int idaapi visit_expr(cexpr_t* expr) override
    {
        if (expr != nullptr) {
            multiply_opcode(expr->op);
        }
        return 0;
    }

    int idaapi visit_insn(cinsn_t* insn) override
    {
        if (insn != nullptr) {
            multiply_opcode(insn->op);
        }
        return 0;
    }

    std::string primes_hash = "1";

private:
    void multiply_opcode(ctype_t op)
    {
        const auto prime = prime_at(static_cast<std::size_t>(op));
        if (prime != 0) {
            primes_hash = decimal_multiply(std::move(primes_hash), prime);
        }
    }
};

const std::unordered_map<std::string, std::size_t>& processor_instruction_indices()
{
    static const auto indices = [] {
        std::vector<std::string> names;
        for (int index = PH.instruc_start; index < PH.instruc_end; ++index) {
            const char* name = PH.instruc[index].name;
            if (name != nullptr && *name != '\0') {
                names.emplace_back(name);
            }
        }
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());

        std::unordered_map<std::string, std::size_t> result;
        for (std::size_t i = 0; i < names.size(); ++i) {
            result.emplace(names[i], i);
        }
        return result;
    }();
    return indices;
}

std::uint64_t mnemonic_prime(const std::string& mnemonic)
{
    const auto& indices = processor_instruction_indices();
    const auto it = indices.find(mnemonic);
    return it == indices.end() ? 1 : prime_at(it->second);
}

void append_program_data(
    soff::ProgramSnapshot& snapshot,
    std::string name,
    std::string type,
    std::string value)
{
    snapshot.program_data.push_back({std::move(name), std::move(type), std::move(value)});
}

struct string_text_sink_t : public text_sink_t
{
    std::string text;

    int idaapi print(const char* str) override
    {
        if (str != nullptr) {
            text += str;
        }
        return 0;
    }
};

bool local_type_starts_with(std::string_view text, std::string_view prefix)
{
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

std::string local_type_definition(uint32 ordinal)
{
    ordvec_t ordinals;
    ordinals.push_back(ordinal);
    string_text_sink_t sink;
    const int exported = print_decls(sink, nullptr, &ordinals, PDF_DEF_FWD | PDF_DEF_BASE);
    return exported == 0 ? std::string() : trim_copy(std::move(sink.text));
}

std::string local_type_kind(std::string_view definition)
{
    const auto text = trim_copy(std::string(definition));
    if (local_type_starts_with(text, "enum ")) {
        return "enum";
    }
    if (local_type_starts_with(text, "union ")) {
        return "union";
    }
    if (local_type_starts_with(text, "struct ")) {
        return "struct";
    }
    return "";
}

void export_local_types(soff::ProgramSnapshot& snapshot)
{
    const auto local_type_count = get_ordinal_count(nullptr);
    if (local_type_count == 0 || local_type_count == uint32(-1)) {
        return;
    }

    for (uint32 ordinal = 1; ordinal <= local_type_count; ++ordinal) {
        const char* name = get_numbered_type_name(nullptr, ordinal);
        if (name == nullptr || name[0] == '\0') {
            continue;
        }
        auto definition = local_type_definition(ordinal);
        if (definition.empty()) {
            continue;
        }
        auto kind = local_type_kind(definition);
        if (kind.empty()) {
            continue;
        }
        append_program_data(snapshot, name, std::move(kind), std::move(definition));
    }
}

std::string disassembly_line(ea_t ea)
{
    qstring line;
    if (!generate_disasm_line(&line, ea, GENDSM_REMOVE_TAGS)) {
        return "";
    }
    return to_string(line);
}

std::string regex_replace_icase(
    const std::string& text,
    const std::string& pattern,
    const std::string& replacement)
{
    return std::regex_replace(
        text,
        std::regex(pattern, std::regex_constants::icase),
        replacement);
}

std::string clean_assembly_line(std::string line)
{
    const auto semicolon = line.find(';');
    if (semicolon != std::string::npos) {
        line.erase(semicolon);
    }
    const auto hash_comment = line.find(" # ");
    if (hash_comment != std::string::npos) {
        line.erase(hash_comment);
    }

    constexpr const char* replacements[] = {
        "loc_", "j_nullsub_", "nullsub_", "j_sub_", "sub_",
        "qword_", "dword_", "byte_", "word_", "off_", "def_", "unk_", "asc_",
        "stru_", "dbl_", "locret_", "flt_", "jpt_",
    };
    for (const auto* replacement : replacements) {
        line = regex_replace_icase(line, std::string(replacement) + "[a-f0-9A-F]+", "XXXX");
    }

    constexpr const char* removals[] = {
        "dword ptr ", "byte ptr ", "word ptr ", "qword ptr ", "short ptr",
    };
    for (const auto* removal : removals) {
        line = regex_replace_icase(line, std::string(removal) + "[a-f0-9A-F]+", "");
    }

    line = regex_replace_icase(line, R"(\+[a-f0-9A-F]+h\+)", "+XXXX+");
    line = regex_replace_icase(line, R"(\.\.[a-f0-9A-F]{8})", "XXX");
    line = regex_replace_icase(line, R"([ \t\n]+$)", "");
    line = regex_replace_icase(line, R"(a[A-Z]+[a-z0-9]+_[0-9]+)", "aXXX");
    line = regex_replace_icase(line, R"(\#0x[A-Z0-9]+)", "0xXXX");
    return line;
}

std::string clean_pseudocode_text(std::string text)
{
    text = regex_replace_icase(text, R"( // .*)", "");
    constexpr const char* replacements[] = {
        "loc_", "j_nullsub_", "nullsub_", "j_sub_", "sub_",
        "qword_", "dword_", "byte_", "word_", "off_", "def_", "unk_", "asc_",
        "stru_", "dbl_", "locret_", "flt_", "jpt_",
    };
    for (const auto* replacement : replacements) {
        text = regex_replace_icase(text, std::string(replacement) + "[a-f0-9A-F]+", std::string(replacement) + "XXXX");
    }
    text = regex_replace_icase(text, R"(v[0-9]+)", "vXXX");
    text = regex_replace_icase(text, R"(a[0-9]+)", "aXXX");
    text = regex_replace_icase(text, R"(arg_[0-9]+)", "aXXX");
    return text;
}

std::string print_tinfo_declaration(const tinfo_t& type)
{
    qstring declaration;
    if (!type.print(&declaration)) {
        return "";
    }
    return to_string(declaration);
}

std::string attached_type_declaration(ea_t ea)
{
    tinfo_t type;
    if (!get_tinfo(&type, ea)) {
        return "";
    }
    return print_tinfo_declaration(type);
}

std::string guessed_type_declaration(ea_t ea)
{
    tinfo_t type;
    if (guess_tinfo(&type, ea) == GUESS_FUNC_FAILED) {
        return "";
    }
    return print_tinfo_declaration(type);
}

std::string type_declaration(ea_t ea)
{
    qstring declaration;
    if (!print_type(&declaration, ea, PRTYPE_1LINE)) {
        return "";
    }
    return to_string(declaration);
}

bool initialize_hexrays()
{
    if (init_hexrays_plugin()) {
        return true;
    }

    std::string decompiler_plugin = "hexrays";
    if (const char* env_plugin = env_value("DIAPHORA_DECOMPILER_PLUGIN")) {
        decompiler_plugin = env_plugin;
    }
    if (load_plugin(decompiler_plugin.c_str()) == nullptr) {
        return false;
    }
    return init_hexrays_plugin();
}

std::string plain_pseudocode_line(const simpleline_t& source_line)
{
    qstring line;
    tag_remove(&line, source_line.line);
    return to_string(line);
}

std::string untag_text(const char* text)
{
    qstring line;
    tag_remove(&line, text != nullptr ? text : "");
    return to_string(line);
}

std::string hexrays_failure_key(const hexrays_failure_t& failure)
{
    std::ostringstream out;
    out << static_cast<int>(failure.code);
    if (failure.errea != BADADDR) {
        out << "@0x" << std::hex << static_cast<soff::Address>(failure.errea);
    }
    const auto description = trim_copy(to_string(failure.desc()));
    if (!description.empty()) {
        out << ":" << description;
    }
    return out.str();
}

void record_hexrays_failure(HexRaysExportContext& hexrays, const hexrays_failure_t& failure)
{
    ++hexrays.failure_codes[hexrays_failure_key(failure)];
}

std::string base64_without_padding(const std::vector<std::uint8_t>& bytes, std::size_t max_size)
{
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const std::uint32_t b0 = bytes[i];
        const std::uint32_t b1 = (i + 1 < bytes.size()) ? bytes[i + 1] : 0;
        const std::uint32_t b2 = (i + 2 < bytes.size()) ? bytes[i + 2] : 0;
        const std::uint32_t triple = (b0 << 16) | (b1 << 8) | b2;

        out.push_back(alphabet[(triple >> 18) & 0x3f]);
        out.push_back(alphabet[(triple >> 12) & 0x3f]);
        if (i + 1 < bytes.size()) {
            out.push_back(alphabet[(triple >> 6) & 0x3f]);
        }
        if (i + 2 < bytes.size()) {
            out.push_back(alphabet[triple & 0x3f]);
        }
        if (out.size() >= max_size) {
            out.resize(max_size);
            return out;
        }
    }
    if (out.size() > max_size) {
        out.resize(max_size);
    }
    return out;
}

std::uint8_t modsum(std::vector<std::uint8_t>::const_iterator begin, std::vector<std::uint8_t>::const_iterator end)
{
    std::uint64_t sum = 0;
    for (auto it = begin; it != end; ++it) {
        sum += *it;
    }
    return static_cast<std::uint8_t>(sum % 255);
}

std::vector<std::uint8_t> mix_kfuzzy_blocks(const std::vector<std::uint8_t>& bytes)
{
    constexpr std::size_t block_size = 512;
    std::vector<std::uint8_t> mixed;
    mixed.reserve(bytes.size() * 2);

    for (std::size_t offset = 0;; offset += block_size) {
        const auto chunk_size = std::min(block_size, offset < bytes.size() ? bytes.size() - offset : 0);
        mixed.insert(mixed.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset),
            bytes.begin() + static_cast<std::ptrdiff_t>(offset + chunk_size));
        mixed.insert(mixed.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset),
            bytes.begin() + static_cast<std::ptrdiff_t>(offset + chunk_size));
        if (chunk_size < block_size) {
            break;
        }
    }

    return mixed;
}

std::string kfuzzy_hash_bytes(const std::vector<std::uint8_t>& bytes, bool aggressive = false)
{
    constexpr std::size_t block_size = 512;
    constexpr std::size_t output_size = 32;
    constexpr std::size_t ignore_range = 2;

    std::vector<std::uint8_t> block_sums;
    for (std::size_t idx = 0;; ++idx) {
        const auto chunk_start = idx * block_size;
        const auto chunk_end = std::min(chunk_start + block_size, bytes.size());
        const auto sum = chunk_start < bytes.size()
            ? modsum(bytes.begin() + static_cast<std::ptrdiff_t>(chunk_start),
                bytes.begin() + static_cast<std::ptrdiff_t>(chunk_end))
            : 0;
        if (sum != 255 && sum != 0) {
            block_sums.push_back(sum);
        }
        if (chunk_start + block_size > bytes.size()) {
            break;
        }
    }

    const auto window_extra = std::min(block_sums.size() / output_size, std::size_t{1});
    std::vector<std::uint8_t> signature;
    for (std::size_t c = 0; c < output_size; ++c) {
        const auto start = std::min(c, block_sums.size());
        const auto end = std::min(c + window_extra + 1, block_sums.size());
        if (aggressive) {
            if (start + ignore_range < end) {
                signature.push_back(block_sums[start + ignore_range]);
            }
        } else if (start + 1 < end) {
            signature.push_back(block_sums[start + 1]);
        }

        std::size_t i = 0;
        for (std::size_t pos = start; pos < end; ++pos) {
            ++i;
            if (i != ignore_range) {
                continue;
            }
            i = 0;
            signature.push_back(block_sums[pos]);
            break;
        }
    }

    return base64_without_padding(signature, output_size);
}

std::array<std::string, 3> koret_fuzzy_hashes(std::string_view text)
{
    if (text.empty()) {
        return {};
    }
    auto bytes = bytes_from_text(text);
    auto mixed = mix_kfuzzy_blocks(bytes);
    auto reversed = bytes;
    std::reverse(reversed.begin(), reversed.end());

    return {
        kfuzzy_hash_bytes(mixed),
        kfuzzy_hash_bytes(bytes),
        kfuzzy_hash_bytes(reversed),
    };
}

void attach_pseudocode_comments(cfunc_t* cfunc, soff::FunctionFeature& feature, HexRaysExportContext& hexrays)
{
    if (cfunc == nullptr) {
        return;
    }

    user_cmts_t* comments = restore_user_cmts(cfunc->entry_ea);
    if (comments == nullptr) {
        return;
    }

    std::map<soff::Address, std::size_t> instruction_indices;
    for (std::size_t i = 0; i < feature.instruction_details.size(); ++i) {
        instruction_indices[feature.instruction_details[i].address] = i;
    }

    for (auto it = user_cmts_begin(comments); it != user_cmts_end(comments); it = user_cmts_next(it)) {
        const treeloc_t& location = user_cmts_first(it);
        citem_cmt_t& comment = user_cmts_second(it);
        const auto instruction = instruction_indices.find(static_cast<soff::Address>(location.ea));
        if (instruction == instruction_indices.end()) {
            continue;
        }
        auto& instruction_feature = feature.instruction_details[instruction->second];
        instruction_feature.pseudocomment = to_string(comment);
        instruction_feature.pseudoitp = static_cast<std::uint64_t>(location.itp);
        ++hexrays.pseudocode_comments;
    }

    user_cmts_free(comments);
}

std::vector<std::string> split_lines(std::string_view text)
{
    std::vector<std::string> lines;
    std::string current;
    for (const char ch : text) {
        if (ch == '\n') {
            lines.push_back(std::move(current));
            current.clear();
            continue;
        }
        if (ch != '\r') {
            current.push_back(ch);
        }
    }
    lines.push_back(std::move(current));
    return lines;
}

std::vector<std::string> split_non_word(std::string_view text)
{
    std::vector<std::string> tokens;
    std::string current;
    for (const unsigned char ch : text) {
        if (std::isalnum(ch) != 0 || ch == '_') {
            current.push_back(static_cast<char>(ch));
            continue;
        }
        if (!current.empty()) {
            tokens.push_back(std::move(current));
            current.clear();
        }
    }
    if (!current.empty()) {
        tokens.push_back(std::move(current));
    }
    return tokens;
}

