#include "soff/cli/mcp_server.hpp"

#include "soff/core/version.hpp"
#include "soff/db/database.hpp"
#include "soff/ui/line_diff.hpp"

#include <array>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace soff::cli {
namespace {

// ---------------------------------------------------------------------------
// Minimal JSON value, parser and writer. MCP messages are small, so a compact
// recursive-descent parser is enough and keeps the CLI dependency-free.
// ---------------------------------------------------------------------------

struct JsonValue
{
    enum class Type { null_t, boolean_t, number_t, string_t, array_t, object_t };

    Type type = Type::null_t;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<JsonValue> items;
    std::vector<std::pair<std::string, JsonValue>> fields;

    const JsonValue* find(std::string_view key) const
    {
        for (const auto& field : fields) {
            if (field.first == key) {
                return &field.second;
            }
        }
        return nullptr;
    }
};

class JsonParser
{
public:
    explicit JsonParser(std::string_view text)
        : text_(text)
    {
    }

    bool parse(JsonValue& out)
    {
        skip_ws();
        if (!parse_value(out, 0)) {
            return false;
        }
        skip_ws();
        return pos_ == text_.size();
    }

private:
    static constexpr int kMaxDepth = 64;

    std::string_view text_;
    std::size_t pos_ = 0;

    void skip_ws()
    {
        while (pos_ < text_.size()) {
            const char ch = text_[pos_];
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool consume(char expected)
    {
        if (pos_ < text_.size() && text_[pos_] == expected) {
            ++pos_;
            return true;
        }
        return false;
    }

    bool literal(std::string_view word)
    {
        if (text_.compare(pos_, word.size(), word) != 0) {
            return false;
        }
        pos_ += word.size();
        return true;
    }

    bool parse_value(JsonValue& out, int depth)
    {
        if (depth > kMaxDepth) {
            return false;
        }
        skip_ws();
        if (pos_ >= text_.size()) {
            return false;
        }
        const char ch = text_[pos_];
        if (ch == '{') {
            return parse_object(out, depth);
        }
        if (ch == '[') {
            return parse_array(out, depth);
        }
        if (ch == '"') {
            out.type = JsonValue::Type::string_t;
            out.string.clear();
            return parse_string_body(out.string);
        }
        if (ch == 't') {
            if (!literal("true")) {
                return false;
            }
            out.type = JsonValue::Type::boolean_t;
            out.boolean = true;
            return true;
        }
        if (ch == 'f') {
            if (!literal("false")) {
                return false;
            }
            out.type = JsonValue::Type::boolean_t;
            out.boolean = false;
            return true;
        }
        if (ch == 'n') {
            return literal("null");
        }
        return parse_number(out);
    }

    bool parse_object(JsonValue& out, int depth)
    {
        out.type = JsonValue::Type::object_t;
        ++pos_; // '{'
        skip_ws();
        if (consume('}')) {
            return true;
        }
        while (true) {
            skip_ws();
            if (pos_ >= text_.size() || text_[pos_] != '"') {
                return false;
            }
            std::string key;
            if (!parse_string_body(key)) {
                return false;
            }
            skip_ws();
            if (!consume(':')) {
                return false;
            }
            JsonValue value;
            if (!parse_value(value, depth + 1)) {
                return false;
            }
            out.fields.emplace_back(std::move(key), std::move(value));
            skip_ws();
            if (consume(',')) {
                continue;
            }
            return consume('}');
        }
    }

    bool parse_array(JsonValue& out, int depth)
    {
        out.type = JsonValue::Type::array_t;
        ++pos_; // '['
        skip_ws();
        if (consume(']')) {
            return true;
        }
        while (true) {
            JsonValue value;
            if (!parse_value(value, depth + 1)) {
                return false;
            }
            out.items.push_back(std::move(value));
            skip_ws();
            if (consume(',')) {
                continue;
            }
            return consume(']');
        }
    }

    bool parse_string_body(std::string& out)
    {
        ++pos_; // opening quote
        while (pos_ < text_.size()) {
            const char ch = text_[pos_];
            if (ch == '"') {
                ++pos_;
                return true;
            }
            if (static_cast<unsigned char>(ch) < 0x20) {
                return false;
            }
            if (ch != '\\') {
                out.push_back(ch);
                ++pos_;
                continue;
            }
            ++pos_; // backslash
            if (pos_ >= text_.size()) {
                return false;
            }
            const char escape = text_[pos_++];
            switch (escape) {
            case '"':
                out.push_back('"');
                break;
            case '\\':
                out.push_back('\\');
                break;
            case '/':
                out.push_back('/');
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u': {
                std::uint32_t code = 0;
                if (!parse_hex4(code)) {
                    return false;
                }
                if (code >= 0xD800 && code <= 0xDBFF) {
                    // High surrogate: must pair with a \uXXXX low surrogate.
                    if (pos_ + 1 >= text_.size() || text_[pos_] != '\\' || text_[pos_ + 1] != 'u') {
                        return false;
                    }
                    pos_ += 2;
                    std::uint32_t low = 0;
                    if (!parse_hex4(low) || low < 0xDC00 || low > 0xDFFF) {
                        return false;
                    }
                    append_utf8(out, 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00));
                    break;
                }
                append_utf8(out, code);
                break;
            }
            default:
                return false;
            }
        }
        return false; // unterminated string
    }

