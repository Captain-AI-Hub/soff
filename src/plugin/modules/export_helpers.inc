void write_crash_marker(const std::filesystem::path& crash_path, const std::string& content)
{
    std::ofstream file(crash_path, std::ios::binary | std::ios::trunc);
    if (!file) {
        throw soff::Error(soff::ErrorCode::export_failed, "failed to create export crash marker: " + crash_path.string());
    }
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!file) {
        throw soff::Error(soff::ErrorCode::export_failed, "failed to write export crash marker: " + crash_path.string());
    }
}

void update_crash_marker(
    const std::filesystem::path* crash_path,
    std::size_t index,
    std::size_t total,
    std::size_t exported,
    std::size_t skipped,
    ea_t address,
    std::string_view name,
    std::string_view phase)
{
    if (crash_path == nullptr) {
        return;
    }

    std::ostringstream marker;
    marker << "soff export in progress\n"
           << "phase=" << phase << '\n'
           << "index=" << index << '\n'
           << "total=" << total << '\n'
           << "exported=" << exported << '\n'
           << "skipped=" << skipped << '\n'
           << "address=" << static_cast<soff::Address>(address) << '\n'
           << "name=" << escape_marker_value(std::string(name)) << '\n';
    write_crash_marker(*crash_path, marker.str());
}

std::string hex_u64(std::uint64_t value)
{
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << value;
    return out.str();
}

std::string hex_bytes(const std::uint8_t* bytes, std::size_t size)
{
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < size; ++i) {
        out << std::setw(2) << static_cast<unsigned>(bytes[i]);
    }
    return out.str();
}

std::string md5_hex(const std::vector<std::uint8_t>& bytes)
{
    boost::uuids::detail::md5 hasher;
    if (!bytes.empty()) {
        hasher.process_bytes(bytes.data(), bytes.size());
    }
    boost::uuids::detail::md5::digest_type digest;
    hasher.get_digest(digest);
    std::uint8_t raw[16];
    std::memcpy(raw, digest, 16);
    return hex_bytes(raw, 16);
}

std::vector<std::uint8_t> bytes_from_text(std::string_view text)
{
    return {text.begin(), text.end()};
}

std::string md5_text(std::string_view text)
{
    return md5_hex(bytes_from_text(text));
}

void append_mnemonic(std::string& mnemonics, const std::string& mnemonic)
{
    if (mnemonic.empty()) {
        return;
    }
    if (!mnemonics.empty()) {
        mnemonics.push_back('\n');
    }
    mnemonics += mnemonic;
}

void append_line(std::string& lines, const std::string& line)
{
    if (!lines.empty()) {
        lines.push_back('\n');
    }
    lines += line;
}

std::string json_address_array(const std::vector<soff::Address>& values)
{
    std::ostringstream out;
    out << '[';
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        out << '"' << values[i] << '"';
    }
    out << ']';
    return out.str();
}

void append_json_escaped(std::ostringstream& out, std::string_view value)
{
    out << '"';
    for (const char ch : value) {
        switch (ch) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            // Control characters must be escaped for the JSON to be valid.
            if (static_cast<unsigned char>(ch) < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(ch));
                out << buffer;
            } else {
                out << ch;
            }
            break;
        }
    }
    out << '"';
}

std::string json_string_array(const std::set<std::string>& values)
{
    std::ostringstream out;
    out << '[';
    bool first = true;
    for (const auto& value : values) {
        if (!first) {
            out << ',';
        }
        first = false;
        append_json_escaped(out, value);
    }
    out << ']';
    return out.str();
}

void append_json_string(std::ostringstream& out, std::string_view value)
{
    append_json_escaped(out, value);
}

std::string forced_operands_json(ea_t ea, const insn_t& instruction)
{
    std::ostringstream out;
    out << '[';
    bool first = true;
    for (int index = 0; index < UA_MAXOP; ++index) {
        if (instruction.ops[index].type == o_void) {
            break;
        }
        if (!is_forced_operand(ea, index)) {
            continue;
        }
        qstring operand;
        if (get_forced_operand(&operand, ea, index) <= 0 || operand.empty()) {
            continue;
        }
        if (!first) {
            out << ',';
        }
        first = false;
        out << '[' << index << ',';
        append_json_string(out, to_string(operand));
        out << ']';
    }
    out << ']';
    return first ? std::string() : out.str();
}

std::string json_signed_array(const std::vector<sval_t>& values)
{
    std::ostringstream out;
    out << '[';
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        out << values[i];
    }
    out << ']';
    return out.str();
}

std::string json_switches(const std::vector<std::pair<std::size_t, std::vector<sval_t>>>& switches)
{
    std::ostringstream out;
    out << '[';
    for (std::size_t i = 0; i < switches.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        out << '[' << switches[i].first << ',' << json_signed_array(switches[i].second) << ']';
    }
    out << ']';
    return out.str();
}

std::string json_component_array(const std::vector<std::vector<std::size_t>>& components)
{
    std::ostringstream out;
    out << '[';
    for (std::size_t i = 0; i < components.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        out << '[';
        for (std::size_t j = 0; j < components[i].size(); ++j) {
            if (j != 0) {
                out << ',';
            }
            out << components[i][j];
        }
        out << ']';
    }
    out << ']';
    return out.str();
}

std::string decimal_multiply(std::string value, std::uint64_t multiplier)
{
    if (value.empty()) {
        value = "1";
    }
    if (multiplier == 0 || value == "0") {
        return "0";
    }

    std::uint64_t carry = 0;
    for (auto it = value.rbegin(); it != value.rend(); ++it) {
        const auto product = static_cast<std::uint64_t>(*it - '0') * multiplier + carry;
        *it = static_cast<char>('0' + (product % 10));
        carry = product / 10;
    }
    while (carry != 0) {
        value.insert(value.begin(), static_cast<char>('0' + (carry % 10)));
        carry /= 10;
    }
    return value;
}

std::vector<std::uint64_t> primes_below(std::size_t limit)
{
    std::vector<bool> composite(limit + 1, false);
    std::vector<std::uint64_t> primes;
    for (std::size_t candidate = 2; candidate < limit; ++candidate) {
        if (composite[candidate]) {
            continue;
        }
        primes.push_back(static_cast<std::uint64_t>(candidate));
        if (candidate * candidate <= limit) {
            for (std::size_t multiple = candidate * candidate; multiple < limit; multiple += candidate) {
                composite[multiple] = true;
            }
        }
    }
    return primes;
}

std::uint64_t prime_at(std::size_t index)
{
    static const auto primes = primes_below(2048 * 2048);
    return index < primes.size() ? primes[index] : 0;
}

