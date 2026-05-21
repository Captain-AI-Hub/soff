#include "soff/export/parallel_features.hpp"

namespace soff::exp {

FunctionFeature compute_features(RawFunctionData&& raw)
{
    FunctionFeature f;
    f.address = raw.address;
    f.rva = raw.rva;
    f.segment_rva = raw.segment_rva;
    f.name = std::move(raw.name);
    f.size = raw.size;
    f.function_flags = raw.function_flags;
    f.mangled_function = std::move(raw.mangled_function);
    f.prototype = std::move(raw.prototype);
    f.prototype2 = std::move(raw.prototype2);
    f.comment = std::move(raw.comment);
    f.source_file = std::move(raw.source_file);
    f.userdata = std::move(raw.userdata);
    f.blocks = std::move(raw.blocks);
    f.microcode_blocks = std::move(raw.microcode_blocks);
    f.instruction_details = std::move(raw.instruction_details);
    f.microcode_instruction_details = std::move(raw.microcode_instruction_details);
    f.call_references = std::move(raw.call_references);
    f.constants = std::move(raw.constants);
    f.assembly = std::move(raw.raw_assembly);
    f.assembly_addrs = std::move(raw.raw_assembly_addrs);
    f.mnemonics = std::move(raw.raw_mnemonics);
    f.names = std::move(raw.raw_names);
    f.switches = std::move(raw.raw_switches);
    f.pseudocode = std::move(raw.pseudocode);
    f.microcode = std::move(raw.microcode);
    f.instruction_count = raw.instruction_details.size();
    f.node_count = f.blocks.size();
    // Hashes and topology are computed by the caller (plugin)
    // since they depend on plugin-local helper functions.
    return f;
}

} // namespace soff::exp