    bool parse_hex4(std::uint32_t& out)
    {
        if (pos_ + 4 > text_.size()) {
            return false;
        }
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char ch = text_[pos_++];
            out <<= 4;
            if (ch >= '0' && ch <= '9') {
                out |= static_cast<std::uint32_t>(ch - '0');
            } else if (ch >= 'a' && ch <= 'f') {
                out |= static_cast<std::uint32_t>(ch - 'a' + 10);
            } else if (ch >= 'A' && ch <= 'F') {
                out |= static_cast<std::uint32_t>(ch - 'A' + 10);
            } else {
                return false;
            }
        }
        return true;
    }

    static void append_utf8(std::string& out, std::uint32_t code)
    {
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    bool parse_number(JsonValue& out)
    {
        const std::size_t start = pos_;
        if (pos_ < text_.size() && text_[pos_] == '-') {
            ++pos_;
        }
        while (pos_ < text_.size()) {
            const char ch = text_[pos_];
            if ((ch >= '0' && ch <= '9') || ch == '.' || ch == 'e' || ch == 'E' || ch == '+' || ch == '-') {
                ++pos_;
            } else {
                break;
            }
        }
        if (pos_ == start) {
            return false;
        }
        const std::string token(text_.substr(start, pos_ - start));
        errno = 0;
        char* end = nullptr;
        const double value = std::strtod(token.c_str(), &end);
        if (end == nullptr || *end != '\0') {
            return false;
        }
        out.type = JsonValue::Type::number_t;
        out.number = value;
        return true;
    }
};

void append_json_string(std::string& out, std::string_view value)
{
    out.push_back('"');
    for (const char ch : value) {
        const auto byte = static_cast<unsigned char>(ch);
        switch (ch) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (byte < 0x20) {
                static const char hex[] = "0123456789abcdef";
                out += "\\u00";
                out.push_back(hex[byte >> 4]);
                out.push_back(hex[byte & 0xF]);
            } else {
                out.push_back(ch);
            }
        }
    }
    out.push_back('"');
}

void append_json_int(std::string& out, std::int64_t value)
{
    out += std::to_string(value);
}

// Appends a numeric SQLite text value verbatim when it is a valid JSON number,
// so ratios keep SQLite's compact formatting (e.g. "0.85").
bool append_json_number_text(std::string& out, const std::string& text)
{
    if (text.empty()) {
        return false;
    }
    const char first = text.front();
    if (!((first >= '0' && first <= '9') || first == '-')) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    std::strtod(text.c_str(), &end);
    if (end == nullptr || *end != '\0') {
        return false;
    }
    out += text;
    return true;
}

std::string serialize_json_value(const JsonValue& value)
{
    switch (value.type) {
    case JsonValue::Type::null_t:
        return "null";
    case JsonValue::Type::boolean_t:
        return value.boolean ? "true" : "false";
    case JsonValue::Type::number_t: {
        if (std::isfinite(value.number) && value.number == std::floor(value.number)
            && std::fabs(value.number) <= 9.0e15) {
            return std::to_string(static_cast<std::int64_t>(value.number));
        }
        std::ostringstream out;
        out << value.number;
        return out.str();
    }
    case JsonValue::Type::string_t: {
        std::string out;
        append_json_string(out, value.string);
        return out;
    }
    case JsonValue::Type::array_t: {
        std::string out = "[";
        for (std::size_t i = 0; i < value.items.size(); ++i) {
            if (i != 0) {
                out += ',';
            }
            out += serialize_json_value(value.items[i]);
        }
        out += ']';
        return out;
    }
    case JsonValue::Type::object_t: {
        std::string out = "{";
        for (std::size_t i = 0; i < value.fields.size(); ++i) {
            if (i != 0) {
                out += ',';
            }
            append_json_string(out, value.fields[i].first);
            out += ':';
            out += serialize_json_value(value.fields[i].second);
        }
        out += '}';
        return out;
    }
    }
    return "null";
}

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

