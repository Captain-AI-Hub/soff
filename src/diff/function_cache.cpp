#include "soff/diff/function_cache.hpp"

#include "soff/core/perf.hpp"

#include <charconv>
#include <cctype>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

namespace soff::diff {
namespace {

bool is_identifier(std::string_view value)
{
    if (value.empty()) {
        return false;
    }

    const auto first = static_cast<unsigned char>(value.front());
    if (!std::isalpha(first) && value.front() != '_') {
        return false;
    }

    for (const char ch : value.substr(1)) {
        const auto byte = static_cast<unsigned char>(ch);
        if (!std::isalnum(byte) && ch != '_') {
            return false;
        }
    }
    return true;
}

std::string schema_prefix(std::string_view schema_name)
{
    if (schema_name.empty() || schema_name == "main") {
        return {};
    }
    if (!is_identifier(schema_name)) {
        throw std::runtime_error("invalid schema name: " + std::string(schema_name));
    }
    return std::string(schema_name) + '.';
}

Address parse_address_or_zero(const std::string& text)
{
    if (text.empty()) {
        return 0;
    }

    Address address = 0;
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, address, 10);
    if (result.ec == std::errc{} && result.ptr == end) {
        return address;
    }

    if (text.front() == '-' || text.front() == '+') {
        throw std::runtime_error("invalid address text: " + text);
    }

    try {
        std::size_t consumed = 0;
        address = static_cast<Address>(std::stoull(text, &consumed, 0));
        if (consumed != text.size()) {
            throw std::runtime_error("invalid address text: " + text);
        }
    } catch (const std::exception&) {
        throw std::runtime_error("invalid address text: " + text);
    }
    return address;
}

int parse_int_or_zero(const std::string& text)
{
    if (text.empty()) {
        return 0;
    }

    int value = 0;
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, value, 10);
    if (result.ec == std::errc{} && result.ptr == end) {
        return value;
    }

    try {
        std::size_t consumed = 0;
        const auto wide_value = std::stoll(text, &consumed, 0);
        if (consumed != text.size() || wide_value < std::numeric_limits<int>::min()
            || wide_value > std::numeric_limits<int>::max()) {
            throw std::runtime_error("invalid integer text: " + text);
        }
        return static_cast<int>(wide_value);
    } catch (const std::exception&) {
        throw std::runtime_error("invalid integer text: " + text);
    }
}

std::string function_count_sql(const std::string& prefix)
{
    return "select count(*) from " + prefix + "functions";
}

std::string function_select_sql(const std::string& prefix)
{
    // Large text columns (clean_assembly/clean_pseudo) are intentionally not
    // cached; FunctionTextResolver fetches them lazily for the few functions
    // that actually need them.
    return "select coalesce(address, ''), coalesce(name, ''), coalesce(mangled_function, ''), "
           "coalesce(pseudocode_primes, ''), coalesce(bytes_hash, ''), "
           "coalesce(md_index, ''), coalesce(constants, ''), coalesce(source_file, ''), "
           "coalesce(nodes, ''), coalesce(edges, ''), coalesce(instructions, ''), "
           "coalesce(size, ''), coalesce(constants_count, '') "
           "from " + prefix + "functions";
}

} // namespace

FunctionCache load_function_cache(db::Database& database, std::string_view schema_name)
{
    soff::perf::ScopedTimer timer("diff.function_cache.load");

    const auto prefix = schema_prefix(schema_name);
    const auto function_count = database.query_int(function_count_sql(prefix));

    FunctionCache cache;
    if (function_count > 0) {
        const auto reserve_count = static_cast<std::size_t>(function_count);
        cache.by_address.reserve(reserve_count);
        cache.by_name.reserve(reserve_count);
        cache.by_mangled_name.reserve(reserve_count);
    }

    auto statement = database.prepare(function_select_sql(prefix));
    while (statement.step()) {
        CachedFunction function;
        function.address = parse_address_or_zero(statement.column_text(0));
        function.name = statement.column_text(1);
        function.mangled_function = statement.column_text(2);
        function.pseudocode_primes = statement.column_text(3);
        function.bytes_hash = statement.column_text(4);
        function.md_index = statement.column_text(5);
        function.constants = statement.column_text(6);
        function.source_file = statement.column_text(7);
        function.nodes = parse_int_or_zero(statement.column_text(8));
        function.edges = parse_int_or_zero(statement.column_text(9));
        function.instructions = parse_int_or_zero(statement.column_text(10));
        function.size = parse_int_or_zero(statement.column_text(11));
        function.constants_count = parse_int_or_zero(statement.column_text(12));

        const auto address = function.address;
        const auto name = function.name;
        const auto mangled_name = function.mangled_function;
        cache.by_address.emplace(address, std::move(function));
        if (!name.empty()) {
            cache.by_name[name].push_back(address);
        }
        if (!mangled_name.empty()) {
            cache.by_mangled_name[mangled_name].push_back(address);
        }
    }

    return cache;
}

} // namespace soff::diff
