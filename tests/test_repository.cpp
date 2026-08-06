#include "soff/analysis/model.hpp"
#include "soff/db/atomic_writer.hpp"
#include "soff/db/database.hpp"
#include "soff/db/repository.hpp"
#include "soff/diff/propagation.hpp"
#include "soff/diff/session.hpp"

#include <boost/unordered/unordered_flat_set.hpp>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

void test_repository_regressions()
{
    const auto atomic_path = std::filesystem::absolute(
        std::filesystem::path("build") / "soff_atomic_writer_test.bin");
    {
        std::ofstream old_file(atomic_path, std::ios::binary | std::ios::trunc);
        old_file << "old";
    }
    {
        soff::db::AtomicFileWriter writer(atomic_path);
        std::ofstream temporary(writer.temporary_path(), std::ios::binary | std::ios::trunc);
        temporary << "uncommitted";
    }
    {
        std::ifstream old_file(atomic_path, std::ios::binary);
        std::string value;
        old_file >> value;
        assert(value == "old");
    }
    {
        soff::db::AtomicFileWriter writer(atomic_path);
        {
            std::ofstream temporary(writer.temporary_path(), std::ios::binary | std::ios::trunc);
            temporary << "new";
        }
        writer.commit();
    }
    {
        std::ifstream new_file(atomic_path, std::ios::binary);
        std::string value;
        new_file >> value;
        assert(value == "new");
    }
    std::cout << "repository: atomic file replacement regression passed\n";

    // Fault injection: make the destination an existing directory so the
    // atomic replacement fails after any SQLite sidecars have been moved.
    // The original destination and sidecar must remain intact, and the
    // temporary output must be cleaned by the writer destructor.
    const auto fault_path = std::filesystem::absolute(
        std::filesystem::path("build") / "soff_atomic_writer_fault_test.bin");
    const auto fault_wal = std::filesystem::path(fault_path.string() + "-wal");
    std::error_code cleanup_error;
    std::filesystem::remove_all(fault_path, cleanup_error);
    std::filesystem::remove(fault_wal, cleanup_error);
    std::filesystem::create_directory(fault_path);
    {
        std::ofstream wal_file(fault_wal, std::ios::binary | std::ios::trunc);
        wal_file << "stable-wal";
    }
    std::filesystem::path temporary_path;
    bool injected_failure = false;
    {
        soff::db::AtomicFileWriter writer(fault_path);
        temporary_path = writer.temporary_path();
        {
            std::ofstream temporary(writer.temporary_path(), std::ios::binary | std::ios::trunc);
            temporary << "faulted-new";
        }
        try {
            writer.commit();
        } catch (const std::exception&) {
            injected_failure = true;
        }
    }
    assert(injected_failure);
    assert(std::filesystem::is_directory(fault_path));
    assert(std::filesystem::exists(fault_wal));
    assert(!std::filesystem::exists(temporary_path));
    {
        std::ifstream wal_file(fault_wal, std::ios::binary);
        std::string value;
        wal_file >> value;
        assert(value == "stable-wal");
    }
    std::filesystem::remove_all(fault_path, cleanup_error);
    std::filesystem::remove(fault_wal, cleanup_error);
    std::cout << "repository: save fault-injection regression passed\n";

    soff::ProgramSnapshot snapshot;
    snapshot.architecture = "metapc";

    soff::FunctionFeature first;
    first.address = 0x401000;
    first.name = "first_source_function";
    first.source_file = "src/john's/main.cpp";
    first.node_count = 2;
    first.instruction_count = 2;
    first.stripped_assembly = "mov eax, 1\nret";
    snapshot.functions.push_back(first);

    soff::FunctionFeature second;
    second.address = 0x402000;
    second.name = "second_source_function";
    second.source_file = "src/john's/main.cpp";
    second.node_count = 3;
    second.instruction_count = 3;
    second.stripped_assembly = "mov eax, 2\nret";
    snapshot.functions.push_back(second);

    const auto path = std::filesystem::absolute(
        std::filesystem::path("build") / "soff_compilation_unit_regression.sqlite");
    soff::SnapshotRepository repository;
    assert(repository.save(snapshot, path));
    repository.save_compilation_units(path);

    soff::db::Database database;
    database.open(path);
    assert(database.query_int("select count(*) from compilation_units") == 1);
    assert(database.query_int("select functions from compilation_units limit 1") == 2);
    assert(database.query_text("select name from compilation_units limit 1") == "src/john's/main.cpp");
    assert(database.query_int("select count(*) from compilation_unit_functions") == 2);

    const auto rows = database.query_rows(
        "select f.address from compilation_unit_functions cuf "
        "inner join functions f on f.id = cuf.func_id order by f.address");
    assert(rows.size() == 2);
    assert(rows[0][0] == "4198400");
    assert(rows[1][0] == "4202496");

    std::cout << "repository: compilation-unit schema regression passed\n";

    auto secondary_snapshot = snapshot;
    secondary_snapshot.functions[0].address = 0x501000;
    secondary_snapshot.functions[1].address = 0x502000;
    const auto secondary_path = std::filesystem::absolute(
        std::filesystem::path("build") / "soff_compilation_unit_secondary.sqlite");
    assert(repository.save(secondary_snapshot, secondary_path));
    repository.save_compilation_units(secondary_path);

    soff::db::Database diff_database;
    diff_database.open(path);
    repository.attach_diff(diff_database, secondary_path);
    std::vector<soff::db::ResultMatch> matches;
    boost::unordered_flat_set<soff::Address> matched_primary;
    boost::unordered_flat_set<soff::Address> matched_secondary;
    const auto added = soff::diff::find_compilation_unit_matches(
        diff_database, matches, matched_primary, matched_secondary, 0.5, true);
    assert(added == 2);
    assert(matches.size() == 2);
    assert(matches[0].description == "Same compilation unit");
    std::cout << "repository: compilation-unit propagation regression passed\n";

    const auto original_function_count = diff_database.query_int("select count(*) from functions");
    bool rejected_output_alias = false;
    try {
        (void)soff::diff::DiffSession{}.run_all(path, secondary_path, path);
    } catch (const std::invalid_argument&) {
        rejected_output_alias = true;
    }
    assert(rejected_output_alias);
    assert(diff_database.query_int("select count(*) from functions") == original_function_count);
    std::cout << "repository: output/input alias regression passed\n";
}