std::string_view trim(std::string_view text)
{
    while (!text.empty()
        && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n')) {
        text.remove_prefix(1);
    }
    while (!text.empty()
        && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n')) {
        text.remove_suffix(1);
    }
    return text;
}

// Accepts decimal, 0x-prefixed hex and trailing-h hex; returns the decimal
// form used by the export/result databases, or the trimmed input unchanged.
std::string normalize_address(std::string_view text)
{
    const auto trimmed = trim(text);
    std::string_view hex;
    if (trimmed.size() > 2 && trimmed.substr(0, 2) == "0x") {
        hex = trimmed.substr(2);
    } else if (trimmed.size() > 1 && (trimmed.back() == 'h' || trimmed.back() == 'H')) {
        hex = trimmed.substr(0, trimmed.size() - 1);
    }
    if (!hex.empty()) {
        std::uint64_t value = 0;
        const auto* begin = hex.data();
        const auto* end = hex.data() + hex.size();
        const auto result = std::from_chars(begin, end, value, 16);
        if (result.ec == std::errc{} && result.ptr == end) {
            return std::to_string(value);
        }
    }
    return std::string(trimmed);
}

// Relative export paths stored in the .soff config resolve against the
// directory holding the result database.
std::filesystem::path resolve_db_path(const std::filesystem::path& result_path, const std::string& stored)
{
    const std::filesystem::path path(stored);
    if (stored.empty() || path.is_absolute()) {
        return path;
    }
    const auto parent = result_path.parent_path();
    if (parent.empty()) {
        return path;
    }
    return parent / path;
}

std::vector<std::string> split_lines(const std::string& text)
{
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find('\n', start);
        if (end == std::string::npos) {
            if (start < text.size()) {
                lines.push_back(text.substr(start));
            }
            break;
        }
        auto line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
        start = end + 1;
    }
    return lines;
}

std::vector<std::string> unified_diff_lines(const std::string& left, const std::string& right)
{
    const auto left_lines = split_lines(left);
    const auto right_lines = split_lines(right);
    const auto entries = soff::ui::compute_line_diff(left_lines, right_lines);
    std::vector<std::string> lines;
    lines.reserve(entries.size());
    for (const auto& entry : entries) {
        switch (entry.kind) {
        case soff::ui::DiffEntry::same:
            lines.push_back(" " + left_lines.at(entry.left_index));
            break;
        case soff::ui::DiffEntry::removed:
            lines.push_back("-" + left_lines.at(entry.left_index));
            break;
        case soff::ui::DiffEntry::added:
            lines.push_back("+" + right_lines.at(entry.right_index));
            break;
        }
    }
    return lines;
}

// ---------------------------------------------------------------------------
// Database access
// ---------------------------------------------------------------------------

soff::db::Database open_soff_database(const std::string& result_path)
{
    const std::filesystem::path path(result_path);
    if (result_path.empty() || !std::filesystem::exists(path)) {
        throw std::runtime_error("result database does not exist: " + result_path);
    }
    soff::db::Database database;
    database.open_read_only(path);
    const auto tables = database.query_int(
        "select count(*) from sqlite_master where type = 'table' and name in ('config', 'results', 'unmatched')");
    if (tables != 3) {
        throw std::runtime_error("not a .soff result database: " + result_path);
    }
    return database;
}

std::pair<std::string, std::string> load_stored_paths(soff::db::Database& database)
{
    const auto rows = database.query_rows("select main_db, diff_db from config limit 1");
    if (rows.empty() || rows.front().size() < 2) {
        throw std::runtime_error("result database has no config row with main_db/diff_db");
    }
    return {rows.front()[0], rows.front()[1]};
}

