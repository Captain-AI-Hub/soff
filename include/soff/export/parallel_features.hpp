#pragma once

#include "soff/analysis/model.hpp"
#include <string>
#include <vector>

namespace soff::exp {

/// Raw data collected from IDA on the main thread.
/// Contains everything needed to compute hashes and features
/// without any further IDA SDK calls.
struct RawFunctionData {
    Address address = 0;
    Address rva = 0;
    Address segment_rva = 0;
    std::string name;
    uint64_t size = 0;
    uint64_t function_flags = 0;
    std::string mangled_function;
    std::string prototype;
    std::string prototype2;
    std::string comment;
    std::string source_file;
    std::string userdata;

    // CFG data
    std::vector<BasicBlock> blocks;
    std::vector<BasicBlock> microcode_blocks;

    // Raw instruction data
    std::vector<InstructionFeature> instruction_details;
    std::vector<InstructionFeature> microcode_instruction_details;
    std::vector<CallReference> call_references;
    std::vector<std::string> constants;

    // Raw text (from IDA, before cleaning)
    std::string raw_assembly;
    std::string raw_assembly_addrs;
    std::string raw_mnemonics;
    std::string raw_names;
    std::string raw_switches;

    // Decompiler output (from IDA main thread)
    std::string pseudocode;
    std::string microcode;

    // Raw bytes for hashing
    std::string bytes_for_hash;
    std::string instructions_for_hash;
};

/// Compute all derived features from raw IDA data.
/// This is a pure function — thread-safe, no IDA SDK calls.
FunctionFeature compute_features(RawFunctionData&& raw);

} // namespace soff::exp
