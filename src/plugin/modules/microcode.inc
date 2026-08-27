bool is_decimal_token(std::string_view text)
{
    return !text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char ch) {
        return std::isdigit(ch) != 0;
    });
}

const std::vector<std::string>& microcode_instruction_names();

bool is_microcode_mnemonic(std::string text)
{
    if (const auto dot = text.find('.'); dot != std::string::npos) {
        text.erase(dot);
    }
    const auto& names = microcode_instruction_names();
    const auto it = std::lower_bound(names.begin(), names.end(), text);
    return it != names.end() && *it == text;
}

std::pair<std::string, std::string> plain_microcode_line(std::string line)
{
    line = trim_copy(std::move(line));
    std::string mnemonic;

    std::istringstream input(line);
    std::vector<std::string> tokens;
    for (std::string token; input >> token;) {
        tokens.push_back(std::move(token));
    }

    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (i != 0 && is_decimal_token(tokens[i])) {
            continue;
        }
        if (!is_microcode_mnemonic(tokens[i])) {
            continue;
        }
        mnemonic = tokens[i];
        const auto pos = line.find(mnemonic);
        if (pos != std::string::npos) {
            line.erase(0, pos);
        }
        break;
    }

    return {line, mnemonic};
}

soff::Address parse_microcode_line_address(std::string_view comments)
{
    std::string token;
    for (const char ch : comments) {
        if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
            break;
        }
        token.push_back(ch);
    }
    if (token.empty()) {
        return 0;
    }
    try {
        return static_cast<soff::Address>(std::stoull(token, nullptr, 16));
    } catch (...) {
        return 0;
    }
}

soff::Address synthetic_microcode_block_address(ea_t function_ea, int serial)
{
    const auto base = static_cast<soff::Address>(function_ea);
    return 0x8000000000000000ULL | ((base & 0x0000FFFFFFFFULL) << 16)
        | static_cast<soff::Address>(serial & 0xFFFF);
}

const std::vector<std::string>& microcode_instruction_names()
{
    static const auto names = [] {
        std::vector<std::string> values{
            "nop", "stx", "ldx", "ldc", "mov", "neg", "lnot", "bnot",
            "xds", "xdu", "low", "high", "add", "sub", "mul", "udiv",
            "sdiv", "umod", "smod", "or", "and", "xor", "shl", "shr",
            "sar", "cfadd", "ofadd", "cfshl", "cfshr", "sets", "seto",
            "setp", "setnz", "setz", "setae", "setb", "seta", "setbe",
            "setg", "setge", "setl", "setle", "jcnd", "jnz", "jz",
            "jae", "jb", "ja", "jbe", "jg", "jge", "jl", "jle",
            "jtbl", "ijmp", "goto", "call", "icall", "ret", "push",
            "pop", "und", "ext", "f2i", "f2u", "i2f", "u2f", "f2f",
            "fneg", "fadd", "fsub", "fmul", "fdiv",
        };
        std::sort(values.begin(), values.end());
        values.erase(std::unique(values.begin(), values.end()), values.end());
        return values;
    }();
    return names;
}

std::uint64_t microcode_mnemonic_prime(const std::string& mnemonic)
{
    const auto& names = microcode_instruction_names();
    const auto it = std::lower_bound(names.begin(), names.end(), mnemonic);
    if (it == names.end() || *it != mnemonic) {
        return 1;
    }
    return prime_at(static_cast<std::size_t>(std::distance(names.begin(), it)));
}

std::string clean_microcode_text(std::string_view microcode)
{
    std::string cleaned;
    for (auto line : split_lines(microcode)) {
        line = trim_copy(std::move(line));
        if (line.empty()) {
            continue;
        }
        auto [plain_line, mnemonic] = plain_microcode_line(line);
        if (mnemonic.empty()) {
            continue;
        }
        if (const auto dot = mnemonic.find('.'); dot != std::string::npos) {
            mnemonic.erase(dot);
        }
        const auto pos = plain_line.find(mnemonic);
        if (pos != std::string::npos) {
            plain_line.erase(0, pos);
        }
        append_line(cleaned, clean_assembly_line(plain_line));
    }
    return cleaned;
}