std::string query_function_column(
    const std::filesystem::path& db_path,
    const std::string& address,
    std::string_view column)
{
    if (!std::filesystem::exists(db_path)) {
        throw std::runtime_error("export database does not exist: " + db_path.string());
    }
    soff::db::Database database;
    database.open_read_only(db_path);
    auto statement = database.prepare(
        "select " + std::string(column) + " from functions where address = ? limit 1");
    statement.bind(1, address);
    if (statement.step()) {
        return statement.column_text(0);
    }
    return "";
}

// ---------------------------------------------------------------------------
// Tool argument helpers
// ---------------------------------------------------------------------------

std::string require_string(const JsonValue& args, const char* key)
{
    const auto* value = args.find(key);
    if (value == nullptr || value->type != JsonValue::Type::string_t) {
        throw std::runtime_error(std::string("missing required string argument: ") + key);
    }
    return value->string;
}

std::string optional_string(const JsonValue& args, const char* key, std::string fallback)
{
    const auto* value = args.find(key);
    if (value == nullptr || value->type == JsonValue::Type::null_t) {
        return fallback;
    }
    if (value->type != JsonValue::Type::string_t) {
        throw std::runtime_error(std::string("argument must be a string: ") + key);
    }
    return value->string;
}

std::int64_t optional_int(
    const JsonValue& args,
    const char* key,
    std::int64_t fallback,
    std::int64_t min_value,
    std::int64_t max_value)
{
    const auto* value = args.find(key);
    if (value == nullptr || value->type == JsonValue::Type::null_t) {
        return fallback;
    }
    if (value->type != JsonValue::Type::number_t || value->number != std::floor(value->number)
        || std::fabs(value->number) > 9.0e15) {
        throw std::runtime_error(std::string("argument must be an integer: ") + key);
    }
    const auto integer = static_cast<std::int64_t>(value->number);
    return integer < min_value ? min_value : (integer > max_value ? max_value : integer);
}

// ---------------------------------------------------------------------------
// Tool implementations (parity with the desktop MCP server)
// ---------------------------------------------------------------------------

std::string tool_diff_results(const JsonValue& args)
{
    const auto result_path_text = require_string(args, "result_path");
    const auto match_type = optional_string(args, "match_type", "all");
    if (match_type != "all" && match_type != "best" && match_type != "partial"
        && match_type != "unreliable" && match_type != "multimatch") {
        throw std::runtime_error(
            "invalid match_type: " + match_type
            + " (expected all, best, partial, unreliable or multimatch)");
    }
    // Integers are validated and clamped above, so inlining them is safe.
    const auto limit = optional_int(args, "limit", 100, 0, 1000);
    const auto offset = optional_int(args, "offset", 0, 0, 1000000000);

    auto database = open_soff_database(result_path_text);
    const auto [stored_main, stored_diff] = load_stored_paths(database);
    const std::filesystem::path result_path(result_path_text);
    const auto filter = match_type != "all";

    std::int64_t total = 0;
    {
        auto statement = database.prepare(
            filter ? "select count(*) from results where type = ?" : "select count(*) from results");
        if (filter) {
            statement.bind(1, match_type);
        }
        if (statement.step()) {
            total = statement.column_int64(0);
        }
    }

    std::string out = "{\"main_db\":";
    append_json_string(out, resolve_db_path(result_path, stored_main).string());
    out += ",\"diff_db\":";
    append_json_string(out, resolve_db_path(result_path, stored_diff).string());
    out += ",\"total\":";
    append_json_int(out, total);
    out += ",\"items\":[";
    {
        const auto sql = filter
            ? "select type, address, name, address2, name2, ratio, nodes1, nodes2, description "
              "from results where type = ? order by ratio asc, line limit "
            : "select type, address, name, address2, name2, ratio, nodes1, nodes2, description "
              "from results order by ratio asc, line limit ";
        auto statement = database.prepare(
            sql + std::to_string(limit) + " offset " + std::to_string(offset));
        if (filter) {
            statement.bind(1, match_type);
        }
        auto first = true;
        while (statement.step()) {
            if (!first) {
                out += ',';
            }
            first = false;
            out += "{\"match_type\":";
            append_json_string(out, statement.column_text(0));
            out += ",\"primary_addr\":";
            append_json_string(out, statement.column_text(1));
            out += ",\"primary_name\":";
            append_json_string(out, statement.column_text(2));
            out += ",\"secondary_addr\":";
            append_json_string(out, statement.column_text(3));
            out += ",\"secondary_name\":";
            append_json_string(out, statement.column_text(4));
            out += ",\"ratio\":";
            if (!append_json_number_text(out, statement.column_text(5))) {
                out += '0';
            }
            out += ",\"nodes1\":";
            append_json_int(out, statement.column_int64(6));
            out += ",\"nodes2\":";
            append_json_int(out, statement.column_int64(7));
            out += ",\"description\":";
            append_json_string(out, statement.column_text(8));
            out += '}';
        }
    }
    out += "]}";
    return out;
}

