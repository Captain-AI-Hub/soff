#include "soff/core/version.hpp"
#include "soff/db/database.hpp"
#include "soff/db/result_repository.hpp"
#include "soff/db/repository.hpp"
#include "soff/diff/ml_features.hpp"
#include "soff/diff/patch_diff.hpp"
#include "soff/diff/session.hpp"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

void print_usage()
{
    std::cout
        << soff::product_name() << " " << soff::version() << '\n'
        << "usage:\n"
        << "  soff_cli\n"
        << "  soff_cli init-db <path> [--diaphora-version]\n"
        << "  soff_cli inspect-db <path> [--top <n>] [--heuristics] [--nonzero-heuristics] "
           "[--unmatched] [--summary-json] [--program-data] [--field-stats]\n"
        << "  soff_cli diff-db <main.sqlite> <diff.sqlite> --out <result.soff> "
           "[--unreliable] [--experimental] [--no-slow] [--max-rows <n>] [--timeout <seconds>]\n"
        << "  soff_cli batch-diff <manifest.json> [--out <dir>]\n"
        << "  soff_cli parity-report <manifest.json>\n"
        << "  soff_cli patch-diff <result.soff> <main.sqlite> <diff.sqlite> [--json]\n"
        << "  soff_cli ml-export <result.soff> <main.sqlite> <diff.sqlite> --out <file.csv|.json>\n"
        << "  soff_cli check-m5-fixture <fixture.json> [--out <result.soff>]\n";
}

bool parse_diff_options(int argc, char** argv, std::filesystem::path& output, soff::diff::DiffSessionOptions& options)
{
    if (argc < 6 || std::string_view(argv[4]) != "--out") {
        return false;
    }

    output = argv[5];
    for (int i = 6; i < argc; ++i) {
        const std::string_view option(argv[i]);
        if (option == "--unreliable") {
            options.sql.enable_unreliable = true;
        } else if (option == "--experimental") {
            options.sql.enable_experimental = true;
        } else if (option == "--no-slow") {
            options.sql.enable_slow = false;
        } else if (option == "--max-rows" && i + 1 < argc) {
            options.sql.max_processed_rows = static_cast<std::size_t>(std::stoull(argv[++i]));
        } else if (option == "--timeout" && i + 1 < argc) {
            options.sql.timeout_seconds = static_cast<std::uint32_t>(std::stoul(argv[++i]));
        } else {
            return false;
        }
    }
    return true;
}

bool has_table(const std::filesystem::path& path, std::string_view table_name)
{
    soff::db::Database database;
    database.open(path);
    const auto sql = "select count(*) from sqlite_master where type = 'table' and name = '" + std::string(table_name) + "'";
    return database.query_int(sql) > 0;
}

std::string validate_export_database(const std::filesystem::path& path, std::string_view label)
{
    if (path.empty()) {
        return std::string(label) + " database path is empty";
    }
    if (!std::filesystem::exists(path)) {
        return std::string(label) + " database does not exist: " + path.string();
    }
    if (std::filesystem::file_size(path) == 0) {
        return std::string(label) + " database is empty: " + path.string();
    }
    try {
        soff::db::Database database;
        database.open(path);
        for (const auto* table : {"version", "program", "functions"}) {
            const auto sql = "select count(*) from sqlite_master where type = 'table' and name = '" + std::string(table) + "'";
            if (database.query_int(sql) == 0) {
                return std::string(label) + " database is missing table '" + table + "': " + path.string();
            }
        }
        if (database.query_int("select count(*) from functions") == 0) {
            return std::string(label) + " database has no exported functions: " + path.string();
        }
    } catch (const std::exception& error) {
        return std::string(label) + " database is not a readable SOFF/Diaphora export: " + error.what();
    }
    return "";
}

std::string json_escape(std::string_view text)
{
    std::ostringstream out;
    for (const char ch : text) {
        switch (ch) {
        case '\\':
            out << "\\\\";
            break;
        case '"':
            out << "\\\"";
            break;
        case '\n':
            out << "\\n";
            break;
        case '\r':
            out << "\\r";
            break;
        case '\t':
            out << "\\t";
            break;
        default:
            out << ch;
            break;
        }
    }
    return out.str();
}

std::int64_t table_count(soff::db::Database& database, std::string_view table_name)
{
    return database.query_int("select count(*) from " + std::string(table_name));
}

std::int64_t object_count(soff::db::Database& database, std::string_view object_type)
{
    return database.query_int(
        "select count(*) from sqlite_master where type = '" + std::string(object_type) + "'");
}

struct InspectOptions
{
    std::size_t top_count = 0;
    bool show_heuristics = false;
    bool show_nonzero_heuristics = false;
    bool show_unmatched = false;
    bool show_summary_json = false;
    bool show_program_data = false;
    bool show_field_stats = false;
};

InspectOptions parse_inspect_options(int argc, char** argv)
{
    InspectOptions options;
    for (int i = 3; i < argc; ++i) {
        const std::string_view option(argv[i]);
        if (option == "--top" && i + 1 < argc) {
            options.top_count = static_cast<std::size_t>(std::stoull(argv[++i]));
        } else if (option == "--heuristics") {
            options.show_heuristics = true;
        } else if (option == "--nonzero-heuristics") {
            options.show_nonzero_heuristics = true;
        } else if (option == "--unmatched") {
            options.show_unmatched = true;
        } else if (option == "--summary-json") {
            options.show_summary_json = true;
        } else if (option == "--program-data") {
            options.show_program_data = true;
        } else if (option == "--field-stats") {
            options.show_field_stats = true;
        } else {
            throw std::runtime_error("invalid inspect-db options");
        }
    }
    return options;
}

