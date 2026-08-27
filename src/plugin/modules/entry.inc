struct soff_plugin_t : public plugmod_t
{
    soff_plugin_t()
    {
        add_idc_func(idc_soff_export_desc);

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

        // DIAPHORA_AUTO: headless export (and optional diff) once the UI loop
        // is ready; running it here in the constructor would race the loader
        // and the initial auto-analysis.
        if (env_value("DIAPHORA_AUTO")) {
            soff_hook_ui_notifications(&soff_auto_mode_hook, this);
        }
    }

    ~soff_plugin_t() override
    {
        del_idc_func("soff_export");

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
    soff::perf::ScopedTimer timer("plugin.init");
    msg("%s %s: plugin loaded\n",
        soff::product_name().data(),
        soff::version().data());
    return new soff_plugin_t;
}