std::string tool_diff_unmatched(const JsonValue& args)
{
    const auto result_path_text = require_string(args, "result_path");
    const auto side = optional_string(args, "side", "all");
    if (side != "all" && side != "primary" && side != "secondary") {
        throw std::runtime_error("invalid side: " + side + " (expected all, primary or secondary)");
    }
    const auto limit = optional_int(args, "limit", 100, 0, 1000);
    const auto offset = optional_int(args, "offset", 0, 0, 1000000000);

    auto database = open_soff_database(result_path_text);
    const auto filter = side != "all";

    std::int64_t total = 0;
    {
        auto statement = database.prepare(
            filter ? "select count(*) from unmatched where type = ?" : "select count(*) from unmatched");
        if (filter) {
            statement.bind(1, side);
        }
        if (statement.step()) {
            total = statement.column_int64(0);
        }
    }

    std::string out = "{\"total\":";
    append_json_int(out, total);
    out += ",\"items\":[";
    {
        const auto sql = filter
            ? "select type, address, name from unmatched where type = ? order by line limit "
            : "select type, address, name from unmatched order by line limit ";
        auto statement = database.prepare(
            sql + std::to_string(limit) + " offset " + std::to_string(offset));
        if (filter) {
            statement.bind(1, side);
        }
        auto first = true;
        while (statement.step()) {
            if (!first) {
                out += ',';
            }
            first = false;
            out += "{\"side\":";
            append_json_string(out, statement.column_text(0));
            out += ",\"address\":";
            append_json_string(out, statement.column_text(1));
            out += ",\"name\":";
            append_json_string(out, statement.column_text(2));
            out += '}';
        }
    }
    out += "]}";
    return out;
}

std::string tool_function_diff(const JsonValue& args, std::string_view column)
{
    const auto result_path_text = require_string(args, "result_path");
    const auto primary_addr = normalize_address(require_string(args, "primary_addr"));
    const auto secondary_addr = normalize_address(require_string(args, "secondary_addr"));

    auto database = open_soff_database(result_path_text);
    const auto [stored_main, stored_diff] = load_stored_paths(database);
    const std::filesystem::path result_path(result_path_text);
    const auto main_db = resolve_db_path(result_path, stored_main);
    const auto diff_db = resolve_db_path(result_path, stored_diff);

    const auto left = query_function_column(main_db, primary_addr, column);
    const auto right = query_function_column(diff_db, secondary_addr, column);

    std::string out = "{\"main_db\":";
    append_json_string(out, main_db.string());
    out += ",\"diff_db\":";
    append_json_string(out, diff_db.string());
    out += ",\"primary_addr\":";
    append_json_string(out, primary_addr);
    out += ",\"secondary_addr\":";
    append_json_string(out, secondary_addr);
    out += ",\"lines\":[";
    auto first = true;
    for (const auto& line : unified_diff_lines(left, right)) {
        if (!first) {
            out += ',';
        }
        first = false;
        append_json_string(out, line);
    }
    out += "]}";
    return out;
}

bool is_known_tool(std::string_view name)
{
    return name == "soff_diff_results" || name == "soff_diff_unmatched"
        || name == "soff_diff_asm" || name == "soff_diff_pseudo";
}

std::string dispatch_tool(std::string_view name, const JsonValue& args)
{
    if (name == "soff_diff_results") {
        return tool_diff_results(args);
    }
    if (name == "soff_diff_unmatched") {
        return tool_diff_unmatched(args);
    }
    if (name == "soff_diff_asm") {
        return tool_function_diff(args, "assembly");
    }
    return tool_function_diff(args, "pseudocode");
}

// ---------------------------------------------------------------------------
// Protocol responses
// ---------------------------------------------------------------------------

