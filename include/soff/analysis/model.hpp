#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace soff {

using Address = std::uint64_t;

struct BasicBlock
{
    Address start = 0;
    Address end = 0;
    std::vector<Address> instructions;
    std::vector<Address> successors;
};

struct InstructionFeature
{
    Address address = 0;
    std::string disassembly;
    std::string mnemonic;
    std::string comment1;
    std::string comment2;
    std::string operand_names;
    std::string name;
    std::string type;
    std::string pseudocomment;
    std::uint64_t pseudoitp = 0;
};

struct CallReference
{
    Address address = 0;
    std::string type;
};

struct FunctionFeature
{
    Address address = 0;
    Address rva = 0;
    Address segment_rva = 0;
    std::string name;
    std::uint64_t size = 0;
    std::uint64_t instruction_count = 0;
    std::uint64_t node_count = 0;
    std::uint64_t edge_count = 0;
    std::uint64_t indegree = 0;
    std::uint64_t outdegree = 0;
    std::uint64_t cyclomatic_complexity = 0;
    std::uint64_t strongly_connected = 0;
    std::uint64_t loops = 0;
    std::string mnemonics;
    std::string names;
    std::string assembly;
    std::string stripped_assembly;
    std::string assembly_addrs;
    std::string bytes_hash;
    std::string function_hash;
    std::string md_index;
    std::string kgh_hash = "1";
    std::string primes_value;
    std::string tarjan_topological_sort;
    std::string strongly_connected_spp = "1";
    std::string mnemonics_spp = "1";
    std::string switches = "[]";
    std::uint64_t bytes_sum = 0;
    std::uint64_t function_flags = 0;
    std::string mangled_function;
    std::string prototype;
    std::string prototype2;
    std::string comment;
    std::string source_file;
    std::string userdata;
    double export_time = 0.0;
    std::string pseudocode;
    std::string stripped_pseudocode;
    std::uint64_t pseudocode_lines = 0;
    std::string pseudocode_hash1;
    std::string pseudocode_hash2;
    std::string pseudocode_hash3;
    std::string pseudocode_primes;
    std::string microcode;
    std::string stripped_microcode;
    std::string microcode_spp = "1";
    std::vector<BasicBlock> blocks;
    std::vector<BasicBlock> microcode_blocks;
    std::vector<InstructionFeature> instruction_details;
    std::vector<InstructionFeature> microcode_instruction_details;
    std::vector<CallReference> call_references;
    std::vector<std::string> constants;
};

struct ProgramDataItem
{
    std::string name;
    std::string type;
    std::string value;
};

struct ProgramSnapshot
{
    std::string input_path;
    std::string architecture;
    std::string callgraph_primes;
    std::string callgraph_all_primes;
    std::vector<ProgramDataItem> program_data;
    std::vector<FunctionFeature> functions;
};

bool is_valid_snapshot(const ProgramSnapshot& snapshot);

} // namespace soff
