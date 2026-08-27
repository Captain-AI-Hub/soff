// Regression tests for the fast-path remaining-functions pass.
//
// The stripped/patchdiff fast paths skip the SQL heuristics; without a
// dedicated pass the changed functions (usually the interesting ones) would
// stay unmatched. These tests build synthetic snapshots that trigger each
// fast path and assert the leftover pairs still get matched, mirroring
// Diaphora's find_remaining_functions() behaviour.

#include "soff/analysis/model.hpp"
#include "soff/db/repository.hpp"
#include "soff/db/result_repository.hpp"
#include "soff/diff/session.hpp"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

soff::FunctionFeature make_function(
    soff::Address address,
    const std::string& name,
    const std::string& bytes_hash,
    const std::string& clean_assembly,
    int nodes = 4,
    int instructions = 10)
{
    soff::FunctionFeature function;
    function.address = address;
    function.rva = address - 0x400000;
    function.segment_rva = function.rva;
    function.name = name;
    function.size = 0x20;
    function.node_count = static_cast<std::uint64_t>(nodes);
    function.edge_count = static_cast<std::uint64_t>(nodes);
    function.instruction_count = static_cast<std::uint64_t>(instructions);
    function.cyclomatic_complexity = 2;
    function.mnemonics = "push\nmov\npop\nret";
    function.assembly = clean_assembly;
    function.stripped_assembly = clean_assembly;
    function.bytes_hash = bytes_hash;
    function.function_hash = bytes_hash;
    return function;
}

const soff::db::ResultMatch* find_match(
    const soff::db::DiffResultSet& results,
    soff::Address primary)
{
    for (const auto& match : results.matches) {
        if (match.primary == primary) {
            return &match;
        }
    }
    return nullptr;
}

void test_stripped_fast_path_remaining()
{
    // 100 identical functions + 1 changed function at the same address.
    // 100/101 >= 99% address+hash match triggers the stripped fast path.
    soff::ProgramSnapshot primary;
    primary.architecture = "metapc";
    auto secondary = primary;

    constexpr int identical_count = 100;
    for (int i = 0; i < identical_count; ++i) {
        const auto address = static_cast<soff::Address>(0x401000 + i * 0x10);
        const auto name = "shared_" + std::to_string(i);
        primary.functions.push_back(make_function(
            address, name, "hash-" + std::to_string(i), "mov eax, 1\nret"));
    }
    // The changed pair: same address, different bytes, similar assembly.
    primary.functions.push_back(make_function(
        0x500000,
        "sub_500000",
        "hash-old",
        "push rbp\nmov eax, 1\npop rbp\nret\nnop"));
    secondary.functions = primary.functions;
    secondary.functions.back() = make_function(
        0x500000,
        "sub_500000",
        "hash-new",
        "push rbp\nmov eax, 2\npop rbp\nret\nnop");

    const auto primary_path = std::filesystem::absolute(
        std::filesystem::path("build") / "soff_fast_path_stripped_primary.sqlite");
    const auto secondary_path = std::filesystem::absolute(
        std::filesystem::path("build") / "soff_fast_path_stripped_secondary.sqlite");
    soff::SnapshotRepository repository;
    assert(repository.save(primary, primary_path));
    assert(repository.save(secondary, secondary_path));

    const auto result_path = std::filesystem::absolute(
        std::filesystem::path("build") / "soff_fast_path_stripped.soff");
    const auto summary = soff::diff::DiffSession{}.run_all(primary_path, secondary_path, result_path);

    // The changed function must be matched, not left unmatched.
    soff::db::ResultRepository result_repository;
    const auto results = result_repository.load(result_path);
    const auto* match = find_match(results, 0x500000);
    assert(match != nullptr);
    assert(match->secondary == 0x500000);
    assert(match->description == "Same binary with symbols stripped");
    assert(match->kind == soff::db::ResultKind::partial);
    assert(match->ratio >= 0.5 && match->ratio < 1.0);
    assert(summary.results.unmatched_primary == 0);
    assert(summary.results.unmatched_secondary == 0);
    std::cout << "fast_path: stripped remaining match passed (ratio=" << match->ratio << ")\n";
}

void test_patchdiff_fast_path_remaining()
{
    // 19 same-name functions with modified bytes (matched by name) +
    // 1 anonymous pair with different names. 19/20 >= 90% same-name match
    // triggers the patchdiff fast path.
    soff::ProgramSnapshot primary;
    primary.architecture = "metapc";
    soff::ProgramSnapshot secondary;
    secondary.architecture = "metapc";

    constexpr int named_count = 19;
    for (int i = 0; i < named_count; ++i) {
        const auto address = static_cast<soff::Address>(0x401000 + i * 0x10);
        const auto name = "named_" + std::to_string(i);
        primary.functions.push_back(make_function(
            address, name, "phash-a" + std::to_string(i), "mov eax, 1\nret"));
        // Same name, same cleaned text, but different bytes hash so the
        // equal-matches pass does not claim them first.
        secondary.functions.push_back(make_function(
            address, name, "phash-b" + std::to_string(i), "mov eax, 1\nret"));
    }
    // The anonymous renamed pair: sub_1000 in primary, sub_2000 in secondary,
    // identical cleaned assembly.
    primary.functions.push_back(make_function(
        0x410000,
        "sub_410000",
        "anon-old",
        "push rbp\nmov ebx, ecx\nshl ebx, 2\npop rbp\nret"));
    secondary.functions.push_back(make_function(
        0x420000,
        "sub_420000",
        "anon-new",
        "push rbp\nmov ebx, ecx\nshl ebx, 2\npop rbp\nret"));

    const auto primary_path = std::filesystem::absolute(
        std::filesystem::path("build") / "soff_fast_path_patch_primary.sqlite");
    const auto secondary_path = std::filesystem::absolute(
        std::filesystem::path("build") / "soff_fast_path_patch_secondary.sqlite");
    soff::SnapshotRepository repository;
    assert(repository.save(primary, primary_path));
    assert(repository.save(secondary, secondary_path));

    const auto result_path = std::filesystem::absolute(
        std::filesystem::path("build") / "soff_fast_path_patch.soff");
    const auto summary = soff::diff::DiffSession{}.run_all(primary_path, secondary_path, result_path);

    soff::db::ResultRepository result_repository;
    const auto results = result_repository.load(result_path);
    const auto* match = find_match(results, 0x410000);
    assert(match != nullptr);
    assert(match->secondary == 0x420000);
    assert(match->description == "Renamed or anonymous function match in patch diffing session");
    assert(match->ratio >= 0.6);
    assert(summary.results.unmatched_primary == 0);
    assert(summary.results.unmatched_secondary == 0);
    std::cout << "fast_path: patchdiff remaining match passed (ratio=" << match->ratio << ")\n";
}

} // namespace

void test_fast_path_remaining()
{
    test_stripped_fast_path_remaining();
    test_patchdiff_fast_path_remaining();
    std::cout << "fast_path: all remaining-match regressions passed\n";
}