struct ToolDescriptor
{
    const char* name;
    const char* description;
    const char* input_schema;
};

const std::array<ToolDescriptor, 4>& builtin_mcp_tools()
{
    static const std::array<ToolDescriptor, 4> tools = {{
        {
            "soff_diff_results",
            "Query matched function pairs from a .soff result database.",
            R"json({"type":"object","properties":{"result_path":{"type":"string","description":"Path to the .soff result database"},"match_type":{"type":"string","enum":["all","best","partial","unreliable","multimatch"],"description":"Match type: all, best, partial, unreliable, or multimatch"},"limit":{"type":"integer","maximum":1000,"description":"Maximum rows to return"},"offset":{"type":"integer","description":"Rows to skip"}},"required":["result_path"]})json",
        },
        {
            "soff_diff_unmatched",
            "Query unmatched functions from a .soff result database.",
            R"json({"type":"object","properties":{"result_path":{"type":"string","description":"Path to the .soff result database"},"side":{"type":"string","enum":["all","primary","secondary"],"description":"Unmatched side: all, primary, or secondary"},"limit":{"type":"integer","maximum":1000,"description":"Maximum rows to return"},"offset":{"type":"integer","description":"Rows to skip"}},"required":["result_path"]})json",
        },
        {
            "soff_diff_asm",
            "Return a unified assembly diff for a matched function pair using paths stored in the .soff config.",
            R"json({"type":"object","properties":{"result_path":{"type":"string","description":"Path to the .soff result database"},"primary_addr":{"type":"string","description":"Primary function address; accepts decimal, 0x-prefixed hex, or trailing-h hex"},"secondary_addr":{"type":"string","description":"Secondary function address; accepts decimal, 0x-prefixed hex, or trailing-h hex"}},"required":["result_path","primary_addr","secondary_addr"]})json",
        },
        {
            "soff_diff_pseudo",
            "Return a unified pseudocode diff for a matched function pair using paths stored in the .soff config.",
            R"json({"type":"object","properties":{"result_path":{"type":"string","description":"Path to the .soff result database"},"primary_addr":{"type":"string","description":"Primary function address; accepts decimal, 0x-prefixed hex, or trailing-h hex"},"secondary_addr":{"type":"string","description":"Secondary function address; accepts decimal, 0x-prefixed hex, or trailing-h hex"}},"required":["result_path","primary_addr","secondary_addr"]})json",
        },
    }};
    return tools;
}

std::string make_result(const JsonValue& id, std::string result_json)
{
    std::string out = "{\"jsonrpc\":\"2.0\",\"id\":";
    out += serialize_json_value(id);
    out += ",\"result\":";
    out += std::move(result_json);
    out += '}';
    return out;
}

std::string make_error(const JsonValue& id, int code, std::string_view message)
{
    std::string out = "{\"jsonrpc\":\"2.0\",\"id\":";
    out += serialize_json_value(id);
    out += ",\"error\":{\"code\":";
    append_json_int(out, code);
    out += ",\"message\":";
    append_json_string(out, message);
    out += "}}";
    return out;
}

std::string build_initialize_result(const JsonValue& params)
{
    std::string protocol_version = "2025-06-18";
    if (const auto* requested = params.find("protocolVersion");
        requested != nullptr && requested->type == JsonValue::Type::string_t
        && !requested->string.empty()) {
        protocol_version = requested->string;
    }
    std::string out = "{\"protocolVersion\":";
    append_json_string(out, protocol_version);
    out += ",\"capabilities\":{\"tools\":{\"listChanged\":false}},\"serverInfo\":{\"name\":\"soff-mcp\",\"title\":\"Soff MCP Server\",\"version\":";
    append_json_string(out, std::string(soff::version()));
    out += "},\"instructions\":";
    append_json_string(
        out,
        "Read-only Soff binary-diff tools. Pass a .soff result database path to query matched or "
        "unmatched functions and to produce unified assembly/pseudocode diffs for matched pairs. "
        "This server speaks MCP over stdio via `soff mcp`.");
    out += '}';
    return out;
}

std::string build_tools_list_result()
{
    std::string out = "{\"tools\":[";
    auto first = true;
    for (const auto& tool : builtin_mcp_tools()) {
        if (!first) {
            out += ',';
        }
        first = false;
        out += "{\"name\":";
        append_json_string(out, tool.name);
        out += ",\"description\":";
        append_json_string(out, tool.description);
        out += ",\"inputSchema\":";
        out += tool.input_schema;
        out += '}';
    }
    out += "]}";
    return out;
}