std::string read_file_text(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to open fixture: " + path.string());
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::filesystem::path resolve_existing_relative_path(
    const std::filesystem::path& path,
    const std::filesystem::path& executable_path)
{
    if (path.empty() || path.is_absolute() || std::filesystem::exists(path)) {
        return path;
    }

    std::vector<std::filesystem::path> roots;
    roots.push_back(std::filesystem::current_path());
    if (!executable_path.empty()) {
        roots.push_back(std::filesystem::absolute(executable_path).parent_path());
    }

    for (auto root : roots) {
        for (int depth = 0; depth < 8 && !root.empty(); ++depth) {
            const auto candidate = root / path;
            if (std::filesystem::exists(candidate)) {
                return candidate;
            }
            root = root.parent_path();
        }
    }
    return path;
}

std::string unescape_json_string(std::string text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\\' || i + 1 >= text.size()) {
            out.push_back(text[i]);
            continue;
        }
        const char escaped = text[++i];
        switch (escaped) {
        case '\\':
        case '"':
        case '/':
            out.push_back(escaped);
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
        default:
            out.push_back(escaped);
            break;
        }
    }
    return out;
}

std::string extract_object(const std::string& text, std::string_view key)
{
    const auto key_text = "\"" + std::string(key) + "\"";
    const auto key_pos = text.find(key_text);
    if (key_pos == std::string::npos) {
        return "";
    }
    const auto open = text.find('{', key_pos + key_text.size());
    if (open == std::string::npos) {
        return "";
    }

    bool in_string = false;
    bool escaped = false;
    int depth = 0;
    for (std::size_t i = open; i < text.size(); ++i) {
        const char ch = text[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (in_string && ch == '\\') {
            escaped = true;
            continue;
        }
        if (ch == '"') {
            in_string = !in_string;
            continue;
        }
        if (in_string) {
            continue;
        }
        if (ch == '{') {
            ++depth;
        } else if (ch == '}') {
            --depth;
            if (depth == 0) {
                return text.substr(open, i - open + 1);
            }
        }
    }
    return "";
}

std::string extract_json_string(const std::string& text, std::string_view key)
{
    const std::regex pattern("\"" + std::string(key) + "\"\\s*:\\s*\"((?:\\\\.|[^\"\\\\])*)\"");
    std::smatch match;
    if (!std::regex_search(text, match, pattern)) {
        return "";
    }
    return unescape_json_string(match[1].str());
}

std::int64_t extract_json_integer(const std::string& text, std::string_view key, std::int64_t fallback = 0)
{
    const std::regex pattern("\"" + std::string(key) + R"("\s*:\s*(-?[0-9]+))");
    std::smatch match;
    if (!std::regex_search(text, match, pattern)) {
        return fallback;
    }
    return std::stoll(match[1].str());
}

struct FieldStats
{
    std::map<std::string, std::int64_t> values;
};

FieldStats query_field_stats(soff::db::Database& database)
{
    const auto rows = database.query_rows(
        "select count(*), "
        "sum(length(coalesce(names, '')) > 0 and coalesce(names, '') != '[]'), "
        "sum(length(coalesce(assembly, '')) > 0), "
        "sum(length(coalesce(clean_assembly, '')) > 0), "
        "sum(length(coalesce(assembly_addrs, '')) > 0), "
        "sum(coalesce(bytes_sum, 0) > 0), "
        "sum(function_flags is not null), "
        "sum(length(coalesce(mangled_function, '')) > 0), "
        "sum(length(coalesce(prototype, '')) > 0), "
        "sum(length(coalesce(prototype2, '')) > 0), "
        "sum(length(coalesce(comment, '')) > 0), "
        "sum(cast(coalesce(md_index, '0') as real) > 0), "
        "sum(coalesce(indegree, 0) > 0), "
        "sum(coalesce(outdegree, 0) > 0), "
        "sum(coalesce(cyclomatic_complexity, 0) > 0), "
        "sum(coalesce(strongly_connected, 0) > 0), "
        "sum(coalesce(loops, 0) > 0), "
        "sum(length(coalesce(tarjan_topological_sort, '')) > 0 and coalesce(tarjan_topological_sort, '') != '[]'), "
        "sum(length(coalesce(strongly_connected_spp, '')) > 0 and coalesce(strongly_connected_spp, '') != '1'), "
        "sum(length(coalesce(mnemonics_spp, '')) > 0 and coalesce(mnemonics_spp, '') != '1'), "
        "sum(length(coalesce(switches, '')) > 0 and coalesce(switches, '') != '[]'), "
        "sum(length(coalesce(pseudocode, '')) > 0), "
        "sum(length(coalesce(clean_pseudo, '')) > 0), "
        "sum(coalesce(pseudocode_lines, 0) > 0), "
        "sum(length(coalesce(pseudocode_hash1, '')) > 0), "
        "sum(length(coalesce(pseudocode_hash2, '')) > 0), "
        "sum(length(coalesce(pseudocode_hash3, '')) > 0), "
        "sum(length(coalesce(pseudocode_primes, '')) > 0), "
        "sum(length(coalesce(microcode, '')) > 0), "
        "sum(length(coalesce(clean_microcode, '')) > 0), "
        "sum(length(coalesce(microcode_spp, '')) > 0 and coalesce(microcode_spp, '') != '1'), "
        "sum(length(coalesce(kgh_hash, '')) > 0 and coalesce(kgh_hash, '') != '1'), "
        "sum(length(coalesce(source_file, '')) > 0), "
        "sum(length(coalesce(userdata, '')) > 0), "
        "sum(coalesce(export_time, 0) > 0) "
        "from functions");
    if (rows.empty()) {
        throw std::runtime_error("failed to query field stats");
    }
    static const char* names[] = {
        "functions",
        "names_nonempty",
        "assembly",
        "clean_assembly",
        "assembly_addrs",
        "bytes_sum",
        "function_flags",
        "mangled_function",
        "prototype",
        "prototype2",
        "comment",
        "md_index_nonzero",
        "indegree_nonzero",
        "outdegree_nonzero",
        "cyclomatic_nonzero",
        "strongly_connected_nonzero",
        "loops_nonzero",
        "tarjan_topological_sort",
        "strongly_connected_spp_nontrivial",
        "mnemonics_spp_nontrivial",
        "switches_nonempty",
        "pseudocode",
        "clean_pseudo",
        "pseudocode_lines",
        "pseudocode_hash1",
        "pseudocode_hash2",
        "pseudocode_hash3",
        "pseudocode_primes",
        "microcode",
        "clean_microcode",
        "microcode_spp_nontrivial",
        "kgh_hash_nontrivial",
        "source_file",
        "userdata",
        "export_time",
    };
    FieldStats stats;
    const auto& row = rows.front();
    for (std::size_t i = 0; i < row.size() && i < (sizeof(names) / sizeof(names[0])); ++i) {
        stats.values[names[i]] = row[i].empty() ? 0 : std::stoll(row[i]);
    }

    const auto instruction_rows = database.query_rows(
        "select count(*), "
        "sum(length(coalesce(comment1, '')) > 0), "
        "sum(length(coalesce(comment2, '')) > 0), "
        "sum(length(coalesce(operand_names, '')) > 0), "
        "sum(length(coalesce(name, '')) > 0), "
        "sum(length(coalesce(type, '')) > 0), "
        "sum(length(coalesce(pseudocomment, '')) > 0), "
        "sum(length(coalesce(pseudoitp, '')) > 0) "
        "from instructions");
    if (!instruction_rows.empty()) {
        const auto& instruction_row = instruction_rows.front();
        static const char* instruction_names[] = {
            "instruction_rows",
            "instruction_comment1",
            "instruction_comment2",
            "instruction_operand_names",
            "instruction_names",
            "instruction_types",
            "instruction_pseudocomments",
            "instruction_pseudoitp",
        };
        for (std::size_t i = 0; i < instruction_row.size()
             && i < (sizeof(instruction_names) / sizeof(instruction_names[0])); ++i) {
            stats.values[instruction_names[i]] = instruction_row[i].empty() ? 0 : std::stoll(instruction_row[i]);
        }
    }
    return stats;
}

std::map<std::string, std::string> program_data_map(const soff::ProgramSnapshot& snapshot)
{
    std::map<std::string, std::string> values;
    for (const auto& item : snapshot.program_data) {
        values[item.name] = item.value;
    }
    return values;
}

void compare_value(
    std::vector<std::string>& failures,
    std::string_view label,
    std::int64_t expected,
    std::int64_t actual)
{
    if (expected != actual) {
        std::ostringstream out;
        out << label << " expected=" << expected << " actual=" << actual;
        failures.push_back(out.str());
    }
}

void check_export_fixture(
    const std::string& fixture_section,
    const std::filesystem::path& db_path,
    std::string_view label,
    std::vector<std::string>& failures)
{
    if (const auto error = validate_export_database(db_path, label); !error.empty()) {
        failures.push_back(error);
        return;
    }

    soff::SnapshotRepository repository;
    const auto snapshot = repository.load(db_path);
    const auto program_data = program_data_map(snapshot);
    soff::db::Database database;
    database.open(db_path);

    const auto export_total = program_data.find("export.total_functions") != program_data.end()
        ? std::stoll(program_data.at("export.total_functions"))
        : -1;
    const auto export_exported = program_data.find("export.exported_functions") != program_data.end()
        ? std::stoll(program_data.at("export.exported_functions"))
        : -1;
    const auto export_skipped = program_data.find("export.skipped_functions") != program_data.end()
        ? std::stoll(program_data.at("export.skipped_functions"))
        : -1;

    compare_value(failures, std::string(label) + ".total_functions", extract_json_integer(fixture_section, "total_functions"), export_total);
    compare_value(failures, std::string(label) + ".exported_functions", extract_json_integer(fixture_section, "exported_functions"), export_exported);
    compare_value(failures, std::string(label) + ".skipped_functions", extract_json_integer(fixture_section, "skipped_functions"), export_skipped);
    compare_value(failures, std::string(label) + ".instructions", extract_json_integer(fixture_section, "instructions"), table_count(database, "instructions"));
    compare_value(failures, std::string(label) + ".basic_blocks", extract_json_integer(fixture_section, "basic_blocks"), table_count(database, "basic_blocks"));
    compare_value(failures, std::string(label) + ".bb_relations", extract_json_integer(fixture_section, "bb_relations"), table_count(database, "bb_relations"));
    compare_value(failures, std::string(label) + ".bb_instructions", extract_json_integer(fixture_section, "bb_instructions"), table_count(database, "bb_instructions"));
    compare_value(failures, std::string(label) + ".function_bblocks", extract_json_integer(fixture_section, "function_bblocks"), table_count(database, "function_bblocks"));
    compare_value(failures, std::string(label) + ".callgraph", extract_json_integer(fixture_section, "callgraph"), table_count(database, "callgraph"));
    compare_value(failures, std::string(label) + ".constants", extract_json_integer(fixture_section, "constants"), table_count(database, "constants"));

    const auto field_fill = extract_object(fixture_section, "field_fill");
    const auto actual_fields = query_field_stats(database);
    for (const auto& [field, actual] : actual_fields.values) {
        const auto expected = extract_json_integer(field_fill, field, -1);
        if (expected >= 0) {
            compare_value(failures, std::string(label) + ".field_fill." + field, expected, actual);
        }
    }
}

void check_heuristic_fixture(
    const std::string& fixture_text,
    const soff::db::DiffResultSet& results,
    std::vector<std::string>& failures)
{
    const auto array_pos = fixture_text.find("\"native_field_heuristics\"");
    const auto fallback_pos = fixture_text.find("\"nonzero_heuristics\"");
    const auto start_pos = array_pos != std::string::npos ? array_pos : fallback_pos;
    if (start_pos == std::string::npos) {
        return;
    }
    const auto array_start = fixture_text.find('[', start_pos);
    const auto array_end = fixture_text.find(']', array_start);
    if (array_start == std::string::npos || array_end == std::string::npos) {
        return;
    }
    const auto array_text = fixture_text.substr(array_start, array_end - array_start + 1);

    std::map<std::string, soff::db::HeuristicStat> actual_by_name;
    for (const auto& stats : results.heuristic_stats) {
        actual_by_name[stats.name] = stats;
    }

    std::size_t pos = 0;
    while (true) {
        const auto open = array_text.find('{', pos);
        if (open == std::string::npos) {
            break;
        }
        const auto close = array_text.find('}', open);
        if (close == std::string::npos) {
            break;
        }
        const auto object = array_text.substr(open, close - open + 1);
        const auto name = extract_json_string(object, "name");
        if (!name.empty()) {
            const auto it = actual_by_name.find(name);
            if (it == actual_by_name.end()) {
                failures.push_back("heuristic missing: " + name);
            } else {
                compare_value(failures, "heuristic." + name + ".candidates", extract_json_integer(object, "candidates"), static_cast<std::int64_t>(it->second.candidates));
                compare_value(failures, "heuristic." + name + ".accepted", extract_json_integer(object, "accepted"), static_cast<std::int64_t>(it->second.accepted));
                compare_value(failures, "heuristic." + name + ".rejected", extract_json_integer(object, "rejected"), static_cast<std::int64_t>(it->second.rejected));
                compare_value(failures, "heuristic." + name + ".skipped", extract_json_integer(object, "skipped"), static_cast<std::int64_t>(it->second.skipped));
                compare_value(failures, "heuristic." + name + ".multimatches", extract_json_integer(object, "multimatches"), static_cast<std::int64_t>(it->second.multimatches));
            }
        }
        pos = close + 1;
    }
}

void check_python_diaphora_baseline(
    const std::string& fixture_text,
    const std::filesystem::path& executable_path,
    std::vector<std::string>& failures)
{
    const auto baseline = extract_object(fixture_text, "python_diaphora_baseline");
    if (baseline.empty()) {
        return;
    }

    const auto result_db = resolve_existing_relative_path(extract_json_string(baseline, "result_db"), executable_path);
    if (result_db.empty() || !std::filesystem::exists(result_db)) {
        failures.push_back("python_diaphora_baseline.result_db missing: " + result_db.string());
        return;
    }

    try {
        const auto summary = soff::db::ResultRepository{}.summarize(result_db);
        compare_value(failures, "python_diaphora.best", extract_json_integer(baseline, "best"), static_cast<std::int64_t>(summary.best));
        compare_value(failures, "python_diaphora.partial", extract_json_integer(baseline, "partial"), static_cast<std::int64_t>(summary.partial));
        compare_value(failures, "python_diaphora.unreliable", extract_json_integer(baseline, "unreliable"), static_cast<std::int64_t>(summary.unreliable));
        compare_value(failures, "python_diaphora.multimatch", extract_json_integer(baseline, "multimatch"), static_cast<std::int64_t>(summary.multimatch));
        compare_value(failures, "python_diaphora.unmatched_primary", extract_json_integer(baseline, "unmatched_primary"), static_cast<std::int64_t>(summary.unmatched_primary));
        compare_value(failures, "python_diaphora.unmatched_secondary", extract_json_integer(baseline, "unmatched_secondary"), static_cast<std::int64_t>(summary.unmatched_secondary));
    } catch (const std::exception& error) {
        failures.push_back("python_diaphora_baseline unreadable: " + std::string(error.what()));
    }
}

int check_m5_fixture(int argc, char** argv)
{
    if (argc != 3 && argc != 5) {
        print_usage();
        return 2;
    }
    const std::filesystem::path fixture_path = resolve_existing_relative_path(argv[2], argv[0]);
    std::filesystem::path override_output;
    if (argc == 5) {
        if (std::string_view(argv[3]) != "--out") {
            print_usage();
            return 2;
        }
        override_output = argv[4];
    }

    const auto fixture_text = read_file_text(fixture_path);
    const auto exports = extract_object(fixture_text, "exports");
    const auto primary = extract_object(exports, "primary");
    const auto secondary = extract_object(exports, "secondary");
    const auto diff_summary = extract_object(fixture_text, "diff_summary");
    const std::filesystem::path main_db = extract_json_string(fixture_text, "main_db");
    const std::filesystem::path diff_db = extract_json_string(fixture_text, "diff_db");
    const std::filesystem::path result_db = override_output.empty()
        ? std::filesystem::path(extract_json_string(fixture_text, "result_db"))
        : override_output;

    std::vector<std::string> failures;
    check_export_fixture(primary, main_db, "primary", failures);
    check_export_fixture(secondary, diff_db, "secondary", failures);
    if (!failures.empty()) {
        std::cout << "fixture=fail path=" << fixture_path.string() << " result=" << result_db.string() << '\n';
        for (const auto& failure : failures) {
            std::cout << "failure " << failure << '\n';
        }
        return 1;
    }

    soff::diff::DiffSessionOptions options;
    options.sql.enable_unreliable = true;
    options.sql.max_processed_rows = 200000;
    options.sql.timeout_seconds = 120;
    const auto summary = soff::diff::DiffSession{options}.run_all(main_db, diff_db, result_db);
    compare_value(failures, "diff.heuristics", extract_json_integer(diff_summary, "heuristics"), static_cast<std::int64_t>(summary.heuristics));
    compare_value(failures, "diff.candidates", extract_json_integer(diff_summary, "candidates"), static_cast<std::int64_t>(summary.candidates));
    compare_value(failures, "diff.accepted", extract_json_integer(diff_summary, "accepted"), static_cast<std::int64_t>(summary.accepted));
    compare_value(failures, "diff.multimatches", extract_json_integer(diff_summary, "multimatches"), static_cast<std::int64_t>(summary.multimatches));
    compare_value(failures, "diff.best", extract_json_integer(diff_summary, "best"), static_cast<std::int64_t>(summary.results.best));
    compare_value(failures, "diff.partial", extract_json_integer(diff_summary, "partial"), static_cast<std::int64_t>(summary.results.partial));
    compare_value(failures, "diff.unreliable", extract_json_integer(diff_summary, "unreliable"), static_cast<std::int64_t>(summary.results.unreliable));
    compare_value(failures, "diff.result_multimatch", extract_json_integer(diff_summary, "result_multimatch"), static_cast<std::int64_t>(summary.results.multimatch));
    compare_value(failures, "diff.unmatched_primary", extract_json_integer(diff_summary, "unmatched_primary"), static_cast<std::int64_t>(summary.results.unmatched_primary));
    compare_value(failures, "diff.unmatched_secondary", extract_json_integer(diff_summary, "unmatched_secondary"), static_cast<std::int64_t>(summary.results.unmatched_secondary));
    compare_value(failures, "diff.row_limited", extract_json_integer(diff_summary, "row_limited"), static_cast<std::int64_t>(summary.row_limited_heuristics));
    compare_value(failures, "diff.timed_out", extract_json_integer(diff_summary, "timed_out"), static_cast<std::int64_t>(summary.timed_out_heuristics));
    compare_value(failures, "diff.cancelled", extract_json_integer(diff_summary, "cancelled"), static_cast<std::int64_t>(summary.cancelled_heuristics));

    const auto result_set = soff::db::ResultRepository{}.load(result_db);
    check_heuristic_fixture(fixture_text, result_set, failures);
    check_python_diaphora_baseline(fixture_text, argv[0], failures);

    if (!failures.empty()) {
        std::cout << "fixture=fail path=" << fixture_path.string() << " result=" << result_db.string() << '\n';
        for (const auto& failure : failures) {
            std::cout << "failure " << failure << '\n';
        }
        return 1;
    }

    std::cout << "fixture=ok path=" << fixture_path.string()
              << " result=" << result_db.string()
              << " best=" << summary.results.best
              << " partial=" << summary.results.partial
              << " unreliable=" << summary.results.unreliable
              << " multimatch=" << summary.results.multimatch
              << " unmatched=" << summary.results.unmatched_primary << "/" << summary.results.unmatched_secondary << '\n';
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc == 1) {
            print_usage();
            return 0;
        }

        const std::string_view command(argv[1]);
        soff::SnapshotRepository repository;

        if (command == "init-db" && (argc == 3 || argc == 4)) {
            if (argc == 4 && std::string_view(argv[3]) != "--diaphora-version") {
                print_usage();
                return 2;
            }
            const auto version_policy = argc == 4
                ? soff::SnapshotVersionPolicy::diaphora_34
                : soff::SnapshotVersionPolicy::soff;
            soff::SnapshotRepository init_repository(version_policy);
            init_repository.create_schema(argv[2]);
            init_repository.create_indices(argv[2]);
            std::cout << "created " << std::filesystem::path(argv[2]).string() << '\n';
            return 0;
        }

        if (command == "inspect-db" && argc >= 3) {
            const std::filesystem::path path(argv[2]);
            const auto inspect_options = parse_inspect_options(argc, argv);
            if (has_table(path, "results") && has_table(path, "config")) {
                soff::db::Database database;
                database.open(path);
                soff::db::ResultRepository result_repository;
                const auto results = result_repository.load(path);
                const auto summary = result_repository.summarize(path);
                std::cout << "type=result"
                          << " main_db=" << results.main_db
                          << " diff_db=" << results.diff_db
                          << " version=" << results.version
                          << " tables=" << object_count(database, "table")
                          << " indexes=" << object_count(database, "index")
                          << " heuristics=" << results.heuristic_stats.size()
                          << " best=" << summary.best
                          << " partial=" << summary.partial
                          << " unreliable=" << summary.unreliable
                          << " multimatch=" << summary.multimatch
                          << " unmatched_primary=" << summary.unmatched_primary
                          << " unmatched_secondary=" << summary.unmatched_secondary << '\n';
                if (inspect_options.show_summary_json) {
                    std::cout << "summary_json {"
                              << "\"type\":\"result\","
                              << "\"main_db\":\"" << json_escape(results.main_db) << "\","
                              << "\"diff_db\":\"" << json_escape(results.diff_db) << "\","
                              << "\"version\":\"" << json_escape(results.version) << "\","
                              << "\"heuristics\":" << results.heuristic_stats.size() << ","
                              << "\"best\":" << summary.best << ","
                              << "\"partial\":" << summary.partial << ","
                              << "\"unreliable\":" << summary.unreliable << ","
                              << "\"multimatch\":" << summary.multimatch << ","
                              << "\"unmatched_primary\":" << summary.unmatched_primary << ","
                              << "\"unmatched_secondary\":" << summary.unmatched_secondary
                              << "}\n";
                }
                if (inspect_options.top_count != 0) {
                    std::size_t emitted = 0;
                    for (const auto& match : results.matches) {
                        if (emitted++ >= inspect_options.top_count) {
                            break;
                        }
                        std::cout << "match"
                                  << " type=" << soff::db::result_kind_name(match.kind)
                                  << " line=" << match.line
                                  << " primary=" << match.primary
                                  << " primary_name=" << match.primary_name
                                  << " secondary=" << match.secondary
                                  << " secondary_name=" << match.secondary_name
                                  << " ratio=" << std::fixed << std::setprecision(6) << match.ratio
                                  << " nodes1=" << match.primary_nodes
                                  << " nodes2=" << match.secondary_nodes
                                  << " description=" << match.description << '\n';
                    }
                }
                if (inspect_options.show_unmatched) {
                    for (const auto& unmatched : results.unmatched) {
                        std::cout << "unmatched"
                                  << " type=" << soff::db::unmatched_kind_name(unmatched.kind)
                                  << " line=" << unmatched.line
                                  << " address=" << unmatched.address
                                  << " name=" << unmatched.name << '\n';
                    }
                }
                if (inspect_options.show_heuristics || inspect_options.show_nonzero_heuristics) {
                    for (const auto& stats : results.heuristic_stats) {
                        if (inspect_options.show_nonzero_heuristics
                            && stats.candidates == 0
                            && stats.accepted == 0
                            && stats.rejected == 0
                            && stats.skipped == 0
                            && stats.multimatches == 0) {
                            continue;
                        }
                        std::cout << "heuristic"
                                  << " line=" << stats.line
                                  << " name=" << stats.name
                                  << " candidates=" << stats.candidates
                                  << " accepted=" << stats.accepted
                                  << " rejected=" << stats.rejected
                                  << " skipped=" << stats.skipped
                                  << " multimatches=" << stats.multimatches
                                  << " row_limit_hit=" << (stats.row_limit_hit ? "yes" : "no")
                                  << " timeout_hit=" << (stats.timeout_hit ? "yes" : "no")
                                  << " cancelled=" << (stats.cancelled ? "yes" : "no") << '\n';
                    }
                }
                return 0;
            }

            if (const auto error = validate_export_database(path, "inspect"); !error.empty()) {
                throw std::runtime_error(error);
            }
            const auto snapshot = repository.load(path);
            soff::db::Database database;
            database.open(path);
            const auto find_program_data = [&](std::string_view name) {
                for (const auto& item : snapshot.program_data) {
                    if (item.name == name) {
                        return item.value;
                    }
                }
                return std::string{};
            };
            std::cout << "arch=" << snapshot.architecture
                      << " version=" << database.query_text("select value from version limit 1")
                      << " tables=" << object_count(database, "table")
                      << " indexes=" << object_count(database, "index")
                      << " program_data=" << snapshot.program_data.size()
                      << " export_total=" << find_program_data("export.total_functions")
                      << " export_exported=" << find_program_data("export.exported_functions")
                      << " export_skipped=" << find_program_data("export.skipped_functions")
                      << " functions=" << snapshot.functions.size()
                      << " instructions=" << table_count(database, "instructions")
                      << " basic_blocks=" << table_count(database, "basic_blocks")
                      << " bb_relations=" << table_count(database, "bb_relations")
                      << " bb_instructions=" << table_count(database, "bb_instructions")
                      << " function_bblocks=" << table_count(database, "function_bblocks")
                      << " callgraph=" << table_count(database, "callgraph")
                      << " constants=" << table_count(database, "constants") << '\n';
            if (inspect_options.show_summary_json) {
                std::cout << "summary_json {"
                          << "\"type\":\"export\","
                          << "\"arch\":\"" << json_escape(snapshot.architecture) << "\","
                          << "\"version\":\"" << json_escape(database.query_text("select value from version limit 1")) << "\","
                          << "\"program_data\":" << snapshot.program_data.size() << ","
                          << "\"export_total\":\"" << json_escape(find_program_data("export.total_functions")) << "\","
                          << "\"export_exported\":\"" << json_escape(find_program_data("export.exported_functions")) << "\","
                          << "\"export_skipped\":\"" << json_escape(find_program_data("export.skipped_functions")) << "\","
                          << "\"functions\":" << snapshot.functions.size() << ","
                          << "\"instructions\":" << table_count(database, "instructions") << ","
                          << "\"basic_blocks\":" << table_count(database, "basic_blocks") << ","
                          << "\"bb_relations\":" << table_count(database, "bb_relations") << ","
                          << "\"bb_instructions\":" << table_count(database, "bb_instructions") << ","
                          << "\"function_bblocks\":" << table_count(database, "function_bblocks") << ","
                          << "\"callgraph\":" << table_count(database, "callgraph") << ","
                          << "\"constants\":" << table_count(database, "constants")
                          << "}\n";
            }
            if (inspect_options.show_program_data) {
                for (const auto& item : snapshot.program_data) {
                    std::cout << "program_data"
                              << " name=" << item.name
                              << " type=" << item.type
                              << " value=" << item.value << '\n';
                }
            }
            if (inspect_options.show_field_stats) {
                const auto rows = database.query_rows(
                    "select count(*), "
                    "sum(length(coalesce(names, '')) > 0 and coalesce(names, '') != '[]'), "
                    "sum(length(coalesce(assembly, '')) > 0), "
                    "sum(length(coalesce(clean_assembly, '')) > 0), "
                    "sum(length(coalesce(assembly_addrs, '')) > 0), "
                    "sum(coalesce(bytes_sum, 0) > 0), "
                    "sum(function_flags is not null), "
                    "sum(length(coalesce(mangled_function, '')) > 0), "
                    "sum(length(coalesce(prototype, '')) > 0), "
                    "sum(length(coalesce(prototype2, '')) > 0), "
                    "sum(length(coalesce(comment, '')) > 0), "
                    "sum(cast(coalesce(md_index, '0') as real) > 0), "
                    "sum(coalesce(indegree, 0) > 0), "
                    "sum(coalesce(outdegree, 0) > 0), "
                    "sum(coalesce(cyclomatic_complexity, 0) > 0), "
                    "sum(coalesce(strongly_connected, 0) > 0), "
                    "sum(coalesce(loops, 0) > 0), "
                    "sum(length(coalesce(tarjan_topological_sort, '')) > 0 and coalesce(tarjan_topological_sort, '') != '[]'), "
                    "sum(length(coalesce(strongly_connected_spp, '')) > 0 and coalesce(strongly_connected_spp, '') != '1'), "
                    "sum(length(coalesce(mnemonics_spp, '')) > 0 and coalesce(mnemonics_spp, '') != '1'), "
                    "sum(length(coalesce(switches, '')) > 0 and coalesce(switches, '') != '[]'), "
                    "sum(length(coalesce(pseudocode, '')) > 0), "
                    "sum(length(coalesce(clean_pseudo, '')) > 0), "
                    "sum(coalesce(pseudocode_lines, 0) > 0), "
                    "sum(length(coalesce(pseudocode_hash1, '')) > 0), "
                    "sum(length(coalesce(pseudocode_hash2, '')) > 0), "
                    "sum(length(coalesce(pseudocode_hash3, '')) > 0), "
                    "sum(length(coalesce(pseudocode_primes, '')) > 0), "
                    "sum(length(coalesce(microcode, '')) > 0), "
                    "sum(length(coalesce(clean_microcode, '')) > 0), "
                    "sum(length(coalesce(microcode_spp, '')) > 0 and coalesce(microcode_spp, '') != '1'), "
                    "sum(length(coalesce(kgh_hash, '')) > 0 and coalesce(kgh_hash, '') != '1'), "
                    "sum(length(coalesce(source_file, '')) > 0), "
                    "sum(length(coalesce(userdata, '')) > 0), "
                    "sum(coalesce(export_time, 0) > 0) "
                    "from functions");
                const auto instruction_rows = database.query_rows(
                    "select count(*), "
                    "sum(length(coalesce(comment1, '')) > 0), "
                    "sum(length(coalesce(comment2, '')) > 0), "
                    "sum(length(coalesce(operand_names, '')) > 0), "
                    "sum(length(coalesce(name, '')) > 0), "
                    "sum(length(coalesce(type, '')) > 0), "
                    "sum(length(coalesce(pseudocomment, '')) > 0), "
                    "sum(length(coalesce(pseudoitp, '')) > 0) "
                    "from instructions");
                if (!rows.empty()) {
                    const auto& row = rows.front();
                    std::cout << "field_stats"
                              << " functions=" << row[0]
                              << " names_nonempty=" << row[1]
                              << " assembly=" << row[2]
                              << " clean_assembly=" << row[3]
                              << " assembly_addrs=" << row[4]
                              << " bytes_sum=" << row[5]
                              << " function_flags=" << row[6]
                              << " mangled_function=" << row[7]
                              << " prototype=" << row[8]
                              << " prototype2=" << row[9]
                              << " comment=" << row[10]
                              << " md_index_nonzero=" << row[11]
                              << " indegree_nonzero=" << row[12]
                              << " outdegree_nonzero=" << row[13]
                              << " cyclomatic_nonzero=" << row[14]
                              << " strongly_connected_nonzero=" << row[15]
                              << " loops_nonzero=" << row[16]
                              << " tarjan_topological_sort=" << row[17]
                              << " strongly_connected_spp_nontrivial=" << row[18]
                              << " mnemonics_spp_nontrivial=" << row[19]
                              << " switches_nonempty=" << row[20]
                              << " pseudocode=" << row[21]
                              << " clean_pseudo=" << row[22]
                              << " pseudocode_lines=" << row[23]
                              << " pseudocode_hash1=" << row[24]
                              << " pseudocode_hash2=" << row[25]
                              << " pseudocode_hash3=" << row[26]
                              << " pseudocode_primes=" << row[27]
                              << " microcode=" << row[28]
                              << " clean_microcode=" << row[29]
                              << " microcode_spp_nontrivial=" << row[30]
                              << " kgh_hash_nontrivial=" << row[31]
                              << " source_file=" << row[32]
                              << " userdata=" << row[33]
                              << " export_time=" << row[34];
                    if (!instruction_rows.empty()) {
                        const auto& instruction_row = instruction_rows.front();
                        std::cout << " instruction_rows=" << instruction_row[0]
                                  << " instruction_comment1=" << instruction_row[1]
                                  << " instruction_comment2=" << instruction_row[2]
                                  << " instruction_operand_names=" << instruction_row[3]
                                  << " instruction_names=" << instruction_row[4]
                                  << " instruction_types=" << instruction_row[5]
                                  << " instruction_pseudocomments=" << instruction_row[6]
                                  << " instruction_pseudoitp=" << instruction_row[7];
                    }
                    std::cout << '\n';
                }
            }
            return 0;
        }

        if (command == "check-m5-fixture") {
            return check_m5_fixture(argc, argv);
        }

        if (command == "diff-db") {
            const std::filesystem::path main_db(argv[2]);
            const std::filesystem::path diff_db(argv[3]);
            std::filesystem::path out_db;
            soff::diff::DiffSessionOptions options;
            if (!parse_diff_options(argc, argv, out_db, options)) {
                print_usage();
                return 2;
            }
            if (const auto error = validate_export_database(main_db, "main"); !error.empty()) {
                throw std::runtime_error(error);
            }
            if (const auto error = validate_export_database(diff_db, "diff"); !error.empty()) {
                throw std::runtime_error(error);
            }

            const auto summary = soff::diff::DiffSession{options}.run_all(main_db, diff_db, out_db);
            std::cout << "heuristics=" << summary.heuristics
                      << " same_processor=" << (summary.same_processor ? "yes" : "no")
                      << " candidates=" << summary.candidates
                      << " accepted=" << summary.accepted
                      << " multimatches=" << summary.multimatches
                      << " best=" << summary.results.best
                      << " partial=" << summary.results.partial
                      << " unreliable=" << summary.results.unreliable
                      << " result_multimatch=" << summary.results.multimatch
                      << " unmatched_primary=" << summary.results.unmatched_primary
                      << " unmatched_secondary=" << summary.results.unmatched_secondary
                      << " row_limited=" << summary.row_limited_heuristics
                      << " timed_out=" << summary.timed_out_heuristics
                      << " cancelled=" << summary.cancelled_heuristics
                      << " out=" << out_db.string() << '\n';
            return 0;
        }

        if ((command == "batch-diff" || command == "parity-report") && argc >= 3) {
            const std::filesystem::path manifest_path(argv[2]);
            const auto manifest_abs = std::filesystem::absolute(manifest_path);
            const auto manifest_dir = manifest_abs.parent_path();
            std::ifstream manifest_file(manifest_abs);
            if (!manifest_file.is_open()) {
                throw std::runtime_error("cannot open manifest: " + manifest_abs.string());
            }
            std::string manifest_text((std::istreambuf_iterator<char>(manifest_file)),
                                       std::istreambuf_iterator<char>());

            std::filesystem::path out_dir = std::filesystem::absolute("build");
            for (int i = 3; i < argc; ++i) {
                if (std::string_view(argv[i]) == "--out" && i + 1 < argc) {
                    out_dir = std::filesystem::absolute(std::filesystem::path(argv[++i]));
                }
            }
            std::filesystem::create_directories(out_dir);

            std::size_t sample_pos = 0;
            const auto samples_start = manifest_text.find("\"samples\"");
            if (samples_start == std::string::npos) {
                throw std::runtime_error("manifest missing 'samples' array");
            }

            std::size_t obj_pos = manifest_text.find('{', manifest_text.find('[', samples_start));
            while (obj_pos != std::string::npos) {
                const auto obj_end = manifest_text.find('}', obj_pos);
                if (obj_end == std::string::npos) break;
                const auto obj = manifest_text.substr(obj_pos, obj_end - obj_pos + 1);

                const auto name = extract_json_string(obj, "name");
                if (name.empty()) {
                    obj_pos = manifest_text.find('{', obj_end + 1);
                    continue;
                }
                auto main_db_rel = extract_json_string(obj, "main_db");
                auto diff_db_rel = extract_json_string(obj, "diff_db");
                const auto main_db = std::filesystem::absolute(manifest_dir / main_db_rel);
                const auto diff_db = std::filesystem::absolute(manifest_dir / diff_db_rel);
                const auto result_path = out_dir / (name + ".soff");

                if (const auto err = validate_export_database(main_db, "main"); !err.empty()) {
                    std::cerr << name << ": " << err << '\n';
                    obj_pos = manifest_text.find('{', obj_end + 1);
                    continue;
                }
                if (const auto err = validate_export_database(diff_db, "diff"); !err.empty()) {
                    std::cerr << name << ": " << err << '\n';
                    obj_pos = manifest_text.find('{', obj_end + 1);
                    continue;
                }

                soff::diff::DiffSessionOptions options;
                options.sql.enable_unreliable = obj.find("\"unreliable\": true") != std::string::npos
                    || obj.find("\"unreliable\":true") != std::string::npos;
                const auto summary = soff::diff::DiffSession{options}.run_all(main_db, diff_db, result_path);

                const auto baseline = extract_object(obj, "python_baseline");
                const auto py_best = extract_json_integer(baseline, "best", -1);
                const auto py_partial = extract_json_integer(baseline, "partial", -1);
                const auto py_unreliable = extract_json_integer(baseline, "unreliable", -1);

                std::cout << name << ": "
                          << "best=" << summary.results.best
                          << " partial=" << summary.results.partial
                          << " unreliable=" << summary.results.unreliable
                          << " prop_name=" << summary.propagation.same_name_matches
                          << " prop_diff=" << summary.propagation.diffing_matches
                          << " prop_const=" << summary.propagation.related_constants_matches
                          << " prop_affine=" << summary.propagation.affine_matches;
                if (py_best >= 0) {
                    std::cout << " | python best=" << py_best
                              << " partial=" << py_partial
                              << " unreliable=" << py_unreliable
                              << " | delta best="
                              << (static_cast<std::int64_t>(summary.results.best) - py_best)
                              << " partial="
                              << (static_cast<std::int64_t>(summary.results.partial) - py_partial);
                }
                std::cout << '\n';

                obj_pos = manifest_text.find('{', obj_end + 1);
                const auto next_bracket = manifest_text.find(']', obj_end);
                if (next_bracket != std::string::npos && next_bracket < obj_pos) break;
            }
            return 0;
        }

        if (command == "patch-diff" && argc >= 5) {
            const std::filesystem::path result_db(argv[2]);
            const std::filesystem::path main_db(argv[3]);
            const std::filesystem::path diff_db(argv[4]);
            const bool json_output = argc > 5 && std::string_view(argv[5]) == "--json";

            soff::db::ResultRepository result_repository;
            const auto results = result_repository.load(result_db);

            soff::db::Database database;
            database.open(main_db);
            soff::SnapshotRepository snapshot_repository;
            snapshot_repository.attach_diff(database, diff_db);

            const auto patch_result = soff::diff::analyze_patch_diff(database, results.matches);
            if (json_output) {
                std::cout << "[\n";
                for (std::size_t i = 0; i < patch_result.findings.size(); ++i) {
                    const auto& f = patch_result.findings[i];
                    std::cout << "  {\"primary\":" << f.primary
                              << ",\"secondary\":" << f.secondary
                              << ",\"primary_name\":\"" << f.primary_name << "\""
                              << ",\"secondary_name\":\"" << f.secondary_name << "\""
                              << ",\"ratio\":" << f.ratio
                              << ",\"detail\":\"" << f.detail << "\""
                              << "}";
                    if (i + 1 < patch_result.findings.size()) std::cout << ',';
                    std::cout << '\n';
                }
                std::cout << "]\n";
            } else {
                std::cout << "findings=" << patch_result.findings.size() << '\n';
                for (const auto& f : patch_result.findings) {
                    std::cout << "  " << f.primary_name << " -> " << f.secondary_name
                              << " ratio=" << f.ratio
                              << " " << f.detail << '\n';
                }
            }
            return 0;
        }

        if (command == "ml-export" && argc >= 7
            && std::string_view(argv[5]) == "--out") {
            const std::filesystem::path result_db(argv[2]);
            const std::filesystem::path main_db(argv[3]);
            const std::filesystem::path diff_db(argv[4]);
            const std::filesystem::path output(argv[6]);

            soff::db::ResultRepository result_repository;
            const auto results = result_repository.load(result_db);

            soff::db::Database database;
            database.open(main_db);
            soff::SnapshotRepository snapshot_repository;
            snapshot_repository.attach_diff(database, diff_db);

            const auto features = soff::diff::extract_ml_features(database, results.matches);
            const auto ext = output.extension().string();
            if (ext == ".json") {
                soff::diff::export_ml_features_json(features, output);
            } else {
                soff::diff::export_ml_features_csv(features, output);
            }
            std::cout << "features=" << features.size() << " out=" << output.string() << '\n';
            return 0;
        }

        print_usage();
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
