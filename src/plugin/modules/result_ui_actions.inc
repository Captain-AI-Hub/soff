void show_result_chooser(const std::filesystem::path& result_path)
{
    if (const auto error = validate_result_database(result_path); !error.empty()) {
        throw soff::Error(soff::ErrorCode::diff_failed, error);
    }
    soff::db::DiffResultSet results;
    {
        soff::perf::ScopedTimer timer("plugin.results.load");
        results = soff::db::ResultRepository{}.load(result_path);
    }
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