void extract_microcode_features(ea_t func_ea, soff::FunctionFeature& feature, HexRaysExportContext& hexrays)
{
    if (!hexrays.available) {
        return;
    }

    hexrays_failure_t failure;
    soff::perf::add_counter("export.hexrays.microcode.attempt");
    // RAII: any exception while collecting blocks would otherwise leak the mba.
    std::unique_ptr<mba_t> mba(soff_gen_microcode(func_ea, &failure));
    if (mba == nullptr) {
        ++hexrays.microcode_failures;
        record_hexrays_failure(hexrays, failure);
        return;
    }

    qstring rendered;
    qstring_printer_t printer(nullptr, rendered, true);
    mba->print(printer);

    mba->build_graph();
    for (int i = 0; i < mba->qty; ++i) {
        // Serial 0 is the entry block and contains real instructions; only the
        // BLT_STOP sentinel is excluded.
        const mblock_t* block = mba->get_mblock(static_cast<uint>(i));
        if (block == nullptr || block->type == BLT_STOP) {
            continue;
        }

        soff::BasicBlock micro_block;
        micro_block.start = synthetic_microcode_block_address(func_ea, i);
        micro_block.end = micro_block.start + 1;

        qstring block_rendered;
        qstring_printer_t block_printer(nullptr, block_rendered, true);
        block->print(block_printer);
        for (auto line : split_lines(untag_text(block_rendered.c_str()))) {
            line = trim_copy(std::move(line));
            if (line.empty()) {
                continue;
            }

            std::string comments;
            if (const auto semicolon = line.find(';'); semicolon != std::string::npos) {
                comments = trim_copy(line.substr(semicolon + 1));
                line.erase(semicolon);
            }
            auto [plain_line, mnemonic] = plain_microcode_line(line);
            if (plain_line.empty() || mnemonic.empty()) {
                continue;
            }
            if (const auto dot = mnemonic.find('.'); dot != std::string::npos) {
                mnemonic.erase(dot);
            }

            const auto synthetic_line_address = micro_block.start + 0x100 + micro_block.instructions.size();
            auto line_address = parse_microcode_line_address(comments);
            if (line_address == 0) {
                line_address = synthetic_line_address;
            }
            feature.microcode_instruction_details.push_back(
                {line_address, trim_copy(std::move(plain_line)), std::move(mnemonic)});
            micro_block.instructions.push_back(static_cast<soff::Address>(feature.microcode_instruction_details.size() - 1));
        }

        for (int successor_index = 0; successor_index < block->nsucc(); ++successor_index) {
            const auto successor = block->succ(successor_index);
            if (successor < 0 || successor >= mba->qty) {
                continue;
            }
            const mblock_t* successor_block = mba->get_mblock(static_cast<uint>(successor));
            if (successor_block == nullptr || successor_block->type == BLT_STOP) {
                continue;
            }
            micro_block.successors.push_back(synthetic_microcode_block_address(func_ea, successor));
        }

        feature.microcode_blocks.push_back(std::move(micro_block));
    }

    std::set<std::string> unique_mnemonics;
    std::string microcode;
    for (auto line : split_lines(untag_text(rendered.c_str()))) {
        if (const auto semicolon = line.find(';'); semicolon != std::string::npos) {
            line.erase(semicolon);
        }
        const auto tokens = split_non_word(line);
        if (tokens.size() <= 2) {
            continue;
        }

        line = trim_copy(std::move(line));
        append_line(microcode, line);

        auto [plain_line, mnemonic] = plain_microcode_line(line);
        if (mnemonic.empty()) {
            continue;
        }
        if (const auto dot = mnemonic.find('.'); dot != std::string::npos) {
            mnemonic.erase(dot);
        }
        if (!mnemonic.empty()) {
            unique_mnemonics.insert(mnemonic);
        }
    }

    if (microcode.empty()) {
        ++hexrays.microcode_failures;
        return;
    }

    std::string microcode_spp = "1";
    for (const auto& mnemonic : unique_mnemonics) {
        microcode_spp = decimal_multiply(std::move(microcode_spp), microcode_mnemonic_prime(mnemonic));
    }

    feature.microcode = std::move(microcode);
    feature.stripped_microcode = clean_microcode_text(feature.microcode);
    feature.microcode_spp = std::move(microcode_spp);
    ++hexrays.microcode_functions;
}

