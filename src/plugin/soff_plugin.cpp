#include <fstream>
#include <sstream>

#include "soff/core/version.hpp"
#include "soff/core/error.hpp"
#include "soff/db/database.hpp"
#include "soff/db/result_repository.hpp"
#include "soff/db/repository.hpp"
#include "soff/diff/session.hpp"
#include "soff/ui/html_diff.hpp"
#include "soff/ui/import_plan.hpp"
#include "soff/ui/line_diff.hpp"

#include <boost/uuid/detail/md5.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#if defined(_WIN32)
#include <windows.h>
#include <wincrypt.h>
#include <shellapi.h>
#else
#include <cstdio>
#endif

#include <bytes.hpp>
#include <funcs.hpp>
#include <gdl.hpp>
#include <graph.hpp>
#include <hexrays.hpp>
#include <ida.hpp>
#include <idp.hpp>
#include <kernwin.hpp>
#include <loader.hpp>
#include <nalt.hpp>
#include <name.hpp>
#include <typeinf.hpp>
#include <ua.hpp>
#include <lines.hpp>
#include <xref.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cctype>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <numeric>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

constexpr const char* export_action_name = "soff:export";
constexpr const char* export_action_label = "Export current IDB";
constexpr const char* diff_action_name = "soff:diff";
constexpr const char* diff_action_label = "Diff SQLite databases";
constexpr const char* view_results_action_name = "soff:view_results";
constexpr const char* view_results_action_label = "Load Diff Results";
constexpr const char* save_results_action_name = "soff:save_results";
constexpr const char* save_results_action_label = "Save Diff Results As";
constexpr const char* import_results_action_name = "soff:import_results";
constexpr const char* import_results_action_label = "Import Diff Results";
constexpr const char* local_diff_action_name = "soff:local_diff";
constexpr const char* local_diff_action_label = "Local Function Diff";
constexpr const char* graph_sync_action_name = "soff:graph_sync_peer";
constexpr const char* graph_jump_action_name = "soff:graph_jump_primary";
constexpr const char* graph_text_action_name = "soff:graph_text_diff";
constexpr const char* graph_import_action_name = "soff:graph_import_match";
constexpr const char* menu_id = "soff_menu";
constexpr const char* menu_label = "Soff";
constexpr const char* menu_path = "Soff/";
constexpr const char* export_menu_path = "Soff/Export current IDB";
constexpr const char* diff_menu_path = "Soff/Diff SQLite databases";
constexpr const char* view_results_menu_path = "Soff/Load Diff Results";
constexpr const char* save_results_menu_path = "Soff/Save Diff Results As";
constexpr const char* import_results_menu_path = "Soff/Import Diff Results";
constexpr const char* local_diff_menu_path = "Soff/Local Function Diff";
constexpr const char* options_node_name = "$ soff options";
constexpr nodeidx_t option_export_use_decompiler = 101;

struct ExportOptions
{
    std::string previous_crash_marker;
    bool crash_file_preexisting = false;
    bool resume_existing_database = false;
    bool use_decompiler = true;
    bool exclude_library_thunk = true;
    bool ida_subs = true;
    bool ignore_small_functions = false;
    ea_t min_ea = 0;
    ea_t max_ea = BADADDR;
};

struct DiffUiOptions
{
    std::string main_db;
    std::string diff_db;
    std::string result_db;
    bool slow = true;
    bool unreliable = false;
    bool experimental = false;
    std::size_t max_rows = 1000000;
    std::uint32_t timeout_seconds = 300;
};

struct ExportStats
{
    std::size_t total_functions = 0;
    std::size_t exported_functions = 0;
    std::size_t resumed_functions = 0;
    std::size_t skipped_functions = 0;
    std::size_t batch_commits = 0;
    std::unordered_map<std::string, std::size_t> skip_reasons;
    std::size_t last_function_index = 0;
    soff::Address last_function_address = 0;
    std::string last_function_name;
    double export_seconds = 0.0;
};

struct HexRaysExportContext
{
    bool requested = false;
    bool available = false;
    std::size_t pseudocode_functions = 0;
    std::size_t pseudocode_failures = 0;
    std::size_t microcode_functions = 0;
    std::size_t microcode_failures = 0;
    std::size_t pseudocode_comments = 0;
    std::size_t cache_clears = 0;
    std::unordered_map<std::string, std::size_t> failure_codes;
};

struct ExportResult
{
    soff::ProgramSnapshot snapshot;
    ExportStats stats;
};

std::string to_string(const qstring& value)
{
    return value.c_str() != nullptr ? value.c_str() : "";
}

std::string trim_copy(std::string text)
{
    const auto first = std::find_if_not(text.begin(), text.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
    const auto last = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }).base();
    if (first >= last) {
        return "";
    }
    return std::string(first, last);
}

const char* env_value(const char* name)
{
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' ? value : nullptr;
}

bool parse_bool(std::string_view value, bool fallback)
{
    if (value == "1" || value == "true" || value == "TRUE" || value == "yes" || value == "YES") {
        return true;
    }
    if (value == "0" || value == "false" || value == "FALSE" || value == "no" || value == "NO") {
        return false;
    }
    return fallback;
}

void read_bool_env(const char* name, bool& option)
{
    if (const char* value = env_value(name)) {
        option = parse_bool(value, option);
    }
}

void read_address_env(const char* name, ea_t& option)
{
    if (const char* value = env_value(name)) {
        option = static_cast<ea_t>(std::stoull(value, nullptr, 0));
    }
}

void copy_path_to_buffer(char* buffer, std::size_t buffer_size, const std::filesystem::path& path)
{
    if (buffer_size == 0) {
        return;
    }
    const auto text = path.string();
    std::strncpy(buffer, text.c_str(), buffer_size - 1);
    buffer[buffer_size - 1] = '\0';
}