std::optional<std::string> handle_request_object(const JsonValue& message)
{
    const auto* id = message.find("id");
    const auto* method = message.find("method");
    if (method == nullptr || method->type != JsonValue::Type::string_t) {
        if (id != nullptr) {
            return make_error(*id, -32600, "invalid request: missing method");
        }
        return std::nullopt;
    }
    // Notifications (no id) are never answered, including unknown ones.
    if (id == nullptr) {
        return std::nullopt;
    }
    const auto& name = method->string;

    JsonValue empty_params;
    empty_params.type = JsonValue::Type::object_t;
    const auto* params = message.find("params");
    if (params == nullptr) {
        params = &empty_params;
    }

    if (name == "initialize") {
        if (params->type != JsonValue::Type::object_t) {
            return make_error(*id, -32602, "invalid initialize params: expected object");
        }
        return make_result(*id, build_initialize_result(*params));
    }
    if (name == "ping") {
        return make_result(*id, "{}");
    }
    if (name == "tools/list") {
        return make_result(*id, build_tools_list_result());
    }
    if (name == "tools/call") {
        if (params->type != JsonValue::Type::object_t) {
            return make_error(*id, -32602, "invalid tools/call params: expected object");
        }
        const auto* tool_name = params->find("name");
        if (tool_name == nullptr || tool_name->type != JsonValue::Type::string_t) {
            return make_error(*id, -32602, "invalid tools/call params: missing tool name");
        }
        if (!is_known_tool(tool_name->string)) {
            return make_error(*id, -32602, "unknown tool: " + tool_name->string);
        }
        JsonValue empty_args;
        empty_args.type = JsonValue::Type::object_t;
        const auto* arguments = params->find("arguments");
        if (arguments == nullptr || arguments->type == JsonValue::Type::null_t) {
            arguments = &empty_args;
        } else if (arguments->type != JsonValue::Type::object_t) {
            return make_error(*id, -32602, "invalid tools/call params: arguments must be an object");
        }
        try {
            return make_result(*id, dispatch_tool(tool_name->string, *arguments));
        } catch (const std::exception& error) {
            // Tool execution failures are reported in-band per the MCP spec.
            std::string payload = "{\"content\":[{\"type\":\"text\",\"text\":";
            append_json_string(payload, error.what());
            payload += "}],\"isError\":true}";
            return make_result(*id, std::move(payload));
        }
    }
    if (name == "resources/list") {
        return make_result(*id, "{\"resources\":[]}");
    }
    if (name == "resources/templates/list") {
        return make_result(*id, "{\"resourceTemplates\":[]}");
    }
    if (name == "prompts/list") {
        return make_result(*id, "{\"prompts\":[]}");
    }
    return make_error(*id, -32601, "method not found: " + name);
}

} // namespace

std::optional<std::string> handle_mcp_message(const std::string& message)
{
    JsonValue request;
    JsonParser parser(message);
    if (!parser.parse(request)) {
        return make_error(JsonValue{}, -32700, "parse error: invalid JSON");
    }
    if (request.type == JsonValue::Type::array_t) {
        // JSON-RPC batching (pre-2025-06-18 MCP clients).
        std::string responses = "[";
        auto any = false;
        for (const auto& item : request.items) {
            if (item.type != JsonValue::Type::object_t) {
                continue;
            }
            if (auto response = handle_request_object(item)) {
                if (!any) {
                    any = true;
                } else {
                    responses += ',';
                }
                responses += *response;
            }
        }
        responses += ']';
        if (!any) {
            return std::nullopt;
        }
        return responses;
    }
    if (request.type != JsonValue::Type::object_t) {
        return make_error(JsonValue{}, -32600, "invalid request: expected JSON object");
    }
    return handle_request_object(request);
}

int run_mcp_server()
{
    std::cerr << soff::product_name() << " " << soff::version()
              << " mcp: serving stdio (newline-delimited JSON-RPC 2.0)" << std::endl;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.find_first_not_of(" \t") == std::string::npos) {
            continue;
        }
        if (auto response = handle_mcp_message(line)) {
            std::cout << *response << '\n' << std::flush;
        }
    }
    return 0;
}

} // namespace soff::cli
