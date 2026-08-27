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
            // Only import user-meaningful bits; overwriting the whole flags
            // word would clobber analysis-computed state (frame, SP, far...).
            function->flags = static_cast<decltype(function->flags)>(
                (function->flags & ~FUNC_NORET)
                | (static_cast<decltype(function->flags)>(item.numeric_value) & FUNC_NORET));
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
    const sizevec_t& selected_rows,
    PathCache<soff::ProgramSnapshot>* primary_snapshot_cache,
    PathCache<soff::ProgramSnapshot>* secondary_snapshot_cache)
{
    try {
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
            PathCache<soff::ProgramSnapshot> primary_uncached;
            PathCache<soff::ProgramSnapshot> secondary_uncached;
            const auto& primary_snapshot = load_snapshot_cached(
                primary_snapshot_cache != nullptr ? *primary_snapshot_cache : primary_uncached,
                selected.main_db);
            const auto& secondary_snapshot = load_snapshot_cached(
                secondary_snapshot_cache != nullptr ? *secondary_snapshot_cache : secondary_uncached,
                selected.diff_db);
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
    } catch (const std::exception& error) {
        warning("Soff selected import failed:\n%s", error.what());
        return SelectedChooserEditResult::unchanged;
    } catch (...) {
        warning("Soff selected import failed with an unknown error");
        return SelectedChooserEditResult::unchanged;
    }
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
        soff::db::DiffResultSet results;
        {
            soff::perf::ScopedTimer timer("plugin.results.load");
            results = soff::db::ResultRepository{}.load(result_path);
        }
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
            PathCache<soff::ProgramSnapshot> primary_snapshot_cache;
            PathCache<soff::ProgramSnapshot> secondary_snapshot_cache;
            const auto& primary_snapshot = load_snapshot_cached(primary_snapshot_cache, results.main_db);
            const auto& secondary_snapshot = load_snapshot_cached(secondary_snapshot_cache, results.diff_db);
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

