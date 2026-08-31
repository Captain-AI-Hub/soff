#include "soff/analysis/model.hpp"
#include "soff/db/database.hpp"
#include "soff/db/repository.hpp"
#include "soff/diff/function_cache.hpp"
#include "soff/diff/propagation.hpp"

#include <boost/unordered/unordered_flat_set.hpp>

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <iostream>

namespace {

soff::FunctionFeature function_feature(
    soff::Address address,
    std::string name,
    std::string clean_assembly,
    std::string clean_pseudo = {})
{
    soff::FunctionFeature function;
    function.address = address;
    function.name = std::move(name);
    function.node_count = 3;
    function.instruction_count = 3;
    function.stripped_assembly = std::move(clean_assembly);
    function.stripped_pseudocode = std::move(clean_pseudo);
    return function;
}

} // namespace

void test_propagation_regressions()
{
    soff::ProgramSnapshot primary;
    primary.architecture = "metapc";
    primary.functions.push_back(function_feature(
        0x410000, "same_name_changed", "mov eax, 1\nret"));
    primary.functions.push_back(function_feature(
        0x420000, "same_name_identical", "mov eax, 2\nret"));
    primary.functions.push_back(function_feature(
        0x430000, "duplicate_name", "mov eax, 3\nret"));
    primary.functions.push_back(function_feature(
        0x440000, "duplicate_name", "mov eax, 4\nret"));
    auto primary_mangled = function_feature(
        0x450000, "display_name_primary", "mov eax, 5\nret");
    primary_mangled.mangled_function = "_Z15shared_mangledv";
    primary.functions.push_back(primary_mangled);

    soff::ProgramSnapshot secondary;
    secondary.architecture = "metapc";
    secondary.functions.push_back(function_feature(
        0x510000, "same_name_changed", "xor ecx, ecx\nret"));
    secondary.functions.push_back(function_feature(
        0x520000, "same_name_identical", "mov eax, 2\nret"));
    secondary.functions.push_back(function_feature(
        0x530000, "duplicate_name", "mov eax, 3\nret"));
    secondary.functions.push_back(function_feature(
        0x540000, "duplicate_name", "mov eax, 4\nret"));
    auto secondary_mangled = function_feature(
        0x550000, "display_name_secondary", "mov eax, 5\nret");
    secondary_mangled.mangled_function = "_Z15shared_mangledv";
    secondary.functions.push_back(secondary_mangled);

    const auto primary_path = std::filesystem::absolute(
        std::filesystem::path("build") / "soff_same_name_primary.sqlite");
    const auto secondary_path = std::filesystem::absolute(
        std::filesystem::path("build") / "soff_same_name_secondary.sqlite");
    soff::SnapshotRepository repository;
    assert(repository.save(primary, primary_path));
    assert(repository.save(secondary, secondary_path));

    soff::db::Database database;
    database.open(primary_path);
    repository.attach_diff(database, secondary_path);
    const auto primary_cache = soff::diff::load_function_cache(database, "main");
    const auto secondary_cache = soff::diff::load_function_cache(database, "diff");
    assert(primary_cache.by_name.at("duplicate_name").size() == 2);
    assert(secondary_cache.by_name.at("duplicate_name").size() == 2);
    soff::diff::FunctionTextResolver primary_texts(database, "main");
    soff::diff::FunctionTextResolver secondary_texts(database, "diff");

    std::vector<soff::db::ResultMatch> matches;
    boost::unordered_flat_set<soff::Address> matched_primary;
    boost::unordered_flat_set<soff::Address> matched_secondary;
    const auto added = soff::diff::find_same_name(
        database,
        matches,
        matched_primary,
        matched_secondary,
        0.9,
        true,
        &primary_cache,
        &secondary_cache,
        &primary_texts,
        &secondary_texts);

    assert(added == 2);
    assert(matches.size() == 2);
    assert(matches.front().primary == 0x420000);
    assert(matches.front().secondary == 0x520000);
    assert(matches.front().ratio == 1.0);
    assert(matches.front().kind == soff::db::ResultKind::best);
    const auto mangled = std::find_if(matches.begin(), matches.end(), [](const auto& match) {
        return match.primary == 0x450000;
    });
    assert(mangled != matches.end());
    assert(mangled->secondary == 0x550000);
    assert(mangled->description == "Same mangled name propagation");
    assert(!matched_primary.contains(0x410000));
    assert(!matched_secondary.contains(0x510000));
    assert(!matched_primary.contains(0x430000));
    assert(!matched_secondary.contains(0x530000));

    std::cout << "propagation: cache scoring and duplicate-name regression passed\n";

    soff::ProgramSnapshot affine_primary;
    affine_primary.architecture = "metapc";
    affine_primary.functions.push_back(function_feature(0x610000, "seed_low_primary", "push rbp\nret"));
    affine_primary.functions.push_back(function_feature(0x620000, "affine_candidate", "mov eax, 1\nret"));
    affine_primary.functions.push_back(function_feature(0x630000, "seed_high_primary", "pop rbp\nret"));

    soff::ProgramSnapshot affine_secondary;
    affine_secondary.architecture = "metapc";
    affine_secondary.functions.push_back(function_feature(0x710000, "seed_low_secondary", "push rbp\nret"));
    affine_secondary.functions.push_back(function_feature(0x720000, "affine_candidate", "xor ecx, ecx\nret"));
    affine_secondary.functions.push_back(function_feature(0x730000, "seed_high_secondary", "pop rbp\nret"));

    const auto affine_primary_path = std::filesystem::absolute(
        std::filesystem::path("build") / "soff_affine_primary.sqlite");
    const auto affine_secondary_path = std::filesystem::absolute(
        std::filesystem::path("build") / "soff_affine_secondary.sqlite");
    assert(repository.save(affine_primary, affine_primary_path));
    assert(repository.save(affine_secondary, affine_secondary_path));

    soff::db::Database affine_database;
    affine_database.open(affine_primary_path);
    repository.attach_diff(affine_database, affine_secondary_path);
    std::vector<soff::db::ResultMatch> affine_seeds{
        {soff::db::ResultKind::best, 0, 0x610000, "seed_low_primary", 0x710000, "seed_low_secondary", 1.0, 3, 3, "seed"},
        {soff::db::ResultKind::best, 1, 0x630000, "seed_high_primary", 0x730000, "seed_high_secondary", 1.0, 3, 3, "seed"},
    };
    boost::unordered_flat_set<soff::Address> affine_primary_seen{0x610000, 0x630000};
    boost::unordered_flat_set<soff::Address> affine_secondary_seen{0x710000, 0x730000};
    const auto rejected_affine = soff::diff::find_locally_affine_functions(
        affine_database,
        affine_seeds,
        affine_primary_seen,
        affine_secondary_seen,
        0.9,
        10,
        true);
    assert(rejected_affine == 0);

    affine_seeds.resize(2);
    affine_primary_seen = {0x610000, 0x630000};
    affine_secondary_seen = {0x710000, 0x730000};
    const auto accepted_affine = soff::diff::find_locally_affine_functions(
        affine_database,
        affine_seeds,
        affine_primary_seen,
        affine_secondary_seen,
        0.4,
        10,
        true);
    assert(accepted_affine == 1);
    assert(affine_seeds.back().primary == 0x620000);
    assert(affine_seeds.back().secondary == 0x720000);
    assert(affine_seeds.back().ratio > 0.4 && affine_seeds.back().ratio < 0.7);
    std::cout << "propagation: affine threshold regression passed\n";

    soff::ProgramSnapshot call_primary;
    call_primary.architecture = "metapc";
    auto primary_seed = function_feature(0x810000, "primary_seed", "push rbp\nret");
    primary_seed.call_references.push_back({0x811000, "call"});
    primary_seed.call_references.push_back({0x812000, "call"});
    call_primary.functions.push_back(primary_seed);
    call_primary.functions.push_back(function_feature(0x811000, "primary_callee_one", "mov eax, 1\nret"));
    call_primary.functions.push_back(function_feature(0x812000, "primary_callee_two", "mov eax, 2\nret"));

    soff::ProgramSnapshot call_secondary;
    call_secondary.architecture = "metapc";
    auto secondary_seed = function_feature(0x910000, "secondary_seed", "push rbp\nret");
    secondary_seed.call_references.push_back({0x911000, "call"});
    secondary_seed.call_references.push_back({0x912000, "call"});
    call_secondary.functions.push_back(secondary_seed);
    call_secondary.functions.push_back(function_feature(0x911000, "secondary_callee_one", "mov eax, 1\nret"));
    call_secondary.functions.push_back(function_feature(0x912000, "secondary_callee_two", "mov eax, 2\nret"));

    const auto call_primary_path = std::filesystem::absolute(
        std::filesystem::path("build") / "soff_call_primary.sqlite");
    const auto call_secondary_path = std::filesystem::absolute(
        std::filesystem::path("build") / "soff_call_secondary.sqlite");
    assert(repository.save(call_primary, call_primary_path));
    assert(repository.save(call_secondary, call_secondary_path));

    soff::db::Database call_database;
    call_database.open(call_primary_path);
    repository.attach_diff(call_database, call_secondary_path);
    std::vector<soff::db::ResultMatch> call_matches{
        {soff::db::ResultKind::best, 0, 0x810000, "primary_seed", 0x910000, "secondary_seed", 1.0, 3, 3, "seed"},
    };
    boost::unordered_flat_set<soff::Address> call_primary_seen{0x810000};
    boost::unordered_flat_set<soff::Address> call_secondary_seen{0x910000};
    soff::diff::PropagationOptions call_options;
    call_options.max_iterations = 1;
    call_options.enable_slow = false;
    call_options.same_name_min_ratio = 0.8;
    call_options.affine_min_ratio = 0.99;
    call_options.diffing_min_ratio = 0.99;
    const auto call_stats = soff::diff::run_propagation(
        call_database,
        call_matches,
        call_primary_seen,
        call_secondary_seen,
        call_options);
    assert(call_stats.call_reference_matches == 2);
    assert(call_matches.size() == 3);
    assert(call_matches[1].primary == 0x811000);
    assert(call_matches[1].secondary == 0x911000);
    assert(call_matches[2].primary == 0x812000);
    assert(call_matches[2].secondary == 0x912000);
    std::cout << "propagation: call-reference ordering and scoring regression passed\n";
}
