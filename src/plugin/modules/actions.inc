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
        print_perf_report_if_enabled();
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
        print_perf_report_if_enabled();
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
        node != nullptr ? node->secondary : 0,
        &context->primary_snapshot_cache,
        &context->secondary_snapshot_cache);
}

bool import_active_graph_match()
{
    try {
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
            const auto& primary_snapshot = load_snapshot_cached(context->primary_snapshot_cache, selected.main_db);
            const auto& secondary_snapshot = load_snapshot_cached(context->secondary_snapshot_cache, selected.diff_db);
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
    } catch (const std::exception& error) {
        warning("Soff graph import failed:\n%s", error.what());
        return false;
    } catch (...) {
        warning("Soff graph import failed with an unknown error");
        return false;
    }
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

// IDC function: soff_export(output_path) -> JSON string
static error_t idaapi idc_soff_export(idc_value_t* argv, idc_value_t* res)
{
    const std::filesystem::path output_path(argv[0].c_str());
    ExportOptions options;
    options.use_decompiler = true;
    try {
        const auto result = build_ida_snapshot(options, nullptr, &output_path);
        std::ostringstream json;
        json << "{\"exported\":" << result.stats.exported_functions
             << ",\"skipped\":" << result.stats.skipped_functions
             << ",\"total\":" << result.stats.total_functions
             << ",\"output\":\"" << output_path.string() << "\"}";
        res->set_string(json.str().c_str());
    } catch (const std::exception& e) {
        res->set_string((std::string("{\"error\":\"") + e.what() + "\"}").c_str());
    }
    return eOk;
}

static const char idc_soff_export_args[] = { VT_STR, 0 };
static const ext_idcfunc_t idc_soff_export_desc = {
    "soff_export", idc_soff_export, idc_soff_export_args, nullptr, 0, EXTFUN_BASE
};

void run_auto_mode()
{
    msg("Soff: DIAPHORA_AUTO mode activated\n");

    // Belt and braces: even after ui_ready_to_run the analysis queue may
    // still hold work; exporting before it drains would see zero functions.
    auto_wait();

    // Determine export path
    std::filesystem::path export_path;
    if (const char* env_out = env_value("DIAPHORA_EXPORT_FILE")) {
        export_path = env_out;
    } else {
        export_path = default_export_path();
    }
    if (export_path.empty()) {
        msg("Soff: DIAPHORA_AUTO: cannot determine export path\n");
        return;
    }

    // Export
    ExportOptions options;
    options.use_decompiler = true;
    apply_env_overrides(options);
    try {
        const auto result = build_ida_snapshot(options, nullptr, &export_path);
        msg("Soff: DIAPHORA_AUTO: exported %zu/%zu functions to %s\n",
            result.stats.exported_functions,
            result.stats.total_functions,
            export_path.string().c_str());
    } catch (const std::exception& e) {
        msg("Soff: DIAPHORA_AUTO: export failed: %s\n", e.what());
        qexit(1);
        return;
    }

    // Optional auto diff
    if (env_value("DIAPHORA_AUTO_DIFF")) {
        DiffUiOptions diff_options;
        diff_options.main_db = export_path.string();
        if (const char* db1 = env_value("DIAPHORA_DB1")) {
            diff_options.main_db = db1;
        }
        if (const char* db2 = env_value("DIAPHORA_DB2")) {
            diff_options.diff_db = db2;
        } else {
            diff_options.diff_db = export_path.string();
        }
        if (const char* out = env_value("DIAPHORA_DIFF_OUT")) {
            diff_options.result_db = out;
        } else {
            diff_options.result_db = std::filesystem::path(diff_options.main_db).replace_extension(".diaphora").string();
        }
        apply_diff_env_overrides(diff_options);

        try {
            const auto summary = diff_databases(diff_options);
            msg("Soff: DIAPHORA_AUTO_DIFF: best=%zu partial=%zu unreliable=%zu unmatched=%zu/%zu\n",
                summary.results.best, summary.results.partial, summary.results.unreliable,
                summary.results.unmatched_primary, summary.results.unmatched_secondary);
        } catch (const std::exception& e) {
            msg("Soff: DIAPHORA_AUTO_DIFF: diff failed: %s\n", e.what());
            qexit(1);
            return;
        }
    }

    msg("Soff: DIAPHORA_AUTO: done, requesting exit\n");
    qexit(0);
}

// Headless runs are deferred to ui_ready_to_run: the PLUGIN_MULTI constructor
// fires while the IDB is still opening, long before the initial auto-analysis
// has populated the function list.
ssize_t idaapi soff_auto_mode_hook(void* user_data, int notification_code, va_list)
{
    if (notification_code != ui_ready_to_run) {
        return 0;
    }
    unhook_from_notification_point(HT_UI, &soff_auto_mode_hook, user_data);
    run_auto_mode();
    return 0;
}