std::string read_text_file(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return "";
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

std::filesystem::path default_export_path()
{
    char input_path[QMAXPATH] = {};
    if (get_input_file_path(input_path, sizeof(input_path)) > 0 && input_path[0] != '\0') {
        return std::filesystem::path(input_path).replace_extension(".sqlite");
    }
    return "soff_export.sqlite";
}

std::filesystem::path default_diff_result_path(const std::string& main_db)
{
    if (!main_db.empty()) {
        return std::filesystem::path(main_db).replace_extension(".soff");
    }
    return "soff_result.soff";
}

std::filesystem::path default_pair_diff_result_path(const std::string& main_db, const std::string& diff_db)
{
    if (main_db.empty()) {
        return default_diff_result_path(main_db);
    }

    const auto primary = std::filesystem::path(main_db);
    if (diff_db.empty()) {
        auto output = primary;
        output.replace_extension(".soff");
        return output;
    }

    const auto secondary = std::filesystem::path(diff_db);
    auto output = primary.parent_path();
    output /= primary.stem().string() + "_vs_" + secondary.stem().string() + ".soff";
    return output;
}

bool sqlite_table_exists(soff::db::Database& database, std::string_view table_name)
{
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
    const auto size = std::filesystem::file_size(path);
    if (size == 0) {
        return std::string(label) + " database is empty: " + path.string();
    }

    try {
        soff::db::Database database;
        database.open(path);
        for (const auto* table : {"version", "program", "functions"}) {
            if (!sqlite_table_exists(database, table)) {
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

std::unordered_set<soff::Address> load_exported_function_addresses(const std::filesystem::path& path)
{
    std::unordered_set<soff::Address> addresses;
    if (path.empty() || !std::filesystem::exists(path) || std::filesystem::file_size(path) == 0) {
        return addresses;
    }

    try {
        soff::db::Database database;
        database.open(path);
        if (!sqlite_table_exists(database, "functions")) {
            return addresses;
        }
        const auto rows = database.query_rows("select address from functions where address is not null and address != ''");
        addresses.reserve(rows.size());
        for (const auto& row : rows) {
            if (row.empty() || row[0].empty()) {
                continue;
            }
            try {
                addresses.insert(static_cast<soff::Address>(std::stoull(row[0], nullptr, 0)));
            } catch (...) {
                msg("Soff: ignoring unreadable exported function address: %s\n", row[0].c_str());
            }
        }
    } catch (const std::exception& error) {
        msg("Soff: cannot read existing export addresses from %s: %s\n", path.string().c_str(), error.what());
    }
    return addresses;
}

std::string validate_result_database(const std::filesystem::path& path)
{
    if (path.empty()) {
        return "result database path is empty";
    }
    if (!std::filesystem::exists(path)) {
        return "result database does not exist: " + path.string();
    }
    if (std::filesystem::file_size(path) == 0) {
        return "result database is empty: " + path.string();
    }

    try {
        soff::db::Database database;
        database.open(path);
        for (const auto* table : {"config", "results", "unmatched"}) {
            if (!sqlite_table_exists(database, table)) {
                return "result database is missing table '" + std::string(table) + "': " + path.string();
            }
        }
    } catch (const std::exception& error) {
        return std::string("result database is not readable: ") + error.what();
    }
    return "";
}

void save_string_option(netnode& node, nodeidx_t index, const std::string& value)
{
    node.supset(index, value.c_str());
}

std::string load_string_option(netnode& node, nodeidx_t index)
{
    qstring value;
    if (node.supstr(&value, index) <= 0) {
        return "";
    }
    return to_string(value);
}

void save_bool_option(netnode& node, nodeidx_t index, bool value)
{
    node.altset(index, value ? 2 : 1);
}

bool load_bool_option(netnode& node, nodeidx_t index, bool& value)
{
    const auto encoded = node.altval(index);
    if (encoded == 1) {
        value = false;
        return true;
    }
    if (encoded == 2) {
        value = true;
        return true;
    }
    return false;
}

void save_dialog_options(const std::filesystem::path& output_path, const ExportOptions& options)
{
    netnode node;
    node.create(options_node_name);

    save_string_option(node, 1, output_path.string());
    save_bool_option(node, option_export_use_decompiler, options.use_decompiler);

    node.altset(1, options.use_decompiler ? 1 : 0);
    node.altset(5, options.exclude_library_thunk ? 1 : 0);
    node.altset(6, options.ida_subs ? 1 : 0);
    node.altset(7, options.ignore_small_functions ? 1 : 0);
    node.altset(9, static_cast<nodeidx_t>(options.min_ea));
    node.altset(10, static_cast<nodeidx_t>(options.max_ea));
}

void load_dialog_options(std::filesystem::path& output_path, ExportOptions& options)
{
    netnode node(options_node_name);
    if (node == BADNODE) {
        return;
    }

    const auto saved_output = load_string_option(node, 1);
    if (!saved_output.empty()) {
        output_path = saved_output;
    }
    if (!load_bool_option(node, option_export_use_decompiler, options.use_decompiler) && node.altval(1) != 0) {
        options.use_decompiler = true;
    }
    options.exclude_library_thunk = node.altval(5) != 0;
    options.ida_subs = node.altval(6) != 0;
    options.ignore_small_functions = node.altval(7) != 0;

    const auto min_ea = node.altval(9);
    const auto max_ea = node.altval(10);
    options.min_ea = static_cast<ea_t>(min_ea);
    options.max_ea = max_ea != 0 ? static_cast<ea_t>(max_ea) : BADADDR;
}

void save_diff_options(const DiffUiOptions& options)
{
    netnode node;
    node.create(options_node_name);

    save_string_option(node, 20, options.main_db);
    save_string_option(node, 21, options.diff_db);
    save_string_option(node, 22, options.result_db);
    node.altset(20, options.slow ? 1 : 0);
    node.altset(21, options.unreliable ? 1 : 0);
    node.altset(22, options.experimental ? 1 : 0);
    node.altset(23, static_cast<nodeidx_t>(options.max_rows));
    node.altset(24, static_cast<nodeidx_t>(options.timeout_seconds));
}

void save_last_result_path(const std::filesystem::path& result_path)
{
    netnode node;
    node.create(options_node_name);
    save_string_option(node, 30, result_path.string());
}

std::filesystem::path load_last_result_path()
{
    netnode node(options_node_name);
    if (node == BADNODE) {
        return {};
    }
    return load_string_option(node, 30);
}

void load_diff_options(DiffUiOptions& options)
{
    netnode node(options_node_name);
    if (node == BADNODE) {
        return;
    }

    options.main_db = load_string_option(node, 20);
    options.diff_db = load_string_option(node, 21);
    options.result_db = load_string_option(node, 22);
    options.slow = node.altval(20) != 0;
    options.unreliable = node.altval(21) != 0;
    options.experimental = node.altval(22) != 0;
    const auto max_rows = node.altval(23);
    const auto timeout = node.altval(24);
    if (max_rows != 0) {
        options.max_rows = static_cast<std::size_t>(max_rows);
    }
    if (timeout != 0) {
        options.timeout_seconds = static_cast<std::uint32_t>(timeout);
    }
}

void apply_env_overrides(ExportOptions& options)
{
    read_bool_env("DIAPHORA_USE_DECOMPILER", options.use_decompiler);
    read_bool_env("DIAPHORA_EXCLUDE_LIBRARY_THUNK", options.exclude_library_thunk);
    read_bool_env("DIAPHORA_IDA_SUBS", options.ida_subs);
    read_bool_env("DIAPHORA_IGNORE_SMALL_FUNCTIONS", options.ignore_small_functions);
    read_address_env("DIAPHORA_FROM_ADDRESS", options.min_ea);
    read_address_env("DIAPHORA_TO_ADDRESS", options.max_ea);
}

void apply_diff_env_overrides(DiffUiOptions& options)
{
    if (const char* main_db = env_value("DIAPHORA_DB1")) {
        options.main_db = main_db;
    }
    if (const char* diff_db = env_value("DIAPHORA_DB2")) {
        options.diff_db = diff_db;
    }
    if (const char* diff_db = env_value("DIAPHORA_FILE_IN")) {
        options.diff_db = diff_db;
    }
    if (const char* diff_out = env_value("DIAPHORA_DIFF_OUT")) {
        options.result_db = diff_out;
    }
    read_bool_env("DIAPHORA_SLOW_HEURISTICS", options.slow);
    read_bool_env("DIAPHORA_UNRELIABLE", options.unreliable);
    read_bool_env("DIAPHORA_EXPERIMENTAL", options.experimental);
}

bool ask_export_options(std::filesystem::path& output_path, ExportOptions& options)
{
    char output_buffer[QMAXPATH] = {};

    copy_path_to_buffer(output_buffer, sizeof(output_buffer), output_path.empty() ? default_export_path() : output_path);

    ushort checks = 0;
    constexpr ushort check_use_decompiler = 1 << 0;
    constexpr ushort check_exclude_library_thunk = 1 << 4;
    constexpr ushort check_ida_subs = 1 << 5;
    constexpr ushort check_ignore_small = 1 << 6;

    if (options.use_decompiler) {
        checks |= check_use_decompiler;
    }
    if (options.exclude_library_thunk) {
        checks |= check_exclude_library_thunk;
    }
    if (options.ida_subs) {
        checks |= check_ida_subs;
    }
    if (options.ignore_small_functions) {
        checks |= check_ignore_small;
    }

    ea_t min_ea = options.min_ea;
    ea_t max_ea = options.max_ea;
    static const char form[] =
        "STARTITEM 0\n"
        "BUTTON YES* Export\n"
        "BUTTON CANCEL Cancel\n"
        "Soff export\n"
        "\n"
        "<~O~utput SQLite:f:1:72::>\n"
        "<~F~rom address:$:18:18::> <~T~o address:$:18:18::>\n"
        "\n"
        "<##Options##Use decompiler:C>\n"
        "<Exclude library/thunk/nullsub:C>\n"
        "<Export IDA generated sub_/j_ names:C>\n"
        "<Ignore very small functions:C>>\n";

    if (ask_form(form, output_buffer, &min_ea, &max_ea, &checks) <= 0) {
        return false;
    }

    output_path = output_buffer;
    options.min_ea = min_ea;
    options.max_ea = max_ea;
    options.use_decompiler = (checks & check_use_decompiler) != 0;
    options.exclude_library_thunk = (checks & check_exclude_library_thunk) != 0;
    options.ida_subs = (checks & check_ida_subs) != 0;
    options.ignore_small_functions = (checks & check_ignore_small) != 0;

    if (output_path.empty()) {
        warning("Soff export needs an output SQLite path.");
        return false;
    }
    return true;
}

bool ask_diff_options(DiffUiOptions& options)
{
    char main_buffer[QMAXPATH] = {};
    char diff_buffer[QMAXPATH] = {};
    char result_buffer[QMAXPATH] = {};
    copy_path_to_buffer(main_buffer, sizeof(main_buffer), options.main_db);
    copy_path_to_buffer(diff_buffer, sizeof(diff_buffer), options.diff_db);
    copy_path_to_buffer(
        result_buffer,
        sizeof(result_buffer),
        options.result_db.empty() ? default_pair_diff_result_path(options.main_db, options.diff_db) : std::filesystem::path(options.result_db));

    ushort checks = 0;
    constexpr ushort check_slow = 1 << 0;
    constexpr ushort check_unreliable = 1 << 1;
    constexpr ushort check_experimental = 1 << 2;
    if (options.slow) {
        checks |= check_slow;
    }
    if (options.unreliable) {
        checks |= check_unreliable;
    }
    if (options.experimental) {
        checks |= check_experimental;
    }

    uval_t max_rows = static_cast<uval_t>(options.max_rows);
    uval_t timeout_seconds = static_cast<uval_t>(options.timeout_seconds);
    static const char form[] =
        "STARTITEM 0\n"
        "BUTTON YES* Diff\n"
        "BUTTON CANCEL Cancel\n"
        "Soff diff\n"
        "\n"
        "<~P~rimary SQLite:f:0:72::>\n"
        "<~S~econdary SQLite:f:0:72::>\n"
        "<~R~esult DB:f:1:72::>\n"
        "<~M~ax rows:u:18:18::> <~T~imeout seconds:u:18:18::>\n"
        "\n"
        "<##Options##Enable slow heuristics:C>\n"
        "<Enable unreliable heuristics:C>\n"
        "<Enable experimental heuristics:C>>\n";

    if (ask_form(form, main_buffer, diff_buffer, result_buffer, &max_rows, &timeout_seconds, &checks) <= 0) {
        return false;
    }

    options.main_db = main_buffer;
    options.diff_db = diff_buffer;
    options.result_db = result_buffer;
    options.max_rows = static_cast<std::size_t>(max_rows);
    options.timeout_seconds = static_cast<std::uint32_t>(timeout_seconds);
    options.slow = (checks & check_slow) != 0;
    options.unreliable = (checks & check_unreliable) != 0;
    options.experimental = (checks & check_experimental) != 0;

    if (options.main_db.empty() || options.diff_db.empty() || options.result_db.empty()) {
        warning("Soff diff needs primary, secondary, and result database paths.");
        return false;
    }
    if (const auto error = validate_export_database(options.main_db, "Primary"); !error.empty()) {
        warning("Soff diff input check failed:\n%s", error.c_str());
        return false;
    }
    if (const auto error = validate_export_database(options.diff_db, "Secondary"); !error.empty()) {
        warning("Soff diff input check failed:\n%s", error.c_str());
        return false;
    }
    if (options.max_rows == 0) {
        warning("Soff diff max rows must be greater than zero.");
        return false;
    }
    return true;
}

std::string escape_marker_value(std::string value)
{
    std::replace(value.begin(), value.end(), '\n', ' ');
    std::replace(value.begin(), value.end(), '\r', ' ');
    return value;
}

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
#if defined(_WIN32)
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    if (!CryptAcquireContextA(&provider, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        throw soff::Error(soff::ErrorCode::export_failed, "CryptAcquireContextA failed");
    }
    const auto release_provider = [&]() {
        if (hash != 0) CryptDestroyHash(hash);
        if (provider != 0) CryptReleaseContext(provider, 0);
    };
    if (!CryptCreateHash(provider, CALG_MD5, 0, 0, &hash)) {
        release_provider();
        throw soff::Error(soff::ErrorCode::export_failed, "CryptCreateHash(MD5) failed");
    }
    if (!bytes.empty() && !CryptHashData(hash, bytes.data(), static_cast<DWORD>(bytes.size()), 0)) {
        release_provider();
        throw soff::Error(soff::ErrorCode::export_failed, "CryptHashData(MD5) failed");
    }
    std::uint8_t digest[16] = {};
    DWORD digest_size = sizeof(digest);
    if (!CryptGetHashParam(hash, HP_HASHVAL, digest, &digest_size, 0)) {
        release_provider();
        throw soff::Error(soff::ErrorCode::export_failed, "CryptGetHashParam(MD5) failed");
    }
    const auto out = hex_bytes(digest, digest_size);
    release_provider();
    return out;
#else
    // Cross-platform: use boost::uuids::detail::md5
    boost::uuids::detail::md5 hasher;
    if (!bytes.empty()) {
        hasher.process_bytes(bytes.data(), bytes.size());
    }
    boost::uuids::detail::md5::digest_type digest;
    hasher.get_digest(digest);
    std::uint8_t raw[16];
    std::memcpy(raw, digest, 16);
    return hex_bytes(raw, 16);
#endif
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
    if (line.empty()) {
        return;
    }
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
        out << '"';
        for (const char ch : value) {
            if (ch == '"' || ch == '\\') {
                out << '\\';
            }
            out << ch;
        }
        out << '"';
    }
    out << ']';
    return out.str();
}

void append_json_string(std::ostringstream& out, std::string_view value)
{
    out << '"';
    for (const char ch : value) {
        if (ch == '"' || ch == '\\') {
            out << '\\';
        }
        out << ch;
    }
    out << '"';
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

void extract_microcode_features(func_t* function, soff::FunctionFeature& feature, HexRaysExportContext& hexrays)
{
    if (!hexrays.available) {
        return;
    }

    mba_ranges_t ranges(function);
    hexrays_failure_t failure;
    mba_t* mba = gen_microcode(ranges, &failure, nullptr, DECOMP_WARNINGS, MMAT_GENERATED);
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
        if (i == 0) {
            continue;
        }
        const mblock_t* block = mba->get_mblock(static_cast<uint>(i));
        if (block == nullptr || block->type == BLT_STOP) {
            continue;
        }

        soff::BasicBlock micro_block;
        micro_block.start = synthetic_microcode_block_address(function->start_ea, i);
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
            if (successor <= 0 || successor >= mba->qty) {
                continue;
            }
            const mblock_t* successor_block = mba->get_mblock(static_cast<uint>(successor));
            if (successor_block == nullptr || successor_block->type == BLT_STOP) {
                continue;
            }
            micro_block.successors.push_back(synthetic_microcode_block_address(function->start_ea, successor));
        }

        feature.microcode_blocks.push_back(std::move(micro_block));
    }
    delete mba;

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

void extract_pseudocode_features(func_t* function, soff::FunctionFeature& feature, HexRaysExportContext& hexrays)
{
    if (!hexrays.available) {
        return;
    }

    hexrays_failure_t failure;
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
    if (!options.ida_subs) {
        if (starts_with(name, "sub_")
            || starts_with(name, "j_")
            || starts_with(name, "unknown")
            || starts_with(name, "nullsub_")) {
            return "ida-generated-name";
        }
        if ((function->flags & FUNC_LIB) != 0) {
            return "library";
        }
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

soff::FunctionFeature read_function_feature(func_t* function, ea_t imagebase, HexRaysExportContext* hexrays)
{
    soff::FunctionFeature feature;
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
        collect_switch_info(ea, switches);
        append_instruction_to_block(feature.blocks, ea);
        assembly_addresses.push_back(static_cast<soff::Address>(ea));

        for (const auto& operand : instruction.ops) {
            if (operand.type == o_void) {
                break;
            }
            if (operand.type == o_imm) {
                append_unique(feature.constants, std::to_string(static_cast<std::uint64_t>(operand.value)));
            }
            if (operand.type == o_displ) {
                append_unique(feature.constants, std::to_string(static_cast<std::uint64_t>(operand.addr)));
            }
        }

        // Extract string constants from data references
        xrefblk_t data_xref;
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
    if (hexrays != nullptr && hexrays->requested) {
        extract_pseudocode_features(function, feature, *hexrays);
        extract_microcode_features(function, feature, *hexrays);
    }
    return feature;
}

ExportResult build_ida_snapshot(
    const ExportOptions& options,
    const std::filesystem::path* crash_path = nullptr,
    const std::filesystem::path* output_path = nullptr)
{
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
    std::vector<soff::FunctionFeature> pending_functions;
    constexpr std::size_t batch_size = 64;
    if (incremental_save) {
        repository.begin_incremental_save(snapshot, *output_path, !options.resume_existing_database);
        pending_functions.reserve(batch_size);
    }
    auto existing_addresses = options.resume_existing_database && incremental_save
        ? load_exported_function_addresses(*output_path)
        : std::unordered_set<soff::Address>{};
    if (!existing_addresses.empty()) {
        msg(
            "Soff: resume enabled; %zu function address(es) already exist in %s\n",
            existing_addresses.size(),
            output_path->string().c_str());
    }

    const auto flush_pending_functions = [&]() {
        if (!incremental_save || pending_functions.empty()) {
            return;
        }
        repository.append_functions(pending_functions, *output_path);
        ++result.stats.batch_commits;
        update_crash_marker(
            crash_path,
            result.stats.last_function_index,
            function_count,
            result.stats.exported_functions,
            result.stats.skipped_functions,
            result.stats.last_function_address,
            result.stats.last_function_name,
            "batch-committed");
        pending_functions.clear();
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
            if (hexrays.available && i != 0 && (i % 256) == 0) {
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
            update_crash_marker(
                crash_path,
                i,
                function_count,
                result.stats.exported_functions,
                result.stats.skipped_functions,
                function->start_ea,
                function_name,
                "read-function");
            const auto skip_reason = skip_reason_for(function, function_name, options);
            if (!skip_reason.empty()) {
                ++result.stats.skipped_functions;
                ++result.stats.skip_reasons[skip_reason];
                update_crash_marker(
                    crash_path,
                    i,
                    function_count,
                    result.stats.exported_functions,
                    result.stats.skipped_functions,
                    function->start_ea,
                    function_name,
                    "skipped-" + skip_reason);
                continue;
            }
            if (existing_addresses.find(static_cast<soff::Address>(function->start_ea)) != existing_addresses.end()) {
                ++result.stats.exported_functions;
                ++result.stats.resumed_functions;
                update_crash_marker(
                    crash_path,
                    i,
                    function_count,
                    result.stats.exported_functions,
                    result.stats.skipped_functions,
                    function->start_ea,
                    function_name,
                    "resumed-existing");
                continue;
            }
            snapshot.functions.push_back(read_function_feature(function, imagebase, &hexrays));
            if (incremental_save) {
                pending_functions.push_back(snapshot.functions.back());
                if (pending_functions.size() >= batch_size) {
                    flush_pending_functions();
                }
            }
            ++result.stats.exported_functions;
            update_crash_marker(
                crash_path,
                i,
                function_count,
                result.stats.exported_functions,
                result.stats.skipped_functions,
                function->start_ea,
                function_name,
                "function-complete");
        }
        flush_pending_functions();
        hide_wait_box();
    } catch (...) {
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
    append_program_data(snapshot, "export.ida_subs", "boolean", options.ida_subs ? "true" : "false");
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

    if (incremental_save) {
        repository.replace_program_data(snapshot.program_data, *output_path);
        repository.finalize_incremental_save(*output_path);
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
    if (const auto error = validate_export_database(options.main_db, "Primary"); !error.empty()) {
        throw soff::Error(soff::ErrorCode::diff_failed, error);
    }
    if (const auto error = validate_export_database(options.diff_db, "Secondary"); !error.empty()) {
        throw soff::Error(soff::ErrorCode::diff_failed, error);
    }

    soff::diff::DiffSessionOptions diff_options;
    diff_options.sql.enable_slow = options.slow;
    diff_options.sql.enable_unreliable = options.unreliable;
    diff_options.sql.enable_experimental = options.experimental;
    diff_options.sql.max_processed_rows = options.max_rows;
    diff_options.sql.timeout_seconds = options.timeout_seconds;

    const auto summary = soff::diff::DiffSession{diff_options}.run_all(
        options.main_db,
        options.diff_db,
        options.result_db);
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

struct ImportApplySummary
{
    std::size_t renamed = 0;
    std::size_t already_named = 0;
    std::size_t prototypes = 0;
    std::size_t function_comments = 0;
    std::size_t function_flags = 0;
    std::size_t instruction_comments = 0;
    std::size_t forced_operands = 0;
    std::size_t pseudocode_comments = 0;
    std::size_t type_libraries = 0;
    std::size_t type_definitions = 0;
    std::size_t missing_function = 0;
    std::size_t failed = 0;
};

struct ResultChooserRow
{
    int match_index = -1;
    std::string type;
    std::string ratio;
    soff::Address primary = 0;
    std::string primary_name;
    soff::Address secondary = 0;
    std::string secondary_name;
    std::string description;
    soff::Address jump_address = 0;
};

enum class SelectedChooserEditResult
{
    unchanged,
    imported,
    jump,
};

soff::ui::ImportPlanOptions import_plan_options_from_env();
ImportApplySummary apply_import_plan(const soff::ui::ImportPlan& plan);
bool refresh_primary_export_after_import(
    const soff::db::DiffResultSet& results,
    bool allow_prompt,
    const char* context);
bool save_or_open_match_html_diff(
    const std::filesystem::path& result_path,
    const soff::db::DiffResultSet& results,
    const soff::db::ResultMatch& match,
    bool open_after_save);
bool save_or_open_match_graph_diff(
    const std::filesystem::path& result_path,
    const soff::db::DiffResultSet& results,
    const soff::db::ResultMatch& match);
enum class TextDiffKind
{
    assembly,
    pseudocode,
    microcode,
};
bool show_ida_match_graph_diff(
    const soff::db::DiffResultSet& results,
    const soff::db::ResultMatch& match,
    soff::ui::GraphDiffKind kind);
bool show_linear_asm_diff(
    const soff::db::DiffResultSet& results,
    const soff::db::ResultMatch& match);
bool show_linear_pseudo_diff(
    const soff::db::DiffResultSet& results,
    const soff::db::ResultMatch& match);
bool show_ida_match_text_diff(
    const soff::db::DiffResultSet& results,
    const soff::db::ResultMatch& match,
    TextDiffKind kind,
    soff::Address focus_primary = 0,
    soff::Address focus_secondary = 0);
bool save_or_open_match_call_context(
    const std::filesystem::path& result_path,
    const soff::db::DiffResultSet& results,
    const soff::db::ResultMatch& match);
bool show_match_diff_panel(
    const std::filesystem::path& result_path,
    const soff::db::DiffResultSet& results,
    const soff::db::ResultMatch& match);
SelectedChooserEditResult import_selected_matches_from_chooser(
    const std::filesystem::path& result_path,
    const soff::db::DiffResultSet& results,
    const std::vector<ResultChooserRow>& rows,
    const sizevec_t& selected_rows);

class diff_results_chooser_t : public chooser_multi_t
{
public:
    explicit diff_results_chooser_t(std::filesystem::path result_path, soff::db::DiffResultSet results)
        : chooser_multi_t(CH_CAN_REFRESH | CH_CAN_EDIT | CH_RESTORE, column_count, widths_, header_, "Soff Diff Results"),
          result_path_(std::move(result_path)),
          results_(std::move(results))
    {
        CASSERT(column_count == 7);
        rebuild_rows();
    }

    const void* get_obj_id(size_t* len) const override
    {
        *len = result_path_text_.size();
        return result_path_text_.data();
    }

    size_t idaapi get_count() const override
    {
        return rows_.size();
    }

    void idaapi get_row(qstrvec_t* cols_, int*, chooser_item_attrs_t*, size_t n) const override
    {
        if (n >= rows_.size()) {
            return;
        }
        const auto& row = rows_[n];
        qstrvec_t& cols = *cols_;
        cols[0].sprnt("%s", row.type.c_str());
        cols[1].sprnt("%s", row.ratio.c_str());
        const auto primary = hex_address(row.primary);
        const auto secondary = hex_address(row.secondary);
        cols[2].sprnt("%s", primary.c_str());
        cols[3].sprnt("%s", row.primary_name.c_str());
        cols[4].sprnt("%s", secondary.c_str());
        cols[5].sprnt("%s", row.secondary_name.c_str());
        cols[6].sprnt("%s", row.description.c_str());
    }

    cbres_t idaapi enter(sizevec_t* sel) override
    {
        if (sel != nullptr && !sel->empty()) {
            const auto n = sel->front();
            if (n < rows_.size()) {
                const auto& row = rows_[n];
                if (row.match_index >= 0 && static_cast<std::size_t>(row.match_index) < results_.matches.size()) {
                    show_match_diff_panel(
                        result_path_,
                        results_,
                        results_.matches[static_cast<std::size_t>(row.match_index)]);
                } else if (row.jump_address != 0) {
                    jumpto(static_cast<ea_t>(row.jump_address));
                }
            }
        }
        return NOTHING_CHANGED;
    }

    cbres_t idaapi edit(sizevec_t* sel) override
    {
        if (sel == nullptr || sel->empty()) {
            return NOTHING_CHANGED;
        }

        const auto edit_result = import_selected_matches_from_chooser(result_path_, results_, rows_, *sel);
        if (edit_result == SelectedChooserEditResult::imported) {
            results_ = soff::db::ResultRepository{}.load(result_path_);
            rebuild_rows();
            return ALL_CHANGED;
        }
        if (edit_result == SelectedChooserEditResult::jump) {
            return enter(sel);
        }
        return NOTHING_CHANGED;
    }

    cbres_t idaapi refresh(sizevec_t*) override
    {
        results_ = soff::db::ResultRepository{}.load(result_path_);
        rebuild_rows();
        return ALL_CHANGED;
    }

private:
    void rebuild_rows()
    {
        result_path_text_ = result_path_.string();
        rows_.clear();
        rows_.reserve(results_.matches.size() + results_.unmatched.size());
        for (std::size_t index = 0; index < results_.matches.size(); ++index) {
            const auto& match = results_.matches[index];
            std::ostringstream ratio;
            ratio << std::fixed << std::setprecision(6) << match.ratio;
            rows_.push_back({
                static_cast<int>(index),
                std::string(soff::db::result_kind_name(match.kind)),
                ratio.str(),
                match.primary,
                match.primary_name,
                match.secondary,
                match.secondary_name,
                match.description,
                match.primary,
            });
        }
        for (const auto& unmatched : results_.unmatched) {
            const bool primary = unmatched.kind == soff::db::UnmatchedKind::primary;
            rows_.push_back({
                -1,
                std::string("unmatched.") + std::string(soff::db::unmatched_kind_name(unmatched.kind)),
                "",
                primary ? unmatched.address : 0,
                primary ? unmatched.name : "",
                primary ? 0 : unmatched.address,
                primary ? "" : unmatched.name,
                "",
                primary ? unmatched.address : 0,
            });
        }
    }

    static constexpr int column_count = 7;
    static const int widths_[];
    static const char* const header_[];
    std::filesystem::path result_path_;
    std::string result_path_text_;
    soff::db::DiffResultSet results_;
    std::vector<ResultChooserRow> rows_;
};

const int diff_results_chooser_t::widths_[] = {
    14,
    10,
    CHCOL_HEX | 16,
    28,
    CHCOL_HEX | 16,
    28,
    42,
};

const char* const diff_results_chooser_t::header_[] = {
    "Type",
    "Ratio",
    "Primary",
    "Primary name",
    "Secondary",
    "Secondary name",
    "Description",
};

std::string db_file_label(const std::string& path)
{
    if (path.empty()) {
        return "(unknown DB)";
    }
    const auto file_name = std::filesystem::path(path).filename().string();
    return file_name.empty() ? path : file_name;
}

std::string side_function_label(const std::string& db_path, const std::string& function_name)
{
    const auto db_label = db_file_label(db_path);
    if (function_name.empty()) {
        return db_label;
    }
    return db_label + " :: " + function_name;
}

enum class MatchDiffPanelAction
{
    assembly_flat,
    assembly_blocks,
    pseudocode_flat,
    microcode_flat,
    microcode_blocks,
    call_context,
    html_open,
    html_save,
    html_graph_preview,
};

struct MatchDiffPanelRow
{
    MatchDiffPanelAction action = MatchDiffPanelAction::assembly_flat;
    std::string content;
    std::string mode;
    std::string target;
    std::string detail;
};

class match_diff_panel_chooser_t : public chooser_multi_t
{
public:
    match_diff_panel_chooser_t(
        std::filesystem::path result_path,
        soff::db::DiffResultSet results,
        soff::db::ResultMatch match)
        : chooser_multi_t(CH_RESTORE, column_count, widths_, header_, "Soff Match Diff Panel"),
          result_path_(std::move(result_path)),
          results_(std::move(results)),
          match_(std::move(match))
    {
        CASSERT(column_count == 4);
        const auto left = side_function_label(results_.main_db, match_.primary_name);
        const auto right = side_function_label(results_.diff_db, match_.secondary_name);
        target_label_ = left + "  <->  " + right;
        rows_ = {
            {MatchDiffPanelAction::assembly_flat, "Assembly", "Linear", target_label_, "Side-by-side linear address disassembly diff"},
            {MatchDiffPanelAction::assembly_blocks, "Assembly", "Graph", target_label_, "Side-by-side IDA graph diff of native basic blocks"},
            {MatchDiffPanelAction::pseudocode_flat, "Pseudocode", "Linear", target_label_, "Side-by-side pseudocode diff"},
            {MatchDiffPanelAction::microcode_blocks, "Microcode", "Graph", target_label_, "Side-by-side IDA graph diff of microcode basic blocks"},
        };
    }

    const void* get_obj_id(size_t* len) const override
    {
        *len = target_label_.size();
        return target_label_.data();
    }

    size_t idaapi get_count() const override
    {
        return rows_.size();
    }

    void idaapi get_row(qstrvec_t* cols_, int*, chooser_item_attrs_t*, size_t n) const override
    {
        if (n >= rows_.size()) {
            return;
        }
        const auto& row = rows_[n];
        qstrvec_t& cols = *cols_;
        cols[0].sprnt("%s", row.content.c_str());
        cols[1].sprnt("%s", row.mode.c_str());
        cols[2].sprnt("%s", row.target.c_str());
        cols[3].sprnt("%s", row.detail.c_str());
    }

    cbres_t idaapi enter(sizevec_t* sel) override
    {
        if (sel == nullptr || sel->empty()) {
            return NOTHING_CHANGED;
        }
        const auto index = sel->front();
        if (index >= rows_.size()) {
            return NOTHING_CHANGED;
        }
        run_action(rows_[index].action);
        return NOTHING_CHANGED;
    }

private:
    void run_action(MatchDiffPanelAction action)
    {
        switch (action) {
        case MatchDiffPanelAction::assembly_flat:
            show_linear_asm_diff(results_, match_);
            break;
        case MatchDiffPanelAction::assembly_blocks:
            show_ida_match_graph_diff(results_, match_, soff::ui::GraphDiffKind::native);
            break;
        case MatchDiffPanelAction::pseudocode_flat:
            show_linear_pseudo_diff(results_, match_);
            break;
        case MatchDiffPanelAction::microcode_flat:
            show_ida_match_text_diff(results_, match_, TextDiffKind::microcode);
            break;
        case MatchDiffPanelAction::microcode_blocks:
            show_ida_match_graph_diff(results_, match_, soff::ui::GraphDiffKind::microcode);
            break;
        case MatchDiffPanelAction::call_context:
            save_or_open_match_call_context(result_path_, results_, match_);
            break;
        case MatchDiffPanelAction::html_open:
            save_or_open_match_html_diff(result_path_, results_, match_, true);
            break;
        case MatchDiffPanelAction::html_save:
            save_or_open_match_html_diff(result_path_, results_, match_, false);
            break;
        case MatchDiffPanelAction::html_graph_preview:
            save_or_open_match_graph_diff(result_path_, results_, match_);
            break;
        }
    }

    static constexpr int column_count = 4;
    static const int widths_[];
    static const char* const header_[];
    std::filesystem::path result_path_;
    soff::db::DiffResultSet results_;
    soff::db::ResultMatch match_;
    std::string target_label_;
    std::vector<MatchDiffPanelRow> rows_;
};

const int match_diff_panel_chooser_t::widths_[] = {
    14,
    16,
    64,
    64,
};

const char* const match_diff_panel_chooser_t::header_[] = {
    "Content",
    "Mode",
    "Files / functions",
    "Purpose",
};

bool show_match_diff_panel(
    const std::filesystem::path& result_path,
    const soff::db::DiffResultSet& results,
    const soff::db::ResultMatch& match)
{
    auto* chooser = new match_diff_panel_chooser_t(result_path, results, match);
    chooser->choose();
    return true;
}

const soff::FunctionFeature* find_function_by_address(
    const soff::ProgramSnapshot& snapshot,
    soff::Address address)
{
    const auto found = std::find_if(
        snapshot.functions.begin(),
        snapshot.functions.end(),
        [address](const soff::FunctionFeature& function) {
            return function.address == address;
        });
    return found == snapshot.functions.end() ? nullptr : &*found;
}

std::string safe_filename_component(std::string text)
{
    if (text.empty()) {
        return "function";
    }
    for (char& ch : text) {
        const auto uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch) == 0 && ch != '_' && ch != '-' && ch != '.') {
            ch = '_';
        }
    }
    if (text.size() > 80) {
        text.resize(80);
    }
    return text;
}

std::filesystem::path default_html_diff_path(
    const std::filesystem::path& result_path,
    const soff::db::ResultMatch& match,
    bool temporary)
{
    std::ostringstream file_name;
    file_name << "soff_diff_"
        << safe_filename_component(match.primary_name)
        << "_vs_"
        << safe_filename_component(match.secondary_name)
        << "_"
        << std::hex << match.primary << "_"
        << match.secondary
        << ".html";
    if (temporary) {
        return std::filesystem::temp_directory_path() / file_name.str();
    }
    const auto parent = result_path.empty() ? std::filesystem::current_path() : result_path.parent_path();
    return parent / file_name.str();
}

std::filesystem::path default_graph_diff_path(
    const std::filesystem::path& result_path,
    const soff::db::ResultMatch& match,
    soff::ui::GraphDiffKind kind,
    bool temporary)
{
    std::ostringstream file_name;
    file_name << "soff_"
        << (kind == soff::ui::GraphDiffKind::native ? "native_graph_" : "microcode_graph_")
        << safe_filename_component(match.primary_name)
        << "_vs_"
        << safe_filename_component(match.secondary_name)
        << "_"
        << std::hex << match.primary << "_"
        << match.secondary
        << ".html";
    if (temporary) {
        return std::filesystem::temp_directory_path() / file_name.str();
    }
    const auto parent = result_path.empty() ? std::filesystem::current_path() : result_path.parent_path();
    return parent / file_name.str();
}

std::filesystem::path default_call_context_path(
    const std::filesystem::path& result_path,
    const soff::db::ResultMatch& match,
    bool temporary)
{
    std::ostringstream file_name;
    file_name << "soff_call_context_"
        << safe_filename_component(match.primary_name)
        << "_vs_"
        << safe_filename_component(match.secondary_name)
        << "_"
        << std::hex << match.primary << "_"
        << match.secondary
        << ".html";
    if (temporary) {
        return std::filesystem::temp_directory_path() / file_name.str();
    }
    const auto parent = result_path.empty() ? std::filesystem::current_path() : result_path.parent_path();
    return parent / file_name.str();
}

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
    bool open_after_save)
{
    try {
        if (const auto error = validate_export_database(results.main_db, "Primary"); !error.empty()) {
            throw soff::Error(soff::ErrorCode::diff_failed, error);
        }
        if (const auto error = validate_export_database(results.diff_db, "Secondary"); !error.empty()) {
            throw soff::Error(soff::ErrorCode::diff_failed, error);
        }

        const auto primary_snapshot = soff::SnapshotRepository{}.load(results.main_db);
        const auto secondary_snapshot = soff::SnapshotRepository{}.load(results.diff_db);
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
    soff::ui::GraphDiffKind kind)
{
    try {
        if (const auto error = validate_export_database(results.main_db, "Primary"); !error.empty()) {
            throw soff::Error(soff::ErrorCode::diff_failed, error);
        }
        if (const auto error = validate_export_database(results.diff_db, "Secondary"); !error.empty()) {
            throw soff::Error(soff::ErrorCode::diff_failed, error);
        }

        const auto primary_snapshot = soff::SnapshotRepository{}.load(results.main_db);
        const auto secondary_snapshot = soff::SnapshotRepository{}.load(results.diff_db);
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
            secondary_context->main_db = results.main_db;
            secondary_context->diff_db = results.diff_db;
            secondary_context->match = match;
            secondary_context->has_match = true;
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
    const soff::db::ResultMatch& match)
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

        const auto primary_snapshot = soff::SnapshotRepository{}.load(results.main_db);
        const auto secondary_snapshot = soff::SnapshotRepository{}.load(results.diff_db);
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
    const soff::db::ResultMatch& match)
{
    try {
        if (const auto error = validate_export_database(results.main_db, "Primary"); !error.empty()) {
            throw soff::Error(soff::ErrorCode::diff_failed, error);
        }
        if (const auto error = validate_export_database(results.diff_db, "Secondary"); !error.empty()) {
            throw soff::Error(soff::ErrorCode::diff_failed, error);
        }

        const auto primary_snapshot = soff::SnapshotRepository{}.load(results.main_db);
        const auto secondary_snapshot = soff::SnapshotRepository{}.load(results.diff_db);
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
    const soff::db::ResultMatch& match)
{
    try {
        if (const auto error = validate_export_database(results.main_db, "Primary"); !error.empty()) {
            throw soff::Error(soff::ErrorCode::diff_failed, error);
        }
        if (const auto error = validate_export_database(results.diff_db, "Secondary"); !error.empty()) {
            throw soff::Error(soff::ErrorCode::diff_failed, error);
        }

        const auto primary_snapshot = soff::SnapshotRepository{}.load(results.main_db);
        const auto secondary_snapshot = soff::SnapshotRepository{}.load(results.diff_db);
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
    const soff::db::ResultMatch& match)
{
    try {
        if (const auto error = validate_export_database(results.main_db, "Primary"); !error.empty()) {
            throw soff::Error(soff::ErrorCode::diff_failed, error);
        }
        if (const auto error = validate_export_database(results.diff_db, "Secondary"); !error.empty()) {
            throw soff::Error(soff::ErrorCode::diff_failed, error);
        }

        const auto primary_snapshot = soff::SnapshotRepository{}.load(results.main_db);
        const auto secondary_snapshot = soff::SnapshotRepository{}.load(results.diff_db);
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
    soff::Address focus_secondary)
{
    try {
        if (const auto error = validate_export_database(results.main_db, "Primary"); !error.empty()) {
            throw soff::Error(soff::ErrorCode::diff_failed, error);
        }
        if (const auto error = validate_export_database(results.diff_db, "Secondary"); !error.empty()) {
            throw soff::Error(soff::ErrorCode::diff_failed, error);
        }

        const auto primary_snapshot = soff::SnapshotRepository{}.load(results.main_db);
        const auto secondary_snapshot = soff::SnapshotRepository{}.load(results.diff_db);
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

void show_result_chooser(const std::filesystem::path& result_path)
{
    if (const auto error = validate_result_database(result_path); !error.empty()) {
        throw soff::Error(soff::ErrorCode::diff_failed, error);
    }
    auto results = soff::db::ResultRepository{}.load(result_path);
    auto* chooser = new diff_results_chooser_t(result_path, std::move(results));
    chooser->choose();
    save_last_result_path(result_path);
}

int run_view_results_ui()
{
    auto default_path = load_last_result_path();
    if (default_path.empty()) {
        DiffUiOptions diff_options;
        load_diff_options(diff_options);
        if (!diff_options.result_db.empty()) {
            default_path = diff_options.result_db;
        }
    }
    const auto default_text = default_path.empty() ? std::string("*.soff") : default_path.string();
    char* selected = ask_file(false, default_text.c_str(), "Select SOFF diff result");
    if (selected == nullptr || selected[0] == '\0') {
        return 0;
    }

    try {
        show_result_chooser(selected);
    } catch (const std::exception& error) {
        warning("Soff failed to load results:\n%s", error.what());
    } catch (...) {
        warning("Soff failed to load results with an unknown error");
    }
    return 1;
}

int run_save_results_ui()
{
    auto source_path = load_last_result_path();
    if (source_path.empty()) {
        DiffUiOptions diff_options;
        load_diff_options(diff_options);
        source_path = diff_options.result_db;
    }
    if (source_path.empty()) {
        warning("Soff has no last diff result path to save");
        return 0;
    }
    if (const auto error = validate_result_database(source_path); !error.empty()) {
        warning("Soff cannot save results:\n%s", error.c_str());
        return 0;
    }

    auto default_output_path = source_path;
    default_output_path.replace_extension(".copy.soff");
    const auto default_output = default_output_path.string();
    char* selected = ask_file(true, default_output.c_str(), "Save SOFF diff result as");
    if (selected == nullptr || selected[0] == '\0') {
        return 0;
    }

    try {
        std::filesystem::copy_file(
            source_path,
            selected,
            std::filesystem::copy_options::overwrite_existing);
        save_last_result_path(selected);
        info("Soff saved diff result:\n%s", selected);
    } catch (const std::exception& error) {
        warning("Soff failed to save results:\n%s", error.what());
    }
    return 1;
}

std::filesystem::path default_result_path_for_import()
{
    if (const char* import_path = env_value("DIAPHORA_IMPORT_RESULTS_FILE")) {
        return import_path;
    }
    if (const char* result_path = env_value("DIAPHORA_RESULTS_FILE")) {
        return result_path;
    }
    auto default_path = load_last_result_path();
    if (!default_path.empty()) {
        return default_path;
    }
    DiffUiOptions diff_options;
    load_diff_options(diff_options);
    if (!diff_options.result_db.empty()) {
        return diff_options.result_db;
    }
    return {};
}

std::string c_declaration_for_apply(std::string declaration)
{
    declaration = trim_copy(std::move(declaration));
    if (declaration.empty()) {
        return declaration;
    }
    if (declaration.find(';') == std::string::npos) {
        declaration.push_back(';');
    }
    return declaration;
}

bool apply_pseudocode_comment(ea_t address, std::uint64_t pseudoitp, const std::string& comment)
{
    func_t* function = get_func(address);
    if (function == nullptr) {
        return false;
    }
    user_cmts_t* comments = restore_user_cmts(function->start_ea);
    if (comments == nullptr) {
        comments = user_cmts_new();
    }
    if (comments == nullptr) {
        return false;
    }

    treeloc_t location;
    location.ea = address;
    location.itp = static_cast<item_preciser_t>(pseudoitp);
    user_cmts_insert(comments, location, citem_cmt_t(comment.c_str()));
    save_user_cmts(function->start_ea, comments);
    user_cmts_free(comments);
    return true;
}

ImportApplySummary apply_import_plan(const soff::ui::ImportPlan& plan)
{
    ImportApplySummary summary;
    for (const auto& item : plan.items) {
        const auto address = static_cast<ea_t>(item.address);
        func_t* function = get_func(address);
        if (item.operation == soff::ui::ImportOperation::rename_function
            || item.operation == soff::ui::ImportOperation::apply_prototype
            || item.operation == soff::ui::ImportOperation::set_function_comment
            || item.operation == soff::ui::ImportOperation::set_function_flags) {
            if (function == nullptr || function->start_ea != address) {
                ++summary.missing_function;
                continue;
            }
        }

        switch (item.operation) {
        case soff::ui::ImportOperation::rename_function: {
            qstring current_name;
            get_func_name(&current_name, function->start_ea);
            if (to_string(current_name) == item.imported_name) {
                ++summary.already_named;
                continue;
            }
            if (set_name(function->start_ea, item.imported_name.c_str(), SN_CHECK | SN_NOWARN)) {
                ++summary.renamed;
            } else {
                ++summary.failed;
            }
            break;
        }
        case soff::ui::ImportOperation::apply_prototype:
            if (apply_cdecl(nullptr, function->start_ea, c_declaration_for_apply(item.value).c_str(), 0)) {
                ++summary.prototypes;
            } else {
                ++summary.failed;
            }
            break;
        case soff::ui::ImportOperation::set_function_comment:
            if (set_func_cmt(function, item.value.c_str(), false)) {
                ++summary.function_comments;
            } else {
                ++summary.failed;
            }
            break;
        case soff::ui::ImportOperation::set_function_flags:
            function->flags = static_cast<decltype(function->flags)>(item.numeric_value);
            if (update_func(function)) {
                ++summary.function_flags;
            } else {
                ++summary.failed;
            }
            break;
        case soff::ui::ImportOperation::set_instruction_comment:
            if (set_cmt(address, item.value.c_str(), false)) {
                ++summary.instruction_comments;
            } else {
                ++summary.failed;
            }
            break;
        case soff::ui::ImportOperation::set_repeatable_instruction_comment:
            if (set_cmt(address, item.value.c_str(), true)) {
                ++summary.instruction_comments;
            } else {
                ++summary.failed;
            }
            break;
        case soff::ui::ImportOperation::set_forced_operand:
            if (set_forced_operand(address, static_cast<int>(item.numeric_value), item.value.c_str())) {
                ++summary.forced_operands;
            } else {
                ++summary.failed;
            }
            break;
        case soff::ui::ImportOperation::set_pseudocode_comment:
            if (apply_pseudocode_comment(address, item.numeric_value, item.value)) {
                ++summary.pseudocode_comments;
            } else {
                ++summary.failed;
            }
            break;
        }
    }
    return summary;
}

bool is_type_definition_item(const soff::ProgramDataItem& item)
{
    return item.type == "structure"
        || item.type == "struct"
        || item.type == "enum"
        || item.type == "union";
}

ImportApplySummary apply_type_definitions(const soff::ProgramSnapshot& secondary, bool import_tils, bool import_definitions)
{
    ImportApplySummary summary;
    if (import_tils) {
        for (const auto& item : secondary.program_data) {
            if (item.type != "til" || item.name.empty()) {
                continue;
            }
            const int result = add_til(item.name.c_str(), ADDTIL_DEFAULT | ADDTIL_SILENT);
            if (result == ADDTIL_OK || result == ADDTIL_COMP) {
                ++summary.type_libraries;
            } else {
                ++summary.failed;
            }
        }
    }

    if (import_definitions) {
        std::unordered_set<std::string> imported_names;
        for (int pass = 0; pass < 3; ++pass) {
            for (const auto& item : secondary.program_data) {
                if (!is_type_definition_item(item) || item.value.empty()) {
                    continue;
                }
                if (!item.name.empty() && imported_names.find(item.name) != imported_names.end()) {
                    continue;
                }
                const int errors = parse_decls(
                    nullptr,
                    item.value.c_str(),
                    nullptr,
                    HTI_DCL | HTI_RELAXED | HTI_SEMICOLON);
                if (errors == 0) {
                    if (item.name.empty() || imported_names.insert(item.name).second) {
                        ++summary.type_definitions;
                    }
                } else if (pass == 2) {
                    ++summary.failed;
                }
            }
        }
    }
    return summary;
}

soff::ui::ImportPlanOptions import_plan_options_from_env()
{
    soff::ui::ImportPlanOptions options;
    options.include_unreliable = parse_bool(
        env_value("DIAPHORA_IMPORT_UNRELIABLE") != nullptr ? env_value("DIAPHORA_IMPORT_UNRELIABLE") : "",
        options.include_unreliable);
    if (const char* minimum_ratio = env_value("DIAPHORA_IMPORT_MIN_RATIO")) {
        options.minimum_ratio = std::stod(minimum_ratio);
    }
    read_bool_env("DIAPHORA_IMPORT_NAMES", options.import_function_names);
    read_bool_env("DIAPHORA_IMPORT_PROTOTYPES", options.import_prototypes);
    read_bool_env("DIAPHORA_IMPORT_COMMENTS", options.import_function_comments);
    read_bool_env("DIAPHORA_IMPORT_FLAGS", options.import_function_flags);
    read_bool_env("DIAPHORA_IMPORT_INSTRUCTION_COMMENTS", options.import_instruction_comments);
    read_bool_env("DIAPHORA_IMPORT_FORCED_OPERANDS", options.import_forced_operands);
    read_bool_env("DIAPHORA_IMPORT_PSEUDOCODE_COMMENTS", options.import_pseudocode_comments);
    return options;
}

std::string import_plan_counts_text(const soff::ui::ImportPlan& plan)
{
    std::ostringstream out;
    out << "Renames=" << plan.function_renames
        << " prototypes=" << plan.prototypes
        << " function_comments=" << plan.function_comments
        << " flags=" << plan.function_flags << '\n'
        << "instruction_comments=" << plan.instruction_comments
        << " repeatable_instruction_comments=" << plan.repeatable_instruction_comments
        << " forced_operands=" << plan.forced_operands
        << " pseudocode_comments=" << plan.pseudocode_comments << '\n'
        << "Skipped: same_name=" << plan.skipped_same_name
        << " auto_name=" << plan.skipped_auto_name
        << " kind=" << plan.skipped_kind
        << " ratio=" << plan.skipped_ratio
        << " empty_name=" << plan.skipped_empty_name
        << " missing_source=" << plan.skipped_missing_source;
    return out.str();
}

std::string import_apply_counts_text(
    const ImportApplySummary& summary,
    const ImportApplySummary* definition_summary,
    std::size_t candidates)
{
    std::ostringstream out;
    out << "Renamed=" << summary.renamed
        << " already_named=" << summary.already_named
        << " prototypes=" << summary.prototypes
        << " function_comments=" << summary.function_comments
        << " flags=" << summary.function_flags << '\n'
        << "instruction_comments=" << summary.instruction_comments
        << " forced_operands=" << summary.forced_operands
        << " pseudocode_comments=" << summary.pseudocode_comments;
    if (definition_summary != nullptr) {
        out << " type_libraries=" << definition_summary->type_libraries
            << " type_definitions=" << definition_summary->type_definitions;
    }
    const auto failed = summary.failed + (definition_summary != nullptr ? definition_summary->failed : 0);
    out << " missing_function=" << summary.missing_function
        << " failed=" << failed << '\n'
        << "Candidates=" << candidates;
    return out.str();
}

bool refresh_primary_export_after_import(
    const soff::db::DiffResultSet& results,
    bool allow_prompt,
    const char* context)
{
    if (results.main_db.empty()) {
        msg("Soff: cannot refresh primary export DB after %s; result has no main_db path\n", context);
        return false;
    }
    if (const auto error = validate_export_database(results.main_db, "Primary"); !error.empty()) {
        msg("Soff: cannot refresh primary export DB after %s: %s\n", context, error.c_str());
        return false;
    }

    const char* refresh_env = env_value("DIAPHORA_IMPORT_REFRESH_DB");
    bool refresh = refresh_env != nullptr ? parse_bool(refresh_env, false) : false;
    if (refresh_env == nullptr && allow_prompt) {
        const int choice = ask_buttons(
            "~R~efresh primary DB",
            "~S~kip",
            "~C~ancel",
            ASKBTN_NO,
            "Soff imported metadata into the current IDB.\n\n"
            "Refresh the primary export DB so future diff/import uses the updated names, types and comments?\n\n%s",
            results.main_db.c_str());
        refresh = choice == ASKBTN_YES;
    }
    if (!refresh) {
        return false;
    }

    try {
        ExportOptions options;
        std::filesystem::path saved_output;
        load_dialog_options(saved_output, options);
        apply_env_overrides(options);
        options.resume_existing_database = false;
        const auto refreshed = export_current_idb(results.main_db, options);
        const auto summary = export_summary_text(refreshed, results.main_db);
        info("Soff primary export DB refreshed after import.\n%s", summary.c_str());
        return true;
    } catch (const std::exception& error) {
        warning("Soff failed to refresh primary export DB after import:\n%s", error.what());
    } catch (...) {
        warning("Soff failed to refresh primary export DB after import with an unknown error");
    }
    return false;
}

SelectedChooserEditResult import_selected_matches_from_chooser(
    const std::filesystem::path& result_path,
    const soff::db::DiffResultSet& results,
    const std::vector<ResultChooserRow>& rows,
    const sizevec_t& selected_rows)
{
    soff::db::DiffResultSet selected;
    selected.main_db = results.main_db;
    selected.diff_db = results.diff_db;
    selected.version = results.version;
    selected.date = results.date;

    std::unordered_set<int> seen_matches;
    for (const auto row_index : selected_rows) {
        if (row_index >= rows.size()) {
            continue;
        }
        const auto match_index = rows[row_index].match_index;
        if (match_index < 0 || static_cast<std::size_t>(match_index) >= results.matches.size()) {
            continue;
        }
        if (!seen_matches.insert(match_index).second) {
            continue;
        }
        selected.matches.push_back(results.matches[static_cast<std::size_t>(match_index)]);
    }

    if (selected.matches.empty()) {
        info("Soff selected rows do not contain importable matches");
        return SelectedChooserEditResult::unchanged;
    }

    const auto options = import_plan_options_from_env();
    soff::ui::ImportPlan plan;
    const auto main_db_error = validate_export_database(selected.main_db, "Primary");
    const auto diff_db_error = validate_export_database(selected.diff_db, "Secondary");
    if (main_db_error.empty() && diff_db_error.empty()) {
        const auto primary_snapshot = soff::SnapshotRepository{}.load(selected.main_db);
        const auto secondary_snapshot = soff::SnapshotRepository{}.load(selected.diff_db);
        plan = soff::ui::build_import_plan(selected, primary_snapshot, secondary_snapshot, options);
    } else {
        msg(
            "Soff: selected import is falling back to result names only: primary=%s secondary=%s\n",
            main_db_error.c_str(),
            diff_db_error.c_str());
        plan = soff::ui::build_import_plan(selected, options);
    }

    if (plan.items.empty()) {
        const auto counts = import_plan_counts_text(plan);
        info(
            "Soff selected import found no candidates for %zu selected match(es).\n%s",
            selected.matches.size(),
            counts.c_str());
        return SelectedChooserEditResult::unchanged;
    }

    const auto counts = import_plan_counts_text(plan);
    const int choice = ask_buttons(
        "~I~mport metadata",
        "~J~ump only",
        "~C~ancel",
        ASKBTN_YES,
        "Import metadata for %zu selected match(es)?\n\n%s\n\n"
        "This modifies the current IDB.",
        selected.matches.size(),
        counts.c_str());
    if (choice == ASKBTN_NO) {
        return SelectedChooserEditResult::jump;
    }
    if (choice != ASKBTN_YES) {
        return SelectedChooserEditResult::unchanged;
    }

    const auto summary = apply_import_plan(plan);
    const auto applied = import_apply_counts_text(summary, nullptr, plan.items.size());
    info("Soff selected import complete.\n%s", applied.c_str());
    refresh_primary_export_after_import(results, true, "selected import");
    save_last_result_path(result_path);
    return SelectedChooserEditResult::imported;
}

int run_import_results_ui()
{
    const auto default_path = default_result_path_for_import();
    const bool env_driven = env_value("DIAPHORA_IMPORT_RESULTS_FILE") != nullptr
        || env_value("DIAPHORA_RESULTS_FILE") != nullptr;
    std::filesystem::path result_path = default_path;
    if (!env_driven) {
        const auto default_text = default_path.empty() ? std::string("*.soff") : default_path.string();
        char* selected = ask_file(false, default_text.c_str(), "Select SOFF diff result to import");
        if (selected == nullptr || selected[0] == '\0') {
            return 0;
        }
        result_path = selected;
    }

    try {
        if (const auto error = validate_result_database(result_path); !error.empty()) {
            throw soff::Error(soff::ErrorCode::diff_failed, error);
        }
        const auto results = soff::db::ResultRepository{}.load(result_path);
        const auto options = import_plan_options_from_env();
        bool import_tils = true;
        bool import_definitions = true;
        read_bool_env("DIAPHORA_IMPORT_TIL", import_tils);
        read_bool_env("DIAPHORA_IMPORT_DEFINITIONS", import_definitions);

        soff::ui::ImportPlan plan;
        ImportApplySummary definition_summary;
        const auto main_db_error = validate_export_database(results.main_db, "Primary");
        const auto diff_db_error = validate_export_database(results.diff_db, "Secondary");
        if (main_db_error.empty() && diff_db_error.empty()) {
            const auto primary_snapshot = soff::SnapshotRepository{}.load(results.main_db);
            const auto secondary_snapshot = soff::SnapshotRepository{}.load(results.diff_db);
            definition_summary = apply_type_definitions(secondary_snapshot, import_tils, import_definitions);
            plan = soff::ui::build_import_plan(results, primary_snapshot, secondary_snapshot, options);
        } else {
            plan = soff::ui::build_import_plan(results, options);
        }
        if (plan.items.empty()) {
            info(
                "Soff import found no candidates.\n"
                "Skipped: same_name=%zu auto_name=%zu kind=%zu ratio=%zu empty_name=%zu missing_source=%zu",
                plan.skipped_same_name,
                plan.skipped_auto_name,
                plan.skipped_kind,
                plan.skipped_ratio,
                plan.skipped_empty_name,
                plan.skipped_missing_source);
            save_last_result_path(result_path);
            return 1;
        }

        const bool auto_accept = parse_bool(
            env_value("DIAPHORA_IMPORT_AUTO") != nullptr ? env_value("DIAPHORA_IMPORT_AUTO") : "",
            false);
        if (!auto_accept) {
            const int choice = ask_buttons(
                "~I~mport names",
                "~V~iew only",
                "~C~ancel",
                ASKBTN_YES,
                "Soff import plan for:\n%s\n\n"
                "Renames=%zu prototypes=%zu function_comments=%zu flags=%zu\n"
                "instruction_comments=%zu repeatable_instruction_comments=%zu forced_operands=%zu pseudocode_comments=%zu\n"
                "Skipped: same_name=%zu auto_name=%zu kind=%zu ratio=%zu empty_name=%zu missing_source=%zu\n\n"
                "This modifies the current IDB.",
                result_path.string().c_str(),
                plan.function_renames,
                plan.prototypes,
                plan.function_comments,
                plan.function_flags,
                plan.instruction_comments,
                plan.repeatable_instruction_comments,
                plan.forced_operands,
                plan.pseudocode_comments,
                plan.skipped_same_name,
                plan.skipped_auto_name,
                plan.skipped_kind,
                plan.skipped_ratio,
                plan.skipped_empty_name,
                plan.skipped_missing_source);
            if (choice == ASKBTN_CANCEL) {
                return 0;
            }
            if (choice == ASKBTN_NO) {
                show_result_chooser(result_path);
                return 1;
            }
        }

        const auto summary = apply_import_plan(plan);
        save_last_result_path(result_path);
        const auto applied = import_apply_counts_text(summary, &definition_summary, plan.items.size());
        info("Soff import complete.\n%s", applied.c_str());
        refresh_primary_export_after_import(results, !auto_accept && !env_driven, "bulk import");
    } catch (const std::exception& error) {
        warning("Soff import failed:\n%s", error.what());
    } catch (...) {
        warning("Soff import failed with an unknown error");
    }
    return 1;
}

int run_export_ui()
{
    ExportOptions options;
    std::filesystem::path output_path;
    const bool env_driven = env_value("DIAPHORA_EXPORT_FILE") != nullptr;
    if (const char* env_output = env_value("DIAPHORA_EXPORT_FILE")) {
        apply_env_overrides(options);
        output_path = env_output;
    } else {
        load_dialog_options(output_path, options);
        apply_env_overrides(options);
        if (!ask_export_options(output_path, options)) {
            return 0;
        }
        save_dialog_options(output_path, options);
    }

    try {
        const auto result = export_current_idb(output_path, options);
        const auto summary = export_summary_text(result, output_path);
        info("Soff export complete.\n%s", summary.c_str());
    } catch (const std::exception& error) {
        warning("Soff export failed:\n%s", error.what());
    } catch (...) {
        warning("Soff export failed with an unknown error");
    }
    return 1;
}

int run_diff_ui()
{
    DiffUiOptions options;
    load_diff_options(options);
    const bool has_env_primary = env_value("DIAPHORA_DB1") != nullptr;
    apply_diff_env_overrides(options);
    if (!has_env_primary) {
        options.main_db = default_export_path().string();
    }
    if (options.result_db.empty() && !options.main_db.empty()) {
        options.result_db = default_diff_result_path(options.main_db).string();
    }

    const bool env_driven = env_value("DIAPHORA_DB1") != nullptr
        && (env_value("DIAPHORA_DB2") != nullptr || env_value("DIAPHORA_FILE_IN") != nullptr)
        && env_value("DIAPHORA_DIFF_OUT") != nullptr;
    if (!env_driven) {
        if (!ask_diff_options(options)) {
            return 0;
        }
        save_diff_options(options);
    }

    try {
        const auto summary = diff_databases(options);
        info(
            "Soff diff complete.\nResult:\n%s\nbest=%zu partial=%zu unreliable=%zu multimatch=%zu unmatched=%zu/%zu",
            options.result_db.c_str(),
            summary.results.best,
            summary.results.partial,
            summary.results.unreliable,
            summary.results.multimatch,
            summary.results.unmatched_primary,
            summary.results.unmatched_secondary);
        show_result_chooser(options.result_db);
    } catch (const std::exception& error) {
        warning("Soff diff failed:\n%s", error.what());
    } catch (...) {
        warning("Soff diff failed with an unknown error");
    }
    return 1;
}

ea_t default_local_diff_primary()
{
    func_t* cursor_function = get_func(get_screen_ea());
    if (cursor_function != nullptr) {
        return cursor_function->start_ea;
    }
    func_t* first = getn_func(0);
    return first != nullptr ? first->start_ea : BADADDR;
}

ea_t default_local_diff_secondary(ea_t primary_ea)
{
    if (primary_ea != BADADDR) {
        if (func_t* next = get_next_func(primary_ea); next != nullptr) {
            return next->start_ea;
        }
    }
    const auto function_count = get_func_qty();
    for (std::size_t i = 0; i < function_count; ++i) {
        func_t* function = getn_func(i);
        if (function != nullptr && function->start_ea != primary_ea) {
            return function->start_ea;
        }
    }
    return BADADDR;
}

bool read_local_diff_env_address(const char* primary_name, const char* fallback_name, ea_t& value)
{
    const char* text = env_value(primary_name);
    if (text == nullptr) {
        text = env_value(fallback_name);
    }
    if (text == nullptr) {
        return false;
    }
    value = static_cast<ea_t>(std::stoull(text, nullptr, 0));
    return true;
}

std::filesystem::path default_local_diff_path(
    const soff::FunctionFeature& primary,
    const soff::FunctionFeature& secondary,
    std::string_view suffix)
{
    std::ostringstream file_name;
    file_name << "soff_local_"
        << suffix
        << "_"
        << safe_filename_component(primary.name)
        << "_vs_"
        << safe_filename_component(secondary.name)
        << "_"
        << std::hex << primary.address << "_"
        << secondary.address
        << ".html";
    return std::filesystem::temp_directory_path() / file_name.str();
}

int run_local_diff_ui()
{
    ea_t primary_ea = default_local_diff_primary();
    ea_t secondary_ea = default_local_diff_secondary(primary_ea);
    ExportOptions export_options;
    std::filesystem::path ignored_output;
    load_dialog_options(ignored_output, export_options);
    apply_env_overrides(export_options);

    const bool env_driven = read_local_diff_env_address("SOFF_LOCAL_DIFF_EA1", "DIAPHORA_LOCAL_DIFF_EA1", primary_ea)
        | read_local_diff_env_address("SOFF_LOCAL_DIFF_EA2", "DIAPHORA_LOCAL_DIFF_EA2", secondary_ea);
    read_bool_env("SOFF_LOCAL_DIFF_DECOMPILER", export_options.use_decompiler);
    read_bool_env("DIAPHORA_LOCAL_DIFF_DECOMPILER", export_options.use_decompiler);

    if (!env_driven) {
        ushort checks = export_options.use_decompiler ? 1 : 0;
        static const char form[] =
            "STARTITEM 0\n"
            "BUTTON YES* Diff\n"
            "BUTTON CANCEL Cancel\n"
            "Soff local function diff\n"
            "\n"
            "<~P~rimary function:$:18:18::> <~S~econdary function:$:18:18::>\n"
            "<Use decompiler:C>>\n";
        if (ask_form(form, &primary_ea, &secondary_ea, &checks) <= 0) {
            return 0;
        }
        export_options.use_decompiler = (checks & 1) != 0;
    }

    try {
        func_t* primary_function = get_func(primary_ea);
        func_t* secondary_function = get_func(secondary_ea);
        if (primary_function == nullptr) {
            throw soff::Error(soff::ErrorCode::diff_failed, "primary address is not inside a function");
        }
        if (secondary_function == nullptr) {
            throw soff::Error(soff::ErrorCode::diff_failed, "secondary address is not inside a function");
        }
        if (primary_function->start_ea == secondary_function->start_ea) {
            throw soff::Error(soff::ErrorCode::diff_failed, "local diff needs two different functions");
        }

        HexRaysExportContext hexrays;
        hexrays.requested = export_options.use_decompiler;
        if (hexrays.requested) {
            hexrays.available = initialize_hexrays();
            msg(
                "Soff: Hex-Rays decompiler %s for local diff\n",
                hexrays.available ? "available" : "not available; pseudocode/microcode will be empty");
        }
        const auto imagebase = get_imagebase();
        auto primary = read_function_feature(primary_function, imagebase, &hexrays);
        auto secondary = read_function_feature(secondary_function, imagebase, &hexrays);

        const int view_choice = env_driven
            ? ASKBTN_YES
            : ask_buttons(
                "~T~ext diff",
                "~N~ative graph",
                "~M~icrocode graph",
                ASKBTN_YES,
                "Choose local diff view for:\n%s -> %s",
                primary.name.c_str(),
                secondary.name.c_str());
        std::string html;
        std::filesystem::path output_path;
        if (view_choice == ASKBTN_NO) {
            html = soff::ui::render_function_graph_diff_html(
                primary,
                secondary,
                soff::ui::GraphDiffKind::native,
                0.0,
                "Local function native graph diff");
            output_path = default_local_diff_path(primary, secondary, "native_graph");
        } else if (view_choice == ASKBTN_CANCEL) {
            if (primary.microcode_blocks.empty() && secondary.microcode_blocks.empty()) {
                throw soff::Error(soff::ErrorCode::diff_failed, "microcode graph is empty; enable decompiler and ensure Hex-Rays is available");
            }
            html = soff::ui::render_function_graph_diff_html(
                primary,
                secondary,
                soff::ui::GraphDiffKind::microcode,
                0.0,
                "Local function microcode graph diff");
            output_path = default_local_diff_path(primary, secondary, "microcode_graph");
        } else if (view_choice == ASKBTN_YES) {
            const auto document = soff::ui::build_function_diff_document(
                primary,
                secondary,
                0.0,
                "Local function diff");
            html = soff::ui::render_html_diff(document);
            output_path = default_local_diff_path(primary, secondary, "text");
        } else {
            return 0;
        }

        if (!write_text_file(output_path, html)) {
            throw soff::Error(soff::ErrorCode::diff_failed, "failed to write local diff HTML: " + output_path.string());
        }
        if (!open_file_with_shell(output_path)) {
            throw soff::Error(soff::ErrorCode::diff_failed, "failed to open local diff HTML: " + output_path.string());
        }
        msg("Soff: opened local diff %s\n", output_path.string().c_str());
    } catch (const std::exception& error) {
        warning("Soff local diff failed:\n%s", error.what());
    } catch (...) {
        warning("Soff local diff failed with an unknown error");
    }
    return 1;
}

GraphDiffNode* active_graph_diff_node_info()
{
    auto* context = active_graph_diff_context();
    const auto node = active_graph_diff_node();
    if (context == nullptr || node < 0 || static_cast<std::size_t>(node) >= context->nodes.size()) {
        return nullptr;
    }
    return &context->nodes[static_cast<std::size_t>(node)];
}

soff::db::DiffResultSet active_graph_result_set()
{
    soff::db::DiffResultSet selected;
    if (auto* context = active_graph_diff_context(); context != nullptr) {
        selected.main_db = context->main_db;
        selected.diff_db = context->diff_db;
        selected.matches.push_back(context->match);
    }
    return selected;
}

bool is_block_level_import_operation(soff::ui::ImportOperation operation)
{
    return operation == soff::ui::ImportOperation::set_instruction_comment
        || operation == soff::ui::ImportOperation::set_repeatable_instruction_comment
        || operation == soff::ui::ImportOperation::set_forced_operand
        || operation == soff::ui::ImportOperation::set_pseudocode_comment;
}

void keep_active_graph_block_items(soff::ui::ImportPlan& plan, const GraphDiffNode& node)
{
    std::unordered_set<soff::Address> primary_addresses(node.primary_instructions.begin(), node.primary_instructions.end());
    if (node.primary != 0) {
        primary_addresses.insert(node.primary);
    }
    if (primary_addresses.empty()) {
        plan.items.clear();
        return;
    }

    plan.items.erase(
        std::remove_if(
            plan.items.begin(),
            plan.items.end(),
            [&](const soff::ui::ImportPlanItem& item) {
                return !is_block_level_import_operation(item.operation)
                    || primary_addresses.find(item.address) == primary_addresses.end();
            }),
        plan.items.end());
}

bool sync_active_graph_node()
{
    auto* context = active_graph_diff_context();
    const auto node = active_graph_diff_node();
    if (context == nullptr || node < 0) {
        warning("Soff graph diff has no active node to synchronize");
        return false;
    }
    sync_peer_graph_node(*context, node);
    return true;
}

bool jump_active_graph_primary()
{
    const auto* node = active_graph_diff_node_info();
    if (node == nullptr || node->primary == 0 || !is_mapped(static_cast<ea_t>(node->primary))) {
        warning("Soff graph node has no mapped primary IDB address");
        return false;
    }
    jumpto(static_cast<ea_t>(node->primary));
    return true;
}

bool show_active_graph_text_diff()
{
    auto* context = active_graph_diff_context();
    if (context == nullptr || !context->has_match) {
        warning("Soff graph diff has no match context");
        return false;
    }
    const int text_choice = ask_buttons(
        "~A~ssembly",
        "~P~seudocode",
        "~M~icrocode",
        ASKBTN_YES,
        "Show text diff for:\n%s -> %s",
        context->match.primary_name.c_str(),
        context->match.secondary_name.c_str());
    if (text_choice != ASKBTN_YES && text_choice != ASKBTN_NO && text_choice != ASKBTN_CANCEL) {
        return false;
    }
    auto results = active_graph_result_set();
    results.matches.clear();
    const auto* node = active_graph_diff_node_info();
    return show_ida_match_text_diff(
        results,
        context->match,
        text_choice == ASKBTN_YES
            ? TextDiffKind::assembly
            : (text_choice == ASKBTN_NO ? TextDiffKind::pseudocode : TextDiffKind::microcode),
        node != nullptr ? node->primary : 0,
        node != nullptr ? node->secondary : 0);
}

bool import_active_graph_match()
{
    auto* context = active_graph_diff_context();
    if (context == nullptr || !context->has_match) {
        warning("Soff graph diff has no match context to import");
        return false;
    }
    const auto* active_node = active_graph_diff_node_info();
    if (active_node == nullptr) {
        warning("Soff graph diff has no active block node to import");
        return false;
    }

    soff::db::DiffResultSet selected;
    selected.main_db = context->main_db;
    selected.diff_db = context->diff_db;
    selected.matches.push_back(context->match);

    soff::ui::ImportPlan plan;
    const auto options = import_plan_options_from_env();
    const auto main_db_error = validate_export_database(selected.main_db, "Primary");
    const auto diff_db_error = validate_export_database(selected.diff_db, "Secondary");
    if (main_db_error.empty() && diff_db_error.empty()) {
        const auto primary_snapshot = soff::SnapshotRepository{}.load(selected.main_db);
        const auto secondary_snapshot = soff::SnapshotRepository{}.load(selected.diff_db);
        plan = soff::ui::build_import_plan(selected, primary_snapshot, secondary_snapshot, options);
    } else {
        plan = soff::ui::build_import_plan(selected, options);
    }
    keep_active_graph_block_items(plan, *active_node);
    if (plan.items.empty()) {
        info(
            "Soff graph block import found no instruction/comment candidates for primary block %s.",
            address_hex(active_node->primary).c_str());
        return false;
    }

    const int choice = ask_buttons(
        "~I~mport block metadata",
        "~C~ancel",
        nullptr,
        ASKBTN_YES,
        "Import %zu instruction/comment item(s) for primary block %s?\n\n"
        "This modifies the current IDB.",
        plan.items.size(),
        address_hex(active_node->primary).c_str());
    if (choice != ASKBTN_YES) {
        return false;
    }
    const auto summary = apply_import_plan(plan);
    const auto applied = import_apply_counts_text(summary, nullptr, plan.items.size());
    info("Soff graph import complete.\n%s", applied.c_str());
    return true;
}

struct graph_sync_peer_handler_t : public action_handler_t
{
    int idaapi activate(action_activation_ctx_t*) override
    {
        return sync_active_graph_node() ? 1 : 0;
    }

    action_state_t idaapi update(action_update_ctx_t*) override
    {
        return active_graph_diff_context() != nullptr ? AST_ENABLE_FOR_IDB : AST_DISABLE_FOR_IDB;
    }
};

struct graph_jump_primary_handler_t : public action_handler_t
{
    int idaapi activate(action_activation_ctx_t*) override
    {
        return jump_active_graph_primary() ? 1 : 0;
    }

    action_state_t idaapi update(action_update_ctx_t*) override
    {
        return active_graph_diff_node_info() != nullptr ? AST_ENABLE_FOR_IDB : AST_DISABLE_FOR_IDB;
    }
};

struct graph_text_diff_handler_t : public action_handler_t
{
    int idaapi activate(action_activation_ctx_t*) override
    {
        return show_active_graph_text_diff() ? 1 : 0;
    }

    action_state_t idaapi update(action_update_ctx_t*) override
    {
        auto* context = active_graph_diff_context();
        return context != nullptr && context->has_match ? AST_ENABLE_FOR_IDB : AST_DISABLE_FOR_IDB;
    }
};

struct graph_import_match_handler_t : public action_handler_t
{
    int idaapi activate(action_activation_ctx_t*) override
    {
        return import_active_graph_match() ? 1 : 0;
    }

    action_state_t idaapi update(action_update_ctx_t*) override
    {
        auto* context = active_graph_diff_context();
        return context != nullptr && context->has_match ? AST_ENABLE_FOR_IDB : AST_DISABLE_FOR_IDB;
    }
};

struct export_handler_t : public action_handler_t
{
    int idaapi activate(action_activation_ctx_t*) override
    {
        return run_export_ui();
    }

    action_state_t idaapi update(action_update_ctx_t*) override
    {
        return AST_ENABLE_FOR_IDB;
    }
};

struct diff_handler_t : public action_handler_t
{
    int idaapi activate(action_activation_ctx_t*) override
    {
        return run_diff_ui();
    }

    action_state_t idaapi update(action_update_ctx_t*) override
    {
        return AST_ENABLE_FOR_IDB;
    }
};

struct view_results_handler_t : public action_handler_t
{
    int idaapi activate(action_activation_ctx_t*) override
    {
        return run_view_results_ui();
    }

    action_state_t idaapi update(action_update_ctx_t*) override
    {
        return AST_ENABLE_FOR_IDB;
    }
};

struct save_results_handler_t : public action_handler_t
{
    int idaapi activate(action_activation_ctx_t*) override
    {
        return run_save_results_ui();
    }

    action_state_t idaapi update(action_update_ctx_t*) override
    {
        return AST_ENABLE_FOR_IDB;
    }
};

struct import_results_handler_t : public action_handler_t
{
    int idaapi activate(action_activation_ctx_t*) override
    {
        return run_import_results_ui();
    }

    action_state_t idaapi update(action_update_ctx_t*) override
    {
        return AST_ENABLE_FOR_IDB;
    }
};

struct local_diff_handler_t : public action_handler_t
{
    int idaapi activate(action_activation_ctx_t*) override
    {
        return run_local_diff_ui();
    }

    action_state_t idaapi update(action_update_ctx_t*) override
    {
        return AST_ENABLE_FOR_IDB;
    }
};

struct soff_plugin_t : public plugmod_t
{
    soff_plugin_t()
    {
        menu_created_ = create_menu(menu_id, menu_label, "Options");
        if (!menu_created_) {
            msg("Soff: failed to create top-level menu %s\n", menu_label);
        }

        auto* graph_sync_handler = new graph_sync_peer_handler_t();
        action_desc_t graph_sync_action = ACTION_DESC_LITERAL_OWNER(
            graph_sync_action_name,
            "Sync paired graph node",
            graph_sync_handler,
            this,
            nullptr,
            "Center the paired node in the other SOFF graph diff viewer",
            -1,
            ADF_OT_PLUGMOD | ADF_OWN_HANDLER | ADF_NO_UNDO);
        if (register_action(graph_sync_action)) {
            graph_sync_registered_ = true;
        } else {
            delete graph_sync_handler;
            msg("Soff: failed to register action %s\n", graph_sync_action_name);
        }

        auto* graph_jump_handler = new graph_jump_primary_handler_t();
        action_desc_t graph_jump_action = ACTION_DESC_LITERAL_OWNER(
            graph_jump_action_name,
            "Jump to primary block",
            graph_jump_handler,
            this,
            nullptr,
            "Jump to the primary IDB address for this graph diff node",
            -1,
            ADF_OT_PLUGMOD | ADF_OWN_HANDLER | ADF_NO_UNDO);
        if (register_action(graph_jump_action)) {
            graph_jump_registered_ = true;
        } else {
            delete graph_jump_handler;
            msg("Soff: failed to register action %s\n", graph_jump_action_name);
        }

        auto* graph_text_handler = new graph_text_diff_handler_t();
        action_desc_t graph_text_action = ACTION_DESC_LITERAL_OWNER(
            graph_text_action_name,
            "Open text diff",
            graph_text_handler,
            this,
            nullptr,
            "Open assembly, pseudocode, or microcode text diff for this match",
            -1,
            ADF_OT_PLUGMOD | ADF_OWN_HANDLER | ADF_NO_UNDO);
        if (register_action(graph_text_action)) {
            graph_text_registered_ = true;
        } else {
            delete graph_text_handler;
            msg("Soff: failed to register action %s\n", graph_text_action_name);
        }

        auto* graph_import_handler = new graph_import_match_handler_t();
        action_desc_t graph_import_action = ACTION_DESC_LITERAL_OWNER(
            graph_import_action_name,
            "Import block metadata",
            graph_import_handler,
            this,
            nullptr,
            "Import comments, forced operands, and pseudocode comments for this graph block",
            -1,
            ADF_OT_PLUGMOD | ADF_OWN_HANDLER | ADF_NO_UNDO);
        if (register_action(graph_import_action)) {
            graph_import_registered_ = true;
        } else {
            delete graph_import_handler;
            msg("Soff: failed to register action %s\n", graph_import_action_name);
        }

        auto* export_handler = new export_handler_t();
        action_desc_t export_action = ACTION_DESC_LITERAL_OWNER(
            export_action_name,
            export_action_label,
            export_handler,
            this,
            nullptr,
            "Export current IDB to SOFF/Diaphora-compatible SQLite",
            -1,
            ADF_OT_PLUGMOD | ADF_OWN_HANDLER | ADF_NO_UNDO);
        if (register_action(export_action)) {
            export_registered_ = true;
            if (!attach_action_to_menu(export_menu_path, export_action_name, SETMENU_APP)
                && !attach_action_to_menu(menu_path, export_action_name, SETMENU_APP)) {
                msg("Soff: failed to attach action %s to menu\n", export_action_name);
            }
        } else {
            delete export_handler;
            msg("Soff: failed to register action %s\n", export_action_name);
        }

        auto* diff_handler = new diff_handler_t();
        action_desc_t diff_action = ACTION_DESC_LITERAL_OWNER(
            diff_action_name,
            diff_action_label,
            diff_handler,
            this,
            nullptr,
            "Diff two SOFF/Diaphora-compatible SQLite exports",
            -1,
            ADF_OT_PLUGMOD | ADF_OWN_HANDLER | ADF_NO_UNDO);
        if (register_action(diff_action)) {
            diff_registered_ = true;
            if (!attach_action_to_menu(diff_menu_path, diff_action_name, SETMENU_APP)
                && !attach_action_to_menu(menu_path, diff_action_name, SETMENU_APP)) {
                msg("Soff: failed to attach action %s to menu\n", diff_action_name);
            }
        } else {
            delete diff_handler;
            msg("Soff: failed to register action %s\n", diff_action_name);
        }

        auto* view_results_handler = new view_results_handler_t();
        action_desc_t view_results_action = ACTION_DESC_LITERAL_OWNER(
            view_results_action_name,
            view_results_action_label,
            view_results_handler,
            this,
            nullptr,
            "Load an existing SOFF/Diaphora diff result database",
            -1,
            ADF_OT_PLUGMOD | ADF_OWN_HANDLER | ADF_NO_UNDO);
        if (register_action(view_results_action)) {
            view_results_registered_ = true;
            if (!attach_action_to_menu(view_results_menu_path, view_results_action_name, SETMENU_APP)
                && !attach_action_to_menu(menu_path, view_results_action_name, SETMENU_APP)) {
                msg("Soff: failed to attach action %s to menu\n", view_results_action_name);
            }
        } else {
            delete view_results_handler;
            msg("Soff: failed to register action %s\n", view_results_action_name);
        }

        auto* save_results_handler = new save_results_handler_t();
        action_desc_t save_results_action = ACTION_DESC_LITERAL_OWNER(
            save_results_action_name,
            save_results_action_label,
            save_results_handler,
            this,
            nullptr,
            "Save the last loaded/generated SOFF diff result database",
            -1,
            ADF_OT_PLUGMOD | ADF_OWN_HANDLER | ADF_NO_UNDO);
        if (register_action(save_results_action)) {
            save_results_registered_ = true;
            if (!attach_action_to_menu(save_results_menu_path, save_results_action_name, SETMENU_APP)
                && !attach_action_to_menu(menu_path, save_results_action_name, SETMENU_APP)) {
                msg("Soff: failed to attach action %s to menu\n", save_results_action_name);
            }
        } else {
            delete save_results_handler;
            msg("Soff: failed to register action %s\n", save_results_action_name);
        }

        auto* import_results_handler = new import_results_handler_t();
        action_desc_t import_results_action = ACTION_DESC_LITERAL_OWNER(
            import_results_action_name,
            import_results_action_label,
            import_results_handler,
            this,
            nullptr,
            "Import function names from a SOFF/Diaphora diff result database",
            -1,
            ADF_OT_PLUGMOD | ADF_OWN_HANDLER | ADF_NO_UNDO);
        if (register_action(import_results_action)) {
            import_results_registered_ = true;
            if (!attach_action_to_menu(import_results_menu_path, import_results_action_name, SETMENU_APP)
                && !attach_action_to_menu(menu_path, import_results_action_name, SETMENU_APP)) {
                msg("Soff: failed to attach action %s to menu\n", import_results_action_name);
            }
        } else {
            delete import_results_handler;
            msg("Soff: failed to register action %s\n", import_results_action_name);
        }

        auto* local_diff_handler = new local_diff_handler_t();
        action_desc_t local_diff_action = ACTION_DESC_LITERAL_OWNER(
            local_diff_action_name,
            local_diff_action_label,
            local_diff_handler,
            this,
            nullptr,
            "Diff two functions in the current IDB",
            -1,
            ADF_OT_PLUGMOD | ADF_OWN_HANDLER | ADF_NO_UNDO);
        if (register_action(local_diff_action)) {
            local_diff_registered_ = true;
            if (!attach_action_to_menu(local_diff_menu_path, local_diff_action_name, SETMENU_APP)
                && !attach_action_to_menu(menu_path, local_diff_action_name, SETMENU_APP)) {
                msg("Soff: failed to attach action %s to menu\n", local_diff_action_name);
            }
        } else {
            delete local_diff_handler;
            msg("Soff: failed to register action %s\n", local_diff_action_name);
        }
    }

    ~soff_plugin_t() override
    {
        if (local_diff_registered_) {
            detach_action_from_menu(local_diff_menu_path, local_diff_action_name);
            detach_action_from_menu(menu_path, local_diff_action_name);
            unregister_action(local_diff_action_name);
        }
        if (import_results_registered_) {
            detach_action_from_menu(import_results_menu_path, import_results_action_name);
            detach_action_from_menu(menu_path, import_results_action_name);
            unregister_action(import_results_action_name);
        }
        if (save_results_registered_) {
            detach_action_from_menu(save_results_menu_path, save_results_action_name);
            detach_action_from_menu(menu_path, save_results_action_name);
            unregister_action(save_results_action_name);
        }
        if (view_results_registered_) {
            detach_action_from_menu(view_results_menu_path, view_results_action_name);
            detach_action_from_menu(menu_path, view_results_action_name);
            unregister_action(view_results_action_name);
        }
        if (diff_registered_) {
            detach_action_from_menu(diff_menu_path, diff_action_name);
            detach_action_from_menu(menu_path, diff_action_name);
            unregister_action(diff_action_name);
        }
        if (export_registered_) {
            detach_action_from_menu(export_menu_path, export_action_name);
            detach_action_from_menu(menu_path, export_action_name);
            unregister_action(export_action_name);
        }
        if (graph_import_registered_) {
            unregister_action(graph_import_action_name);
        }
        if (graph_text_registered_) {
            unregister_action(graph_text_action_name);
        }
        if (graph_jump_registered_) {
            unregister_action(graph_jump_action_name);
        }
        if (graph_sync_registered_) {
            unregister_action(graph_sync_action_name);
        }
        if (menu_created_) {
            delete_menu(menu_id);
        }
    }

    bool idaapi run(size_t) override
    {
        static const char prompt[] =
            "Soff has separate workflows.\n"
            "\n"
            "Choose the operation to run.";
        const int choice = ask_buttons(
            "~E~xport current IDB",
            "~D~iff SQLite databases",
            "~L~oad diff results",
            ASKBTN_YES,
            "%s",
            prompt);
        if (choice == ASKBTN_YES) {
            run_export_ui();
        } else if (choice == ASKBTN_NO) {
            run_diff_ui();
        } else if (choice == ASKBTN_CANCEL) {
            const int result_choice = ask_buttons(
                "~L~oad diff results",
                "~I~mport diff results",
                "~L~ocal function diff",
                ASKBTN_YES,
                "Choose the result operation to run.");
            if (result_choice == ASKBTN_YES) {
                run_view_results_ui();
            } else if (result_choice == ASKBTN_NO) {
                run_import_results_ui();
            } else if (result_choice == ASKBTN_CANCEL) {
                run_local_diff_ui();
            }
        }
        return true;
    }

private:
    bool menu_created_ = false;
    bool export_registered_ = false;
    bool diff_registered_ = false;
    bool view_results_registered_ = false;
    bool save_results_registered_ = false;
    bool import_results_registered_ = false;
    bool local_diff_registered_ = false;
    bool graph_sync_registered_ = false;
    bool graph_jump_registered_ = false;
    bool graph_text_registered_ = false;
    bool graph_import_registered_ = false;
};

plugmod_t* idaapi init()
{
    msg("%s %s: plugin loaded\n",
        soff::product_name().data(),
        soff::version().data());
    return new soff_plugin_t;
}

} // namespace

plugin_t PLUGIN =
{
    IDP_INTERFACE_VERSION,
    PLUGIN_MULTI | PLUGIN_HIDE,
    init,
    nullptr,
    nullptr,
    "soff binary diffing rewrite skeleton",
    nullptr,
    "Soff",
    nullptr,
};
