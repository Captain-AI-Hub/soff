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
    // Store bool options as value+1 so 0 means "never saved" (sentinel)
    node.altset(5, options.exclude_library_thunk ? 2 : 1);
    node.altset(6, options.use_microcode ? 2 : 1);
    node.altset(7, options.ignore_small_functions ? 2 : 1);
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
    // Sentinel encoding: 0=never saved (keep default), 1=false, 2=true
    if (const auto v = node.altval(5); v != 0) {
        options.exclude_library_thunk = (v == 2);
    }
    if (const auto v = node.altval(6); v != 0) {
        options.use_microcode = (v == 2);
    }
    if (const auto v = node.altval(7); v != 0) {
        options.ignore_small_functions = (v == 2);
    }

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
    read_bool_env("DIAPHORA_USE_MICROCODE", options.use_microcode);
    read_bool_env("SOFF_USE_MICROCODE", options.use_microcode);
    read_bool_env("DIAPHORA_EXCLUDE_LIBRARY_THUNK", options.exclude_library_thunk);
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
    constexpr ushort check_use_microcode = 1 << 1;
    constexpr ushort check_exclude_library_thunk = 1 << 4;
    constexpr ushort check_ignore_small = 1 << 6;

    if (options.use_decompiler) {
        checks |= check_use_decompiler;
    }
    if (options.use_microcode) {
        checks |= check_use_microcode;
    }
    if (options.exclude_library_thunk) {
        checks |= check_exclude_library_thunk;
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
        "<Export microcode (slow):C>\n"
        "<Exclude library/thunk/nullsub:C>\n"
        "<Ignore very small functions:C>>\n";

    if (ask_form(form, output_buffer, &min_ea, &max_ea, &checks) <= 0) {
        return false;
    }

    output_path = output_buffer;
    options.min_ea = min_ea;
    options.max_ea = max_ea;
    options.use_decompiler = (checks & check_use_decompiler) != 0;
    options.use_microcode = (checks & check_use_microcode) != 0;
    options.exclude_library_thunk = (checks & check_exclude_library_thunk) != 0;
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

