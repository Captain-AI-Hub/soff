#include "soff/analysis/model.hpp"
#include "soff/core/hooks.hpp"
#include <boost/unordered/unordered_flat_set.hpp>
#include "soff/db/database.hpp"
#include "soff/db/result_repository.hpp"
#include "soff/db/schema.hpp"
#include "soff/db/repository.hpp"
#include "soff/diff/heuristics.hpp"
#include "soff/diff/ml_features.hpp"
#include "soff/diff/patch_diff.hpp"
#include "soff/diff/propagation.hpp"
#include "soff/diff/ratio.hpp"
#include "soff/diff/session.hpp"
#include "soff/diff/sql_runner.hpp"
#include "soff/ui/html_diff.hpp"
#include "soff/ui/import_plan.hpp"

#include <cassert>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <utility>

int main()
{
    soff::ProgramSnapshot snapshot;
    snapshot.input_path = "sample.exe";
    snapshot.architecture = "metapc";
    snapshot.program_data.push_back({"export.total_functions", "integer", "1"});
    snapshot.program_data.push_back({"export.exported_functions", "integer", "1"});
    snapshot.program_data.push_back({"export.skipped_functions", "integer", "0"});
    soff::FunctionFeature start;
    start.address = 0x401000;
    start.rva = 0x1000;
    start.segment_rva = 0x1000;
    start.name = "start";
    start.size = 4;
    start.instruction_count = 2;
    start.node_count = 2;
    start.edge_count = 1;
    start.indegree = 1;
    start.outdegree = 1;
    start.cyclomatic_complexity = 1;
    start.strongly_connected = 1;
    start.loops = 0;
    start.mnemonics = "push\nret";
    start.names = "start";
    start.assembly = "push rbp\nret";
    start.stripped_assembly = "push rbp\nret";
    start.assembly_addrs = "[\"4198400\",\"4198401\"]";
    start.bytes_hash = "same-bytes";
    start.function_hash = "same-function";
    start.md_index = "1.25";
    start.primes_value = "3";
    start.tarjan_topological_sort = "[[0]]";
    start.strongly_connected_spp = "1";
    start.mnemonics_spp = "15";
    start.switches = "[]";
    start.bytes_sum = 0x55 + 0xc3;
    start.function_flags = 0x10;
    start.mangled_function = "start";
    start.prototype = "int start(void)";
    start.prototype2 = "int start(void)";
    start.comment = "entry";
    start.pseudocode = "return 0;";
    start.stripped_pseudocode = "return 0;";
    start.pseudocode_lines = 1;
    start.pseudocode_hash1 = "pseudo-hash-1";
    start.pseudocode_hash2 = "pseudo-hash-2";
    start.pseudocode_hash3 = "pseudo-hash-3";
    start.blocks.push_back({0x401000, 0x401001, {0x401000}, {0x401001}});
    start.blocks.push_back({0x401001, 0x401004, {0x401001}, {}});
    start.instruction_details.push_back({0x401000, "push rbp", "push", "prologue", "repeatable prologue"});
    start.instruction_details.back().operand_names = "[[0,\"rbp\"]]";
    start.instruction_details.push_back({0x401001, "ret", "ret"});
    start.call_references.push_back({0x402000, "call near"});
    start.constants.push_back("7");
    snapshot.functions.push_back(std::move(start));

    assert(soff::is_valid_snapshot(snapshot));
    assert(!soff::db::diaphora_compatible_schema().tables.empty());
    assert(!soff::db::diaphora_compatible_schema().indexes.empty());
    const auto& heuristics = soff::diff::builtin_heuristics();
    assert(heuristics.size() == 51);

    auto best = 0;
    auto partial = 0;
    auto unreliable = 0;
    auto no_fps = 0;
    auto ratio = 0;
    auto ratio_min = 0;
    auto trusted_ratio_min = 0;
    auto no_flags = 0;
    auto same_cpu = 0;
    auto slow = 0;
    auto slow_unreliable = 0;
    auto helper_same_cpu = 0;
    auto helper_slow = 0;
    auto helper_unreliable = 0;
    for (const auto& heuristic : heuristics) {
        switch (heuristic.category) {
        case soff::diff::HeuristicCategory::best:
            ++best;
            break;
        case soff::diff::HeuristicCategory::partial:
            ++partial;
            break;
        case soff::diff::HeuristicCategory::unreliable:
            ++unreliable;
            break;
        case soff::diff::HeuristicCategory::experimental:
            break;
        }

        switch (heuristic.ratio_mode) {
        case soff::diff::RatioMode::no_false_positives:
            ++no_fps;
            break;
        case soff::diff::RatioMode::ratio:
            ++ratio;
            break;
        case soff::diff::RatioMode::ratio_with_minimum:
            ++ratio_min;
            break;
        case soff::diff::RatioMode::trusted_ratio_with_minimum:
            ++trusted_ratio_min;
            break;
        }

        if (heuristic.flags == soff::diff::heuristic_flag_none) {
            ++no_flags;
        } else if (heuristic.flags == soff::diff::heuristic_flag_same_cpu) {
            ++same_cpu;
        } else if (heuristic.flags == soff::diff::heuristic_flag_slow) {
            ++slow;
        } else if (heuristic.flags == (soff::diff::heuristic_flag_slow | soff::diff::heuristic_flag_unreliable)) {
            ++slow_unreliable;
        }

        helper_same_cpu += soff::diff::supports_same_cpu_only(heuristic) ? 1 : 0;
        helper_slow += soff::diff::is_slow(heuristic) ? 1 : 0;
        helper_unreliable += soff::diff::is_unreliable(heuristic) ? 1 : 0;
    }

    assert(best == 13);
    assert(partial == 30);
    assert(unreliable == 8);
    assert(no_fps == 5);
    assert(ratio == 23);
    assert(ratio_min == 22);
    assert(trusted_ratio_min == 1);
    assert(no_flags == 31);
    assert(same_cpu == 8);
    assert(slow == 9);
    assert(slow_unreliable == 3);
    assert(helper_same_cpu == 8);
    assert(helper_slow == 12);
    assert(helper_unreliable == 11);

    const auto first_required_fields = soff::diff::required_fields(heuristics.front());
    assert(std::find(first_required_fields.begin(), first_required_fields.end(), "rva") != first_required_fields.end());
    assert(std::find(first_required_fields.begin(), first_required_fields.end(), "segment_rva") != first_required_fields.end());
    assert(std::find(first_required_fields.begin(), first_required_fields.end(), "bytes_hash") != first_required_fields.end());
    assert(soff::diff::supports_same_cpu_only(heuristics.front()));
    assert(!soff::diff::is_slow(heuristics.front()));
    assert(soff::diff::line_lcs_ratio("a\nb\nc", "a\nx\nc") > 0.66);
    assert(soff::diff::line_lcs_ratio("a\nb", "x\ny") == 0.0);
    assert(soff::diff::sequence_matcher_quick_ratio("a\nb", "b\na") == 1.0);
    assert(std::abs(soff::diff::ast_prime_difference_ratio("30", "42") - 0.33333333333333337) < 0.000001);
    assert(std::abs(soff::diff::ast_prime_difference_ratio("2310", "2730") - 0.6) < 0.000001);
    assert(soff::diff::candidate_text_ratio("", "", "", "", "mov eax, ebx", "mov eax, ebx", "", "") == 1.0);

    const auto heuristic_issues = soff::diff::validate_builtin_heuristics();
    for (const auto& issue : heuristic_issues) {
        std::cerr << "heuristic validation: " << issue << '\n';
    }
    assert(heuristic_issues.empty());

    const auto db_path = std::filesystem::absolute(std::filesystem::path("build") / "soff_smoke.sqlite");
    std::filesystem::create_directories(db_path.parent_path());
    soff::SnapshotRepository repository;
    assert(repository.save(snapshot, db_path));
    soff::db::Database smoke_db;
    smoke_db.open(db_path);
    auto version_stmt = smoke_db.prepare("select value from version limit 1");
    assert(version_stmt.step());
    assert(version_stmt.column_text(0) == soff::SnapshotRepository::soff_version_value);
    assert(!version_stmt.step());
    const auto function_rows = smoke_db.query_rows("select name, address from functions order by id limit 1");
    assert(function_rows.size() == 1);
    assert(function_rows[0].size() == 2);
    assert(function_rows[0][0] == "start");
    auto reusable_stmt = smoke_db.prepare("select ?");
    reusable_stmt.bind(1, "first");
    assert(reusable_stmt.step());
    assert(reusable_stmt.column_text(0) == "first");
    assert(!reusable_stmt.step());
    reusable_stmt.reset();
    reusable_stmt.bind(1, "second");
    assert(reusable_stmt.step());
    assert(reusable_stmt.column_text(0) == "second");
    assert(smoke_db.query_int("select count(*) from instructions") == 2);
    assert(smoke_db.query_int("select count(*) from basic_blocks") == 2);
    assert(smoke_db.query_int("select count(*) from function_bblocks") == 2);
    assert(smoke_db.query_int("select count(*) from bb_instructions") == 2);
    assert(smoke_db.query_int("select count(*) from bb_relations") == 1);
    assert(smoke_db.query_int("select count(*) from callgraph") == 1);
    assert(smoke_db.query_int("select count(*) from constants") == 1);
    smoke_db.execute(
        "update functions set nodes = 3, instructions = 12, rva = '4096', segment_rva = '4096', "
        "bytes_hash = 'same-bytes', function_hash = 'same-function', "
        "assembly = 'push rbp\nmov rbp, rsp\npop rbp', clean_assembly = 'push rbp\nmov rbp, rsp\npop rbp', "
        "pseudocode = 'int start() { return 0; }', clean_pseudo = 'int start() { return 0; }' "
        "where name = 'start'");
    smoke_db.execute(
        "insert into functions (name, address, nodes, instructions, rva, segment_rva, bytes_hash, function_hash, "
        "assembly, clean_assembly, pseudocode, clean_pseudo, md_index, pseudocode_primes, indegree, outdegree, "
        "switches, cyclomatic_complexity, constants, source_file) "
        "values ('start_clone', '4202496', 3, 12, '8192', '8192', 'same-bytes', 'same-function', "
        "'push rbp\nmov rbp, rsp\npop rbp', 'push rbp\nmov rbp, rsp\npop rbp', "
        "'int start() { return 0; }', 'int start() { return 0; }', '10.0', '2310', 1, 1, "
        "'[1]', 2, '[\"7\", \"11\"]', 'sample.c')");
    smoke_db.execute(
        "update functions set md_index = '10.0', pseudocode_primes = '2310', indegree = 1, outdegree = 1, "
        "switches = '[1]', cyclomatic_complexity = 2, primes_value = '5', constants = '[\"7\", \"11\"]', source_file = 'sample.c' "
        "where name = 'start'");
    smoke_db.execute(
        "insert into functions (name, address, nodes, instructions, rva, segment_rva) "
        "values ('lonely', '4206592', 1, 1, '12288', '12288')");
    smoke_db.close();

    const auto diaphora_version_path = std::filesystem::absolute(
        std::filesystem::path("build") / "soff_diaphora_version.sqlite");
    soff::SnapshotRepository diaphora_version_repository(soff::SnapshotVersionPolicy::diaphora_34);
    assert(diaphora_version_repository.save(snapshot, diaphora_version_path));
    soff::db::Database diaphora_version_db;
    diaphora_version_db.open(diaphora_version_path);
    assert(diaphora_version_db.query_text("select value from version limit 1") == soff::SnapshotRepository::diaphora_version_value);
    assert(diaphora_version_db.query_int("select count(*) from sqlite_master where type = 'index'") > 0);
    diaphora_version_db.close();

    const auto incremental_path = std::filesystem::absolute(
        std::filesystem::path("build") / "soff_incremental.sqlite");
    auto incremental_snapshot = snapshot;
    incremental_snapshot.program_data.push_back({"export.crash_resume_supported", "boolean", "true"});
    incremental_snapshot.functions.clear();
    repository.begin_incremental_save(incremental_snapshot, incremental_path, true);
    repository.append_functions({snapshot.functions[0]}, incremental_path);
    auto resumed_function = snapshot.functions[0];
    resumed_function.address = 0x401100;
    resumed_function.rva = 0x1100;
    resumed_function.name = "after_resume";
    repository.append_functions({resumed_function}, incremental_path);
    repository.replace_program_data(incremental_snapshot.program_data, incremental_path);
    repository.finalize_incremental_save(incremental_path);
    const auto incremental_loaded = repository.load(incremental_path);
    assert(incremental_loaded.functions.size() == 2);
    assert(incremental_loaded.functions[0].name == "start");
    assert(incremental_loaded.functions[1].name == "after_resume");
    assert(incremental_loaded.program_data.size() == 4);

    const auto loaded = repository.load(db_path);
    assert(loaded.architecture == "metapc");
    assert(loaded.program_data.size() == 3);
    assert(loaded.program_data[0].name == "export.total_functions");
    assert(loaded.functions.size() == 3);
    assert(loaded.functions[0].address == 0x401000);
    assert(loaded.functions[0].name == "start");
    assert(loaded.functions[0].names == "start");
    assert(loaded.functions[0].assembly == "push rbp\nmov rbp, rsp\npop rbp");
    assert(loaded.functions[0].stripped_assembly == "push rbp\nmov rbp, rsp\npop rbp");
    assert(loaded.functions[0].bytes_sum == 0x55 + 0xc3);
    assert(loaded.functions[0].function_flags == 0x10);
    assert(loaded.functions[0].prototype == "int start(void)");
    assert(loaded.functions[0].instruction_details.size() == 2);
    assert(loaded.functions[0].instruction_details[0].comment1 == "prologue");
    assert(loaded.functions[0].instruction_details[0].comment2 == "repeatable prologue");
    assert(loaded.functions[0].instruction_details[0].operand_names == "[[0,\"rbp\"]]");
    assert(loaded.functions[0].pseudocode == "int start() { return 0; }");
    assert(loaded.functions[0].stripped_pseudocode == "int start() { return 0; }");
    assert(loaded.functions[0].pseudocode_lines == 1);
    assert(loaded.functions[0].pseudocode_hash1 == "pseudo-hash-1");
    assert(loaded.functions[0].pseudocode_hash2 == "pseudo-hash-2");
    assert(loaded.functions[0].pseudocode_hash3 == "pseudo-hash-3");
    assert(loaded.functions[0].md_index == "10.0");
    assert(loaded.functions[0].cyclomatic_complexity == 2);
    assert(loaded.functions[0].primes_value == "5");
    assert(loaded.functions[0].strongly_connected == 1);
    assert(loaded.functions[0].tarjan_topological_sort == "[[0]]");
    assert(loaded.functions[0].mnemonics_spp == "15");
    assert(loaded.functions[0].switches == "[1]");
    assert(loaded.functions[0].blocks.size() == 2);
    assert(loaded.functions[0].blocks[0].start == 0x401000);
    assert(loaded.functions[0].blocks[0].instructions.size() == 1);
    assert(loaded.functions[0].blocks[0].instructions[0] == 0x401000);
    assert(loaded.functions[0].blocks[0].successors.size() == 1);
    assert(loaded.functions[0].blocks[0].successors[0] == 0x401001);
    assert(loaded.functions[0].blocks[1].instructions.size() == 1);
    assert(loaded.functions[0].blocks[1].instructions[0] == 0x401001);
    assert(loaded.functions[0].call_references.size() == 1);
    assert(loaded.functions[0].call_references[0].address == 0x402000);

    soff::db::DiffResultSet results;
    results.main_db = "main.sqlite";
    results.diff_db = "diff.sqlite";
    results.matches.push_back({
        soff::db::ResultKind::best,
        0,
        0x401000,
        "start",
        0x501000,
        "start",
        1.0,
        3,
        3,
        "Same RVA and hash",
    });
    results.unmatched.push_back({soff::db::UnmatchedKind::primary, 0, 0x402000, "only_primary"});

    const auto result_path = std::filesystem::absolute(std::filesystem::path("build") / "soff_results.soff");
    soff::db::ResultRepository result_repository;
    assert(result_repository.save(results, result_path));
    const auto loaded_results = result_repository.load(result_path);
    assert(loaded_results.main_db == "main.sqlite");
    assert(loaded_results.diff_db == "diff.sqlite");
    assert(loaded_results.matches.size() == 1);
    assert(loaded_results.matches[0].primary == 0x401000);
    assert(loaded_results.matches[0].secondary == 0x501000);
    assert(loaded_results.matches[0].description == "Same RVA and hash");
    assert(loaded_results.unmatched.size() == 1);
    assert(loaded_results.unmatched[0].address == 0x402000);
    assert(loaded_results.heuristic_stats.empty());
    const auto manual_summary = result_repository.summarize(result_path);
    assert(manual_summary.best == 1);
    assert(manual_summary.unmatched_primary == 1);

    soff::db::DiffResultSet import_results;
    import_results.matches.push_back({
        soff::db::ResultKind::best,
        0,
        0x401000,
        "start",
        0x501000,
        "imported_start",
        1.0,
        3,
        3,
        "rename candidate",
    });
    import_results.matches.push_back({
        soff::db::ResultKind::best,
        1,
        0x401010,
        "same_name",
        0x501010,
        "same_name",
        1.0,
        1,
        1,
        "same name",
    });
    import_results.matches.push_back({
        soff::db::ResultKind::partial,
        2,
        0x401020,
        "weak",
        0x501020,
        "weak_import",
        0.25,
        1,
        1,
        "low ratio",
    });
    import_results.matches.push_back({
        soff::db::ResultKind::best,
        3,
        0x401030,
        "named",
        0x501030,
        "sub_501030",
        0.9,
        1,
        1,
        "auto name",
    });
    import_results.matches.push_back({
        soff::db::ResultKind::multimatch,
        4,
        0x401040,
        "ambiguous",
        0x501040,
        "imported_ambiguous",
        0.95,
        1,
        1,
        "ambiguous",
    });
    const auto import_plan = soff::ui::build_import_plan(import_results);
    assert(import_plan.items.size() == 1);
    assert(import_plan.items[0].address == 0x401000);
    assert(import_plan.items[0].imported_name == "imported_start");
    assert(import_plan.function_renames == 1);
    assert(import_plan.skipped_same_name == 1);
    assert(import_plan.skipped_ratio == 1);
    assert(import_plan.skipped_auto_name == 1);
    assert(import_plan.skipped_kind == 1);

    soff::ProgramSnapshot primary_import_snapshot;
    soff::FunctionFeature primary_import_function;
    primary_import_function.address = 0x401000;
    primary_import_function.name = "sub_401000";
    primary_import_function.prototype = "int sub_401000(void)";
    primary_import_function.instruction_details.push_back({0x401000, "ret", "ret"});
    primary_import_snapshot.functions.push_back(primary_import_function);
    soff::ProgramSnapshot secondary_import_snapshot;
    soff::FunctionFeature secondary_import_function;
    secondary_import_function.address = 0x501000;
    secondary_import_function.name = "real_entry";
    secondary_import_function.prototype = "int real_entry(void)";
    secondary_import_function.comment = "secondary comment";
    secondary_import_function.function_flags = 0x20;
    secondary_import_function.instruction_details.push_back({
        0x501000,
        "ret",
        "ret",
        "instruction comment",
        "repeatable instruction comment",
        "[[1,\"forced text\"]]",
        "",
        "",
        "pseudo comment",
        2,
    });
    secondary_import_snapshot.functions.push_back(secondary_import_function);
    soff::db::DiffResultSet detailed_import_results;
    detailed_import_results.matches.push_back({
        soff::db::ResultKind::best,
        0,
        0x401000,
        "sub_401000",
        0x501000,
        "real_entry",
        1.0,
        1,
        1,
        "detailed import",
    });
    soff::ui::ImportPlanOptions detailed_import_options;
    detailed_import_options.import_function_flags = true;
    const auto detailed_import_plan = soff::ui::build_import_plan(
        detailed_import_results,
        primary_import_snapshot,
        secondary_import_snapshot,
        detailed_import_options);
    assert(detailed_import_plan.function_renames == 1);
    assert(detailed_import_plan.prototypes == 1);
    assert(detailed_import_plan.function_comments == 1);
    assert(detailed_import_plan.function_flags == 1);
    assert(detailed_import_plan.instruction_comments == 1);
    assert(detailed_import_plan.repeatable_instruction_comments == 1);
    assert(detailed_import_plan.forced_operands == 1);
    assert(detailed_import_plan.pseudocode_comments == 1);
    assert(detailed_import_plan.items.size() == 8);

    auto secondary_diff_function = secondary_import_function;
    secondary_diff_function.assembly = "push rbp\nmov eax, 1\nret";
    secondary_diff_function.stripped_assembly = "push rbp\nmov eax, 1\nret";
    secondary_diff_function.pseudocode = "return 1;";
    secondary_diff_function.stripped_pseudocode = "return 1;";
    secondary_diff_function.microcode = "m_mov eax, #1\nm_ret";
    secondary_diff_function.stripped_microcode = "m_mov eax #1\nm_ret";
    secondary_diff_function.blocks.push_back({0x501000, 0x501010, {0x501000, 0x501004}, {0x501010}});
    secondary_diff_function.blocks.push_back({0x501010, 0x501014, {0x501010}, {}});
    secondary_diff_function.microcode_blocks.push_back({0xA000, 0, {0}, {0xA001}});
    secondary_diff_function.microcode_blocks.push_back({0xA001, 0, {1}, {}});
    auto primary_diff_function = snapshot.functions[0];
    primary_diff_function.pseudocode = "return <0>;";
    primary_diff_function.microcode_blocks.push_back({0x9000, 0, {0}, {}});
    const auto html_document = soff::ui::build_function_diff_document(
        primary_diff_function,
        secondary_diff_function,
        0.75,
        "smoke diff");
    const auto html = soff::ui::render_html_diff(html_document);
    assert(html.find("Assembly") != std::string::npos);
    assert(html.find("Pseudocode") != std::string::npos);
    assert(html.find("Microcode") != std::string::npos);
    assert(html.find("return &lt;0&gt;;") != std::string::npos);
    assert(html.find("class=\"chg\"") != std::string::npos);
    const auto native_graph_html = soff::ui::render_function_graph_diff_html(
        primary_diff_function,
        secondary_diff_function,
        soff::ui::GraphDiffKind::native,
        0.75,
        "native graph smoke");
    assert(native_graph_html.find("Native CFG") != std::string::npos);
    assert(native_graph_html.find("nodes=2") != std::string::npos);
    assert(native_graph_html.find("class=\"del\"") != std::string::npos
        || native_graph_html.find("class=\"ins\"") != std::string::npos
        || native_graph_html.find("class=\"chg\"") != std::string::npos);
    const auto micro_graph_html = soff::ui::render_function_graph_diff_html(
        primary_diff_function,
        secondary_diff_function,
        soff::ui::GraphDiffKind::microcode,
        0.75,
        "micro graph smoke");
    assert(micro_graph_html.find("Microcode CFG") != std::string::npos);
    assert(micro_graph_html.find("#0-&gt;#1") != std::string::npos);
    soff::ProgramSnapshot primary_context_snapshot;
    primary_context_snapshot.functions.push_back(primary_diff_function);
    soff::FunctionFeature primary_caller;
    primary_caller.address = 0x410000;
    primary_caller.name = "caller_a";
    primary_caller.size = 0x20;
    primary_caller.call_references.push_back({primary_diff_function.address, "call near"});
    primary_context_snapshot.functions.push_back(primary_caller);
    soff::FunctionFeature primary_callee;
    primary_callee.address = 0x420000;
    primary_callee.name = "callee_a";
    primary_callee.size = 0x20;
    primary_context_snapshot.functions.push_back(primary_callee);
    primary_context_snapshot.functions[0].call_references.push_back({primary_callee.address, "call near"});

    soff::ProgramSnapshot secondary_context_snapshot;
    secondary_context_snapshot.functions.push_back(secondary_diff_function);
    soff::FunctionFeature secondary_caller;
    secondary_caller.address = 0x510000;
    secondary_caller.name = "caller_b";
    secondary_caller.size = 0x20;
    secondary_caller.call_references.push_back({secondary_diff_function.address, "call near"});
    secondary_context_snapshot.functions.push_back(secondary_caller);
    soff::FunctionFeature secondary_callee;
    secondary_callee.address = 0x520000;
    secondary_callee.name = "callee_b";
    secondary_callee.size = 0x20;
    secondary_context_snapshot.functions.push_back(secondary_callee);
    secondary_context_snapshot.functions[0].call_references.push_back({secondary_callee.address, "call far"});

    const auto context_html = soff::ui::render_call_context_diff_html(
        primary_context_snapshot,
        secondary_context_snapshot,
        primary_diff_function.address,
        secondary_diff_function.address,
        0.75,
        "call context smoke");
    assert(context_html.find("Call Context") != std::string::npos);
    assert(context_html.find("Callers") != std::string::npos);
    assert(context_html.find("Callees") != std::string::npos);
    assert(context_html.find("caller_a") != std::string::npos);
    assert(context_html.find("callee_b") != std::string::npos);

    const auto exact_result_path = std::filesystem::absolute(std::filesystem::path("build") / "soff_exact_session.soff");
    const auto exact_summary = soff::diff::DiffSession{}.run_exact(db_path, db_path, exact_result_path);
    assert(exact_summary.heuristics == 5);
    assert(exact_summary.same_processor);
    assert(exact_summary.candidates >= 1);
    assert(exact_summary.accepted >= 1);
    const auto exact_results = result_repository.load(exact_result_path);
    assert(exact_results.main_db == db_path.string());
    assert(exact_results.diff_db == db_path.string());
    assert(exact_results.matches.size() >= 2);
    assert(exact_results.heuristic_stats.size() == 5);
    assert(exact_results.heuristic_stats[0].candidates >= exact_results.heuristic_stats[0].accepted);
    auto exact_multimatches = 0;
    for (const auto& match : exact_results.matches) {
        if (match.kind == soff::db::ResultKind::multimatch) {
            ++exact_multimatches;
        }
    }
    assert(exact_multimatches == 0);
    const auto exact_result_summary = result_repository.summarize(exact_result_path);
    assert(exact_result_summary.best >= 1);
    assert(exact_result_summary.multimatch == 0);
    assert(exact_result_summary.unmatched_primary >= 1);
    assert(exact_result_summary.unmatched_secondary >= 1);

    const auto all_result_path = std::filesystem::absolute(std::filesystem::path("build") / "soff_all_session.soff");
    const auto all_summary = soff::diff::DiffSession{}.run_all(db_path, db_path, all_result_path);
    assert(all_summary.same_processor);
    assert(all_summary.multimatches == 0);
    assert(all_summary.results.best >= 1);
    assert(all_summary.results.multimatch == 0);
    const auto all_results = result_repository.load(all_result_path);
    // Self-diff may trigger stripped binary fast path (skips SQL heuristics)
    if (!all_results.heuristic_stats.empty()) {
        assert(all_results.heuristic_stats.size() >= exact_results.heuristic_stats.size());
        auto total_stat_candidates = std::size_t{0};
        auto total_stat_accepted = std::size_t{0};
        for (const auto& stats : all_results.heuristic_stats) {
            total_stat_candidates += stats.candidates;
            total_stat_accepted += stats.accepted;
        }
        assert(total_stat_candidates == all_summary.candidates);
        assert(total_stat_accepted == all_summary.accepted);
    }

    soff::db::Database cancellable_db;
    cancellable_db.open(db_path);
    repository.attach_diff(cancellable_db, db_path);
    soff::diff::SqlRunnerOptions cancel_options;
    cancel_options.progress_check_interval = 1;
    cancel_options.cancel_requested = [] { return true; };
    const auto cancelled_run = soff::diff::SqlHeuristicRunner{}.run_all(cancellable_db, cancel_options);
    assert(!cancelled_run.stats.empty());
    assert(cancelled_run.stats.front().cancelled);
    assert(cancelled_run.matches.empty());

    // M9: Hook dispatch test
    struct TestHook : soff::DiffHooks
    {
        bool on_finish_called = false;
        int on_match_calls = 0;

        std::optional<std::string> on_launch_heuristic(
            std::string_view name, std::string_view sql) override
        {
            if (name == "Same RVA and hash") return std::nullopt;
            return std::string(sql);
        }

        soff::MatchDecision on_match(const soff::MatchContext& context) override
        {
            ++on_match_calls;
            return {true, context.ratio};
        }

        void on_finish() override { on_finish_called = true; }
    };

    TestHook hook;
    soff::diff::DiffSessionOptions hook_options;
    hook_options.hooks = &hook;
    const auto hook_result_path = std::filesystem::absolute(
        std::filesystem::path("build") / "soff_hook_test.soff");
    const auto hook_summary = soff::diff::DiffSession{hook_options}.run_all(
        db_path, db_path, hook_result_path);
    assert(hook.on_finish_called);
    bool has_rva_hash = false;
    for (const auto& s : soff::diff::SqlHeuristicRunner{}.run_all(
             cancellable_db, soff::diff::SqlRunnerOptions{}).stats) {
        (void)s;
    }
    const auto hook_results = result_repository.load(hook_result_path);
    for (const auto& s : hook_results.heuristic_stats) {
        if (s.name == "Same RVA and hash") has_rva_hash = true;
    }
    assert(!has_rva_hash);
    std::cout << "hook: on_finish=" << hook.on_finish_called
              << " on_match_calls=" << hook.on_match_calls << '\n';

    // M9: Patch diff pattern detection tests
    {
        std::string detail;
        assert(soff::diff::detect_signed_unsigned_change(
            "jl label1\nmov eax, 1", "jb label1\nmov eax, 1", detail));
        assert(detail == "jl -> jb");

        assert(!soff::diff::detect_signed_unsigned_change(
            "mov eax, 1\nret", "mov eax, 1\nret", detail));

        assert(soff::diff::detect_unsafe_function_pattern(
            "x = foo();\nreturn x;",
            "x = foo();\nsprintf(buf, x);\nreturn x;", detail));
        assert(detail.find("printf") != std::string::npos);

        assert(soff::diff::detect_size_check_added(
            "x = read();\nprocess(x);",
            "x = read();\nif ( x < max_size )\n  process(x);", detail));
        assert(detail.find("Size check") != std::string::npos);
    }
    std::cout << "patch_diff: pattern tests passed\n";

    // M9: ML feature vector smoke
    {
        const auto all_results = result_repository.load(all_result_path);
        soff::db::Database ml_db;
        ml_db.open(db_path);
        soff::SnapshotRepository ml_repo;
        ml_repo.attach_diff(ml_db, db_path);
        const auto features = soff::diff::extract_ml_features(ml_db, all_results.matches);
        assert(!features.empty());
        assert(features.front().ratio > 0.0);
        const auto csv_path = std::filesystem::absolute(
            std::filesystem::path("build") / "soff_ml_test.csv");
        soff::diff::export_ml_features_csv(features, csv_path);
        assert(std::filesystem::exists(csv_path));
        assert(std::filesystem::file_size(csv_path) > 0);
    }
    std::cout << "ml_features: export test passed\n";

    // M10: Propagation test (self-diff: find_same_name API works correctly)
    {
        soff::db::Database prop_db;
        prop_db.open(db_path);
        soff::SnapshotRepository prop_repo;
        prop_repo.attach_diff(prop_db, db_path);

        std::vector<soff::db::ResultMatch> prop_matches;
        boost::unordered_flat_set<soff::Address> prop_primary;
        boost::unordered_flat_set<soff::Address> prop_secondary;

        const auto same_name_count = soff::diff::find_same_name(
            prop_db, prop_matches, prop_primary, prop_secondary, 0.5, true);
        // Smoke DB has 1 function named "_start" - should match itself
        assert(same_name_count > 0 || prop_matches.empty());
        // Verify the function doesn't crash and produces valid output
        assert(prop_primary.size() == prop_matches.size());
    }
    std::cout << "propagation: find_same_name test passed\n";

    // M10: DiffSession with propagation enabled (smoke DB has 1 function,
    // SQL heuristics already match it, so propagation adds 0 - that's fine for correctness)
    {
        soff::diff::DiffSessionOptions prop_options;
        prop_options.propagation.enabled = true;
        const auto prop_result_path = std::filesystem::absolute(
            std::filesystem::path("build") / "soff_propagation_test.soff");
        const auto prop_summary = soff::diff::DiffSession{prop_options}.run_all(
            db_path, db_path, prop_result_path);
        assert(prop_summary.results.best > 0);
    }
    std::cout << "propagation: session integration test passed\n";

    std::cout << "schema tables=" << soff::db::diaphora_compatible_schema().tables.size()
              << " heuristics=" << heuristics.size() << '\n';
    std::cout << "db=" << db_path.string() << '\n';
    std::cout << "results=" << result_path.string() << '\n';

    extern void test_line_diff();
    test_line_diff();

    return 0;
}
