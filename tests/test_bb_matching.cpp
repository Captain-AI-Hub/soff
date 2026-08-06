#include "soff/analysis/model.hpp"
#include "soff/db/database.hpp"
#include "soff/db/repository.hpp"
#include "soff/diff/bb_matching.hpp"

#include <cassert>
#include <filesystem>
#include <iostream>

namespace {

soff::FunctionFeature cfg_function(soff::Address function_address, soff::Address micro_base, int micro_blocks)
{
    soff::FunctionFeature function;
    function.address = function_address;
    function.name = "cfg_function";
    function.node_count = 2;
    function.edge_count = 1;
    function.instruction_count = 3;
    function.stripped_assembly = "mov eax, ebx\ncmp eax, 1\nret";
    function.instruction_details.push_back({function_address, "mov eax, ebx", "mov"});
    function.instruction_details.push_back({function_address + 1, "cmp eax, 1", "cmp"});
    function.instruction_details.push_back({function_address + 0x10, "ret", "ret"});
    function.blocks.push_back({
        function_address,
        function_address + 2,
        {function_address, function_address + 1},
        {function_address + 0x10},
    });
    function.blocks.push_back({
        function_address + 0x10,
        function_address + 0x11,
        {function_address + 0x10},
        {},
    });

    for (int i = 0; i < micro_blocks; ++i) {
        const auto instruction_index = function.microcode_instruction_details.size();
        function.microcode_instruction_details.push_back({
            micro_base + static_cast<soff::Address>(i),
            i + 1 == micro_blocks ? "ret" : "mov t0, t1",
            i + 1 == micro_blocks ? "ret" : "mov",
        });
        function.microcode_blocks.push_back({
            micro_base + 0x100 + static_cast<soff::Address>(i),
            micro_base + 0x101 + static_cast<soff::Address>(i),
            {static_cast<soff::Address>(instruction_index)},
            i + 1 < micro_blocks
                ? std::vector<soff::Address>{micro_base + 0x101 + static_cast<soff::Address>(i)}
                : std::vector<soff::Address>{},
        });
    }
    return function;
}

} // namespace

void test_bb_matching_regressions()
{
    soff::ProgramSnapshot primary;
    primary.architecture = "metapc";
    primary.functions.push_back(cfg_function(0xA10000, 0x80000000, 3));

    soff::ProgramSnapshot secondary;
    secondary.architecture = "metapc";
    secondary.functions.push_back(cfg_function(0xB10000, 0x90000000, 1));

    const auto primary_path = std::filesystem::absolute(
        std::filesystem::path("build") / "soff_bb_primary.sqlite");
    const auto secondary_path = std::filesystem::absolute(
        std::filesystem::path("build") / "soff_bb_secondary.sqlite");
    soff::SnapshotRepository repository;
    assert(repository.save(primary, primary_path));
    assert(repository.save(secondary, secondary_path));

    soff::db::Database database;
    database.open(primary_path);
    repository.attach_diff(database, secondary_path);

    const auto native = soff::diff::match_basic_blocks(
        database, 0xA10000, 0xB10000, soff::diff::BasicBlockKind::native);
    assert(native.primary_blocks == 2);
    assert(native.secondary_blocks == 2);
    assert(native.primary_edges == 1);
    assert(native.secondary_edges == 1);
    assert(native.preserved_edges == 1);
    assert(native.similarity() > 0.7);

    const auto microcode = soff::diff::match_basic_blocks(
        database, 0xA10000, 0xB10000, soff::diff::BasicBlockKind::microcode);
    assert(microcode.primary_blocks == 3);
    assert(microcode.secondary_blocks == 1);

    soff::diff::BasicBlockMatchResult low_confidence;
    low_confidence.primary_blocks = 2;
    low_confidence.secondary_blocks = 2;
    low_confidence.matches = {
        {1, 10, 0.4, "degree+count"},
        {2, 20, 0.4, "degree+count"},
    };
    assert(low_confidence.similarity() < 0.6);

    std::cout << "bb_matching: native/microcode isolation and weighted confidence passed\n";
}
