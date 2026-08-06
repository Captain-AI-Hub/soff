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
constexpr asize_t function_reserve_min_size = 1;
constexpr asize_t function_reserve_max_size = 1024 * 1024;
constexpr std::size_t instruction_reserve_divisor = 4;
constexpr std::size_t assembly_reserve_multiplier = 32;
constexpr std::size_t stripped_assembly_reserve_multiplier = 24;
constexpr std::size_t mnemonic_reserve_multiplier = 8;

struct ExportOptions
{
    std::string previous_crash_marker;
    bool crash_file_preexisting = false;
    bool resume_existing_database = false;
    bool use_decompiler = true;
    bool use_microcode = false;
    bool exclude_library_thunk = true;
    bool ignore_small_functions = false;
    ea_t min_ea = 0;
    ea_t max_ea = BADADDR;
    soff::ExportHooks* hooks = nullptr;
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

template <typename Value>
struct PathCache
{
    std::filesystem::path path;
    std::shared_ptr<const Value> value;

    void clear()
    {
        path.clear();
        value.reset();
    }
};

const soff::ProgramSnapshot& load_snapshot_cached(
    PathCache<soff::ProgramSnapshot>& cache,
    const std::filesystem::path& path)
{
    if (!cache.value || cache.path != path) {
        soff::perf::ScopedTimer timer("plugin.snapshot.load");
        auto loaded_path = path;
        auto value = std::make_shared<const soff::ProgramSnapshot>(soff::SnapshotRepository{}.load(path));
        cache.path = std::move(loaded_path);
        cache.value = std::move(value);
    }
    return *cache.value;
}

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

void print_perf_report_if_enabled()
{
    if (soff::perf::enabled()) {
        const auto report = soff::perf::render_report();
        msg("%s", report.c_str());
    }
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

