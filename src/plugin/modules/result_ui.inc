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
    std::size_t global_variables = 0;
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
    bool open_after_save,
    PathCache<soff::ProgramSnapshot>* primary_snapshot_cache = nullptr,
    PathCache<soff::ProgramSnapshot>* secondary_snapshot_cache = nullptr);
bool save_or_open_match_graph_diff(
    const std::filesystem::path& result_path,
    const soff::db::DiffResultSet& results,
    const soff::db::ResultMatch& match,
    PathCache<soff::ProgramSnapshot>* primary_snapshot_cache = nullptr,
    PathCache<soff::ProgramSnapshot>* secondary_snapshot_cache = nullptr);
enum class TextDiffKind
{
    assembly,
    pseudocode,
    microcode,
};
bool show_ida_match_graph_diff(
    const soff::db::DiffResultSet& results,
    const soff::db::ResultMatch& match,
    soff::ui::GraphDiffKind kind,
    PathCache<soff::ProgramSnapshot>* primary_snapshot_cache = nullptr,
    PathCache<soff::ProgramSnapshot>* secondary_snapshot_cache = nullptr);
bool show_linear_asm_diff(
    const soff::db::DiffResultSet& results,
    const soff::db::ResultMatch& match,
    PathCache<soff::ProgramSnapshot>* primary_snapshot_cache = nullptr,
    PathCache<soff::ProgramSnapshot>* secondary_snapshot_cache = nullptr);
bool show_linear_pseudo_diff(
    const soff::db::DiffResultSet& results,
    const soff::db::ResultMatch& match,
    PathCache<soff::ProgramSnapshot>* primary_snapshot_cache = nullptr,
    PathCache<soff::ProgramSnapshot>* secondary_snapshot_cache = nullptr);
bool show_ida_match_text_diff(
    const soff::db::DiffResultSet& results,
    const soff::db::ResultMatch& match,
    TextDiffKind kind,
    soff::Address focus_primary = 0,
    soff::Address focus_secondary = 0,
    PathCache<soff::ProgramSnapshot>* primary_snapshot_cache = nullptr,
    PathCache<soff::ProgramSnapshot>* secondary_snapshot_cache = nullptr);
bool save_or_open_match_call_context(
    const std::filesystem::path& result_path,
    const soff::db::DiffResultSet& results,
    const soff::db::ResultMatch& match,
    PathCache<soff::ProgramSnapshot>* primary_snapshot_cache = nullptr,
    PathCache<soff::ProgramSnapshot>* secondary_snapshot_cache = nullptr);
bool show_match_diff_panel(
    const std::filesystem::path& result_path,
    const soff::db::DiffResultSet& results,
    const soff::db::ResultMatch& match);
SelectedChooserEditResult import_selected_matches_from_chooser(
    const std::filesystem::path& result_path,
    const soff::db::DiffResultSet& results,
    const std::vector<ResultChooserRow>& rows,
    const sizevec_t& selected_rows,
    PathCache<soff::ProgramSnapshot>* primary_snapshot_cache = nullptr,
    PathCache<soff::ProgramSnapshot>* secondary_snapshot_cache = nullptr);

class diff_results_chooser_t : public chooser_multi_t
{
public:
    explicit diff_results_chooser_t(std::filesystem::path result_path, soff::db::DiffResultSet results)
        : chooser_multi_t(CH_CAN_REFRESH | CH_CAN_EDIT | CH_RESTORE, column_count, widths_, header_, "Soff Diff Results"),
          results_(std::move(results))
    {
        CASSERT(column_count == 7);
        set_result_path(std::move(result_path));
        results_loaded_ = true;
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

        ensure_results_loaded();
        const auto edit_result = import_selected_matches_from_chooser(
            result_path_,
            results_,
            rows_,
            *sel,
            &primary_snapshot_cache_,
            &secondary_snapshot_cache_);
        if (edit_result == SelectedChooserEditResult::imported) {
            primary_snapshot_cache_.clear();
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
        ensure_results_loaded();
        rebuild_rows();
        return ALL_CHANGED;
    }

private:
    void set_result_path(std::filesystem::path result_path)
    {
        if (result_path_ == result_path) {
            return;
        }
        result_path_ = std::move(result_path);
        results_loaded_ = false;
        primary_snapshot_cache_.clear();
        secondary_snapshot_cache_.clear();
    }

    void ensure_results_loaded()
    {
        if (results_loaded_) {
            return;
        }
        soff::perf::ScopedTimer timer("plugin.results.load");
        results_ = soff::db::ResultRepository{}.load(result_path_);
        results_loaded_ = true;
    }

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
    bool results_loaded_ = false;
    PathCache<soff::ProgramSnapshot> primary_snapshot_cache_;
    PathCache<soff::ProgramSnapshot> secondary_snapshot_cache_;
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
            show_linear_asm_diff(results_, match_, &primary_snapshot_cache_, &secondary_snapshot_cache_);
            break;
        case MatchDiffPanelAction::assembly_blocks:
            show_ida_match_graph_diff(results_, match_, soff::ui::GraphDiffKind::native, &primary_snapshot_cache_, &secondary_snapshot_cache_);
            break;
        case MatchDiffPanelAction::pseudocode_flat:
            show_linear_pseudo_diff(results_, match_, &primary_snapshot_cache_, &secondary_snapshot_cache_);
            break;
        case MatchDiffPanelAction::microcode_flat:
            show_ida_match_text_diff(results_, match_, TextDiffKind::microcode, 0, 0, &primary_snapshot_cache_, &secondary_snapshot_cache_);
            break;
        case MatchDiffPanelAction::microcode_blocks:
            show_ida_match_graph_diff(results_, match_, soff::ui::GraphDiffKind::microcode, &primary_snapshot_cache_, &secondary_snapshot_cache_);
            break;
        case MatchDiffPanelAction::call_context:
            save_or_open_match_call_context(result_path_, results_, match_, &primary_snapshot_cache_, &secondary_snapshot_cache_);
            break;
        case MatchDiffPanelAction::html_open:
            save_or_open_match_html_diff(result_path_, results_, match_, true, &primary_snapshot_cache_, &secondary_snapshot_cache_);
            break;
        case MatchDiffPanelAction::html_save:
            save_or_open_match_html_diff(result_path_, results_, match_, false, &primary_snapshot_cache_, &secondary_snapshot_cache_);
            break;
        case MatchDiffPanelAction::html_graph_preview:
            save_or_open_match_graph_diff(result_path_, results_, match_, &primary_snapshot_cache_, &secondary_snapshot_cache_);
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
    PathCache<soff::ProgramSnapshot> primary_snapshot_cache_;
    PathCache<soff::ProgramSnapshot> secondary_snapshot_cache_;
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

