#include "soff/diff/heuristics.hpp"

#include <algorithm>
#include <array>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_set>

namespace soff::diff {

namespace {

constexpr std::array<std::string_view, 49> function_columns{
    "id",
    "name",
    "address",
    "nodes",
    "edges",
    "indegree",
    "outdegree",
    "size",
    "instructions",
    "mnemonics",
    "names",
    "prototype",
    "cyclomatic_complexity",
    "primes_value",
    "comment",
    "mangled_function",
    "bytes_hash",
    "pseudocode",
    "pseudocode_lines",
    "pseudocode_hash1",
    "pseudocode_primes",
    "function_flags",
    "assembly",
    "prototype2",
    "pseudocode_hash2",
    "pseudocode_hash3",
    "strongly_connected",
    "loops",
    "rva",
    "tarjan_topological_sort",
    "strongly_connected_spp",
    "clean_assembly",
    "clean_pseudo",
    "mnemonics_spp",
    "switches",
    "function_hash",
    "bytes_sum",
    "md_index",
    "constants",
    "constants_count",
    "segment_rva",
    "assembly_addrs",
    "kgh_hash",
    "source_file",
    "userdata",
    "microcode",
    "clean_microcode",
    "microcode_spp",
    "export_time",
};

bool is_function_column(std::string_view name)
{
    return std::find(function_columns.begin(), function_columns.end(), name) != function_columns.end();
}

const std::regex& function_field_regex()
{
    static const std::regex function_ref(R"REGEX(\b(?:f|df)\.([A-Za-z_][A-Za-z0-9_]*)\b)REGEX");
    return function_ref;
}

} // namespace

std::string_view category_name(HeuristicCategory category)
{
    switch (category) {
    case HeuristicCategory::best:
        return "Best";
    case HeuristicCategory::partial:
        return "Partial";
    case HeuristicCategory::experimental:
        return "Experimental";
    case HeuristicCategory::unreliable:
        return "Unreliable";
    }
    return "Unknown";
}

std::string_view ratio_mode_name(RatioMode mode)
{
    switch (mode) {
    case RatioMode::no_false_positives:
        return "No false positives";
    case RatioMode::ratio:
        return "Ratio";
    case RatioMode::ratio_with_minimum:
        return "Ratio with minimum";
    case RatioMode::trusted_ratio_with_minimum:
        return "Trusted ratio with minimum";
    }
    return "Unknown";
}

bool has_flag(const HeuristicDefinition& heuristic, HeuristicFlag flag)
{
    return (heuristic.flags & flag) != 0;
}

bool supports_same_cpu_only(const HeuristicDefinition& heuristic)
{
    return has_flag(heuristic, heuristic_flag_same_cpu);
}

bool is_slow(const HeuristicDefinition& heuristic)
{
    return has_flag(heuristic, heuristic_flag_slow);
}

bool is_unreliable(const HeuristicDefinition& heuristic)
{
    return heuristic.category == HeuristicCategory::unreliable
        || has_flag(heuristic, heuristic_flag_unreliable);
}

std::vector<std::string> required_fields(const HeuristicDefinition& heuristic)
{
    std::unordered_set<std::string> seen;
    std::vector<std::string> fields;
    const std::string sql{heuristic.sql};
    for (auto it = std::sregex_iterator(sql.begin(), sql.end(), function_field_regex()); it != std::sregex_iterator(); ++it) {
        auto field = (*it)[1].str();
        if (seen.insert(field).second) {
            fields.push_back(std::move(field));
        }
    }
    std::sort(fields.begin(), fields.end());
    return fields;
}

const std::vector<HeuristicDefinition>& builtin_heuristics()
{
    static const std::vector<HeuristicDefinition> heuristics{
        {
            "Same RVA and hash",
            HeuristicCategory::best,
            RatioMode::no_false_positives,
            heuristic_flag_same_cpu,
            0.0,
            R"SOFFSQL( select  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Same RVA and hash' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

              from functions f,
                   diff.functions df
             where (df.rva = f.rva
                 or df.segment_rva = f.segment_rva)
               and df.bytes_hash = f.bytes_hash
               and df.instructions = f.instructions
               and ((f.name = df.name and substr(f.name, 1, 4) != 'sub_')
                 or (substr(f.name, 1, 4) = 'sub_' or substr(df.name, 1, 4) = 'sub_'))
               and f.nodes >= 3
               and df.nodes >= 3
               %POSTFIX%)SOFFSQL",
        },
        {
            "Same order and hash",
            HeuristicCategory::best,
            RatioMode::no_false_positives,
            heuristic_flag_same_cpu,
            0.0,
            R"SOFFSQL( select  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Same order and hash' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

              from functions f,
                   diff.functions df
             where df.id = f.id
               and df.bytes_hash = f.bytes_hash
               and df.instructions = f.instructions
               and ((f.name = df.name and substr(f.name, 1, 4) != 'sub_')
                 or (substr(f.name, 1, 4) = 'sub_' or substr(df.name, 1, 4) = 'sub_'))
               and ((f.nodes > 1 and df.nodes > 1
                 and f.instructions > 5 and df.instructions > 5)
                  or f.instructions > 10 and df.instructions > 10)
               %POSTFIX%)SOFFSQL",
        },
        {
            "Function Hash",
            HeuristicCategory::best,
            RatioMode::no_false_positives,
            heuristic_flag_same_cpu,
            0.0,
            R"SOFFSQL( select distinct  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Function Hash' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

              from functions f,
                   diff.functions df
             where f.function_hash = df.function_hash 
               and ((f.nodes > 1 and df.nodes > 1
                 and f.instructions > 5 and df.instructions > 5)
                  or f.instructions > 10 and df.instructions > 10)
               %POSTFIX%)SOFFSQL",
        },
        {
            "Bytes hash",
            HeuristicCategory::best,
            RatioMode::no_false_positives,
            heuristic_flag_same_cpu,
            0.0,
            R"SOFFSQL( select distinct  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Bytes hash' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

              from functions f,
                   diff.functions df
             where f.bytes_hash = df.bytes_hash
               and f.instructions > 5 and df.instructions > 5
               %POSTFIX%)SOFFSQL",
        },
        {
            "Same address and mnemonics",
            HeuristicCategory::best,
            RatioMode::ratio,
            heuristic_flag_none,
            0.0,
            R"SOFFSQL( select distinct  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Same address and mnemonics' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

              from functions f,
                   diff.functions df
             where df.address = f.address
               and df.mnemonics = f.mnemonics
               and df.instructions = f.instructions
               and df.instructions > 5
               and ((f.name = df.name and substr(f.name, 1, 4) != 'sub_')
                 or (substr(f.name, 1, 4) = 'sub_' or substr(df.name, 1, 4) = 'sub_'))
               %POSTFIX%
             order by f.source_file = df.source_file)SOFFSQL",
        },
        {
            "Same cleaned assembly",
            HeuristicCategory::best,
            RatioMode::ratio,
            heuristic_flag_same_cpu,
            0.0,
            R"SOFFSQL( select  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Same cleaned assembly' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

        from functions f,
             diff.functions df
       where f.clean_assembly = df.clean_assembly
         and f.nodes >= 3 and df.nodes >= 3
         and f.name not like 'nullsub%'
         and df.name not like 'nullsub%'
         %POSTFIX%
       order by f.source_file = df.source_file)SOFFSQL",
        },
        {
            "Same cleaned microcode",
            HeuristicCategory::best,
            RatioMode::ratio,
            heuristic_flag_same_cpu,
            0.0,
            R"SOFFSQL( select  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Same cleaned microcode' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

        from functions f,
             diff.functions df
       where f.clean_microcode = df.clean_microcode
         and length(coalesce(f.clean_microcode, '')) > 0
         and length(coalesce(df.clean_microcode, '')) > 0
         and f.instructions > 3 and df.instructions > 3
         and f.name not like 'nullsub%'
         and df.name not like 'nullsub%'
         %POSTFIX%
       order by f.source_file = df.source_file)SOFFSQL",
        },
        {
            "Same cleaned pseudo-code",
            HeuristicCategory::best,
            RatioMode::ratio,
            heuristic_flag_none,
            0.0,
            R"SOFFSQL( select  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Same cleaned pseudo-code' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

        from functions f,
             diff.functions df
       where f.clean_pseudo = df.clean_pseudo
         and length(coalesce(f.clean_pseudo, '')) > 0
         and length(coalesce(df.clean_pseudo, '')) > 0
         and f.pseudocode_lines > 5 and df.pseudocode_lines > 5
         and f.name not like 'nullsub%'
         and df.name not like 'nullsub%'
         %POSTFIX%
       order by f.source_file = df.source_file)SOFFSQL",
        },
        {
            "Same address, nodes, edges and mnemonics",
            HeuristicCategory::best,
            RatioMode::ratio,
            heuristic_flag_none,
            0.0,
            R"SOFFSQL(select  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Same address, nodes, edges and mnemonics' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

       from functions f,
            diff.functions df
      where f.rva = df.rva
        and f.instructions = df.instructions
        and f.nodes = df.nodes
        and f.edges = df.edges
        and f.mnemonics = df.mnemonics
        and f.instructions > 3
        and df.instructions > 3
        and f.nodes > 1
        %POSTFIX%
      order by f.source_file = df.source_file)SOFFSQL",
        },
        {
            "Same RVA",
            HeuristicCategory::best,
            RatioMode::ratio_with_minimum,
            heuristic_flag_same_cpu,
            0.7,
            R"SOFFSQL( select distinct  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Same RVA' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

              from functions f,
                   diff.functions df
             where df.rva = f.rva
               and ((f.name = df.name and substr(f.name, 1, 4) != 'sub_')
                or (substr(f.name, 1, 4) = 'sub_' or substr(df.name, 1, 4) = 'sub_'))
               and  f.nodes >= 3
               and df.nodes >= 3
               %POSTFIX%
             order by f.source_file = df.source_file)SOFFSQL",
        },
        {
            "Equal assembly or pseudo-code",
            HeuristicCategory::best,
            RatioMode::no_false_positives,
            heuristic_flag_none,
            0.0,
            R"SOFFSQL(select  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Equal pseudo-code' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

       from functions f,
            diff.functions df
      where f.pseudocode = df.pseudocode
        and df.pseudocode is not null
        and f.pseudocode_lines >= 5
        and f.name not like 'nullsub%'
        and df.name not like 'nullsub%'
        %POSTFIX%
      union
     select  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Equal assembly' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

       from functions f,
            diff.functions df
      where f.assembly = df.assembly
        and df.assembly is not null
        and f.instructions >= 4 and df.instructions >= 4
        and f.name not like 'nullsub%'
        and df.name not like 'nullsub%'
        %POSTFIX%)SOFFSQL",
        },
        {
            "Microcode mnemonics small primes product",
            HeuristicCategory::best,
            RatioMode::ratio,
            heuristic_flag_none,
            0.0,
            R"SOFFSQL( select  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Microcode mnemonics small primes product' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

        from functions f,
             diff.functions df
       where f.microcode_spp = df.microcode_spp
         and f.microcode_spp != 1
         and df.microcode_spp != 1
         and f.instructions > 5 and df.instructions > 5
         and f.nodes > 2 and df.nodes > 2
         and f.name not like 'nullsub%'
         and df.name not like 'nullsub%'
         %POSTFIX%
       order by f.source_file = df.source_file)SOFFSQL",
        },
        {
            "Same named compilation unit function match",
            HeuristicCategory::partial,
            RatioMode::trusted_ratio_with_minimum,
            heuristic_flag_none,
            0.44,
            R"SOFFSQL(  select  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Same named compilation unit function match' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

               from main.compilation_units main_cu,
                    main.compilation_unit_functions mcuf,
                    main.functions f,
                    diff.compilation_units diff_cu,
                    diff.compilation_unit_functions dcuf,
                    diff.functions df
              where main_cu.name != ''
                and diff_cu.name != ''
                and main_cu.name = diff_cu.name
                and f.id = mcuf.func_id
                and df.id = dcuf.func_id
                and mcuf.cu_id = main_cu.id
                and dcuf.cu_id = diff_cu.id
                and df.primes_value = f.primes_value
                and df.nodes = f.nodes
                and f.nodes >= 5
                %POSTFIX%)SOFFSQL",
        },
        {
            "Same anonymous compilation unit function match",
            HeuristicCategory::partial,
            RatioMode::ratio_with_minimum,
            heuristic_flag_none,
            0.449,
            R"SOFFSQL(  select  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Same anonymous compilation unit function match' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

               from main.compilation_units main_cu,
                    main.compilation_unit_functions mcuf,
                    main.functions f,
                    diff.compilation_units diff_cu,
                    diff.compilation_unit_functions dcuf,
                    diff.functions df
              where main_cu.name != ''
                and diff_cu.name != ''
                and main_cu.name = diff_cu.name
                and f.id = mcuf.func_id
                and df.id = dcuf.func_id
                and mcuf.cu_id = main_cu.id
                and dcuf.cu_id = diff_cu.id
                and df.pseudocode_primes = f.pseudocode_primes
                and df.nodes = f.nodes
                and f.nodes >= 5
                %POSTFIX% 
              order by f.source_file = df.source_file)SOFFSQL",
        },
        {
            "Same compilation unit",
            HeuristicCategory::partial,
            RatioMode::ratio,
            heuristic_flag_slow,
            0.0,
            R"SOFFSQL(select  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Same compilation unit' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

                from main.compilation_units mcu,
                  main.compilation_unit_functions mcuf,
                  main.functions f,
                  diff.compilation_units dcu,
                  diff.compilation_unit_functions dcuf,
                  diff.functions df
              where dcu.pseudocode_primes = mcu.pseudocode_primes
                and mcuf.cu_id = mcu.id
                and dcuf.cu_id = dcu.id
                and f.id = mcuf.func_id
                and df.id = dcuf.func_id
                and f.nodes > 4
                and df.nodes > 4
                and (substr(f.name, 1, 4) = 'sub_' or substr(df.name, 1, 4) == 'sub_')
                %POSTFIX%)SOFFSQL",
        },
        {
            "Same KOKA hash and constants",
            HeuristicCategory::partial,
            RatioMode::ratio,
            heuristic_flag_none,
            0.0,
            R"SOFFSQL(select  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Same KOKA hash and constants' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

       from main.constants mc,
            diff.constants dc,
            main.functions  f,
            diff.functions df
      where mc.constant = dc.constant
        and  f.id = mc.func_id
        and df.id = dc.func_id
        and f.kgh_hash = df.kgh_hash
        and f.nodes >= 3
        %POSTFIX%)SOFFSQL",
        },
        {
            "Same KOKA hash and MD-Index",
            HeuristicCategory::partial,
            RatioMode::ratio,
            heuristic_flag_none,
            0.0,
            R"SOFFSQL(
     select  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Same KOKA hash and MD-Index' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

       from functions f,
            diff.functions df
      where f.kgh_hash = df.kgh_hash
        and f.md_index = df.md_index
        and f.nodes = df.nodes
        and f.nodes >= 4
        and f.outdegree = df.outdegree
        and f.indegree  = df.indegree
        and (substr(f.name, 1, 4) = 'sub_' or substr(df.name, 1, 4) = 'sub_')
        %POSTFIX%)SOFFSQL",
        },
        {
            "Same constants",
            HeuristicCategory::partial,
            RatioMode::ratio_with_minimum,
            heuristic_flag_none,
            0.5,
            R"SOFFSQL(select  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Same constants' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

       from functions f,
            diff.functions df
      where f.constants = df.constants
        and f.constants_count = df.constants_count
        and f.constants_count > 1
        %POSTFIX%
      order by f.source_file = df.source_file)SOFFSQL",
        },
        {
            "Same rare KOKA hash",
            HeuristicCategory::partial,
            RatioMode::ratio_with_minimum,
            heuristic_flag_none,
            0.45,
            R"SOFFSQL(
with shared_hashes as (
 select kgh_hash
   from diff.functions
  where kgh_hash != 0
  group by kgh_hash
 having count(*) <= 2
  union 
 select kgh_hash
   from main.functions
  where kgh_hash != 0
  group by kgh_hash
 having count(*) <= 2
)
select  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Same rare KOKA hash' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

  from functions f,
       diff.functions df,
       shared_hashes
 where f.kgh_hash = df.kgh_hash
   and df.kgh_hash = shared_hashes.kgh_hash
   and f.nodes > 5
   and (substr(f.name, 1, 4) = 'sub_'
     or substr(df.name, 1, 4) = 'sub_')
   %POSTFIX%)SOFFSQL",
        },
        {
            "Same rare MD Index",
            HeuristicCategory::partial,
            RatioMode::ratio,
            heuristic_flag_none,
            0.0,
            R"SOFFSQL(
     with shared_mds as (
      select md_index
        from diff.functions
       where md_index != 0
       group by md_index
      having count(*) <= 2
      union 
      select md_index
        from main.functions
       where md_index != 0
       group by md_index
      having count(*) <= 2
     )
     select  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Same rare MD Index' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

       from functions f,
            diff.functions df,
            shared_mds
      where f.md_index = df.md_index
        and df.md_index = shared_mds.md_index
        and f.nodes > 10
        %POSTFIX%
      order by f.source_file = df.source_file)SOFFSQL",
        },
        {
            "Same address and rare constant",
            HeuristicCategory::partial,
            RatioMode::ratio_with_minimum,
            heuristic_flag_none,
            0.5,
            R"SOFFSQL(select distinct  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Same address and rare constant' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

       from main.constants mc,
            diff.constants dc,
            main.functions  f,
            diff.functions df
      where mc.constant = dc.constant
        and  f.id = mc.func_id
        and df.id = dc.func_id
        and df.address = f.address
        %POSTFIX%
      order by f.source_file = df.source_file)SOFFSQL",
        },
        {
            "Same rare constant",
            HeuristicCategory::partial,
            RatioMode::ratio_with_minimum,
            heuristic_flag_slow,
            0.2,
            R"SOFFSQL(select  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Same rare constant' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

       from main.constants mc,
            diff.constants dc,
            main.functions  f,
            diff.functions df
      where mc.constant = dc.constant
        and  f.id = mc.func_id
        and df.id = dc.func_id
        and f.nodes >= 3 and df.nodes >= 3
        and f.constants_count > 0
        %POSTFIX%)SOFFSQL",
        },
        {
            "Same MD Index and constants",
            HeuristicCategory::partial,
            RatioMode::ratio,
            heuristic_flag_none,
            0.0,
            R"SOFFSQL( select distinct  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Same MD Index and constants' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

        from functions f,
             diff.functions df
       where f.md_index = df.md_index
         and f.md_index > 0
         and f.nodes >= 3 and df.nodes >= 3
         and ((f.constants = df.constants
         and f.constants_count > 0))
        %POSTFIX%
      order by f.source_file = df.source_file)SOFFSQL",
        },
        {
            "Import names hash",
            HeuristicCategory::partial,
            RatioMode::ratio,
            heuristic_flag_none,
            0.0,
            R"SOFFSQL(select distinct  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Import names hash' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

              from functions f,
                  diff.functions df
            where f.names = df.names
              and f.names != '[]'
              and f.md_index = df.md_index
              and f.instructions = df.instructions
              and f.nodes > 5 and df.nodes > 5
              %POSTFIX%
            order by f.source_file = df.source_file)SOFFSQL",
        },
        {
            "Mnemonics and names",
            HeuristicCategory::partial,
            RatioMode::ratio,
            heuristic_flag_none,
            0.0,
            R"SOFFSQL( select  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Mnemonics and names' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

        from functions f,
             diff.functions df
       where f.mnemonics = df.mnemonics
         and f.instructions = df.instructions
         and f.names = df.names
         and f.names != '[]'
         and f.instructions > 5 and df.instructions > 5
         %POSTFIX%
       order by f.source_file = df.source_file)SOFFSQL",
        },
        {
            "Pseudo-code fuzzy hash",
            HeuristicCategory::partial,
            RatioMode::ratio,
            heuristic_flag_none,
            0.0,
            R"SOFFSQL(select distinct  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Pseudo-code fuzzy hash' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

       from functions f,
            diff.functions df
      where df.pseudocode_hash1 = f.pseudocode_hash1
        and df.pseudocode_hash2 = f.pseudocode_hash2
        and df.pseudocode_hash3 = f.pseudocode_hash3
        and df.pseudocode_hash1 is not null
        and df.pseudocode_hash2 is not null
        and df.pseudocode_hash3 is not null
        and f.instructions > 5
        and df.instructions > 5
        %POSTFIX%
      order by f.source_file = df.source_file)SOFFSQL",
        },
        {
            "Similar pseudo-code and names",
            HeuristicCategory::partial,
            RatioMode::ratio_with_minimum,
            heuristic_flag_none,
            0.579,
            R"SOFFSQL(select distinct  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Similar pseudo-code and names' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

       from functions f,
            diff.functions df
      where f.pseudocode_lines = df.pseudocode_lines
        and f.names = df.names
        and df.names != '[]'
        and df.pseudocode_lines > 5
        and df.pseudocode is not null 
        and f.pseudocode is not null
        %POSTFIX%
      order by f.source_file = df.source_file)SOFFSQL",
        },
        {
            "Mnemonics small-primes-product",
            HeuristicCategory::partial,
            RatioMode::ratio_with_minimum,
            heuristic_flag_none,
            0.6,
            R"SOFFSQL( select  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Mnemonics small-primes-product' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

        from functions f,
             diff.functions df
       where f.mnemonics_spp = df.mnemonics_spp
         and f.instructions = df.instructions
         and f.nodes > 1 and df.nodes > 1
         and df.instructions > 5
         %POSTFIX%)SOFFSQL",
        },
        {
            "Same nodes, edges, loops and strongly connected components",
            HeuristicCategory::partial,
            RatioMode::ratio_with_minimum,
            heuristic_flag_none,
            0.549,
            R"SOFFSQL(select  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Same nodes, edges, loops and strongly connected components' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

       from functions f,
            diff.functions df
      where f.nodes = df.nodes
        and f.edges = df.edges
        and f.strongly_connected = df.strongly_connected
        and f.loops = df.loops
        and f.nodes > 5 and df.nodes > 5
        and f.loops > 0
        and (substr(f.name, 1, 4) = 'sub_' or substr(df.name, 1, 4) == 'sub_')
        %POSTFIX%)SOFFSQL",
        },
        {
            "Same low complexity, prototype and names",
            HeuristicCategory::partial,
            RatioMode::ratio_with_minimum,
            heuristic_flag_none,
            0.5,
            R"SOFFSQL(
       select distinct  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Same low complexity, prototype and names' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

         from functions f,
              diff.functions df
        where f.names = df.names
          and f.cyclomatic_complexity = df.cyclomatic_complexity
          and f.cyclomatic_complexity < 20
          and f.prototype2 = df.prototype2
          and df.names != '[]'
          %POSTFIX%)SOFFSQL",
        },
        {
            "Same low complexity and names",
            HeuristicCategory::partial,
            RatioMode::ratio_with_minimum,
            heuristic_flag_none,
            0.5,
            R"SOFFSQL(select  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Same low complexity and names' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

       from functions f,
            diff.functions df
      where f.names = df.names
        and f.cyclomatic_complexity = df.cyclomatic_complexity
        and f.cyclomatic_complexity < 15
        and df.names != '[]'
        and (substr(f.name, 1, 4) = 'sub_' or substr(df.name, 1, 4) == 'sub_')
        %POSTFIX%)SOFFSQL",
        },
        {
            "Switch structures",
            HeuristicCategory::partial,
            RatioMode::ratio_with_minimum,
            heuristic_flag_none,
            0.5,
            R"SOFFSQL(select  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Switch structures' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

       from functions f,
            diff.functions df
      where f.switches = df.switches
        and df.switches != '[]'
        and f.nodes > 5 and df.nodes > 5
        %POSTFIX%
      order by f.source_file = df.source_file)SOFFSQL",
        },
        {
            "Pseudo-code fuzzy (normal)",
            HeuristicCategory::partial,
            RatioMode::ratio_with_minimum,
            heuristic_flag_none,
            0.5,
            R"SOFFSQL(select distinct  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Pseudo-code fuzzy (normal)' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

       from functions f,
            diff.functions df
      where df.pseudocode_hash1 = f.pseudocode_hash1
        and f.pseudocode_lines > 5 and df.pseudocode_lines > 5
        %POSTFIX%
      order by f.source_file = df.source_file)SOFFSQL",
        },
        {
            "Pseudo-code fuzzy (mixed)",
            HeuristicCategory::partial,
            RatioMode::ratio,
            heuristic_flag_none,
            0.0,
            R"SOFFSQL(select distinct  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Pseudo-code fuzzy (mixed)' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

       from functions f,
            diff.functions df
      where df.pseudocode_hash3 = f.pseudocode_hash3
        and f.pseudocode_lines > 5 and df.pseudocode_lines > 5
        %POSTFIX%
      order by f.source_file = df.source_file)SOFFSQL",
        },
        {
            "Pseudo-code fuzzy (reverse)",
            HeuristicCategory::partial,
            RatioMode::ratio,
            heuristic_flag_none,
            0.0,
            R"SOFFSQL(select distinct  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Pseudo-code fuzzy (reverse)' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

       from functions f,
            diff.functions df
      where df.pseudocode_hash2 = f.pseudocode_hash2
        and f.pseudocode_lines > 5 and df.pseudocode_lines > 5
        %POSTFIX%
      order by f.source_file = df.source_file)SOFFSQL",
        },
        {
            "Pseudo-code fuzzy AST hash",
            HeuristicCategory::partial,
            RatioMode::ratio_with_minimum,
            heuristic_flag_none,
            0.35,
            R"SOFFSQL(select distinct  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Pseudo-code fuzzy AST hash' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

       from functions f,
            diff.functions df
      where df.pseudocode_primes = f.pseudocode_primes
        and f.pseudocode_lines >= 3
        and length(f.pseudocode_primes) >= 35
        %POSTFIX%
      order by f.source_file = df.source_file)SOFFSQL",
        },
        {
            "Partial pseudo-code fuzzy hash (normal)",
            HeuristicCategory::partial,
            RatioMode::ratio_with_minimum,
            heuristic_flag_slow | heuristic_flag_unreliable,
            0.5,
            R"SOFFSQL(  select distinct  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Partial pseudo-code fuzzy hash (normal)' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

         from functions f,
              diff.functions df
        where substr(df.pseudocode_hash1, 1, 16) = substr(f.pseudocode_hash1, 1, 16)
          and f.nodes > 5 and df.nodes > 5
          %POSTFIX%
        order by f.source_file = df.source_file)SOFFSQL",
        },
        {
            "Partial pseudo-code fuzzy hash (reverse)",
            HeuristicCategory::partial,
            RatioMode::ratio_with_minimum,
            heuristic_flag_slow | heuristic_flag_unreliable,
            0.5,
            R"SOFFSQL(  select distinct  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Partial pseudo-code fuzzy hash (reverse)' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

         from functions f,
              diff.functions df
        where substr(df.pseudocode_hash2, 1, 16) = substr(f.pseudocode_hash2, 1, 16)
          and f.nodes > 5 and df.nodes > 5
          %POSTFIX%
        order by f.source_file = df.source_file)SOFFSQL",
        },
        {
            "Partial pseudo-code fuzzy hash (mixed)",
            HeuristicCategory::partial,
            RatioMode::ratio_with_minimum,
            heuristic_flag_slow | heuristic_flag_unreliable,
            0.5,
            R"SOFFSQL(  select distinct  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Partial pseudo-code fuzzy hash (mixed)' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

         from functions f,
              diff.functions df
        where substr(df.pseudocode_hash3, 1, 16) = substr(f.pseudocode_hash3, 1, 16)
          and f.nodes > 5 and df.nodes > 5
          %POSTFIX%
        order by f.source_file = df.source_file)SOFFSQL",
        },
        {
            "Same rare assembly instruction",
            HeuristicCategory::partial,
            RatioMode::ratio_with_minimum,
            heuristic_flag_same_cpu,
            0.5,
            R"SOFFSQL(
with main_asm as (
  select f.id, f.name, inst.disasm
    from main.instructions inst,
         main.functions f
   where f.id = inst.func_id
     and f.name not like 'nullsub%'
     and inst.disasm is not null
     and inst.disasm != ''
     and coalesce(inst.asm_type, '') = ''
   group by inst.disasm
  having count(0) = 1
),
diff_asm as (
  select f.id, f.name, inst.disasm
    from diff.instructions inst,
         diff.functions f
   where f.id = inst.func_id
     and f.name not like 'nullsub%'
     and inst.disasm is not null
     and inst.disasm != ''
     and coalesce(inst.asm_type, '') = ''
   group by inst.disasm
  having count(0) = 1
),
query1 as (
  select distinct main_asm.id main_func_id, diff_asm.id diff_func_id
    from main_asm,
         diff_asm
   where main_asm.disasm = diff_asm.disasm
)
select  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Same rare assembly instruction' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

  from main.functions f,
       diff.functions df,
       query1
 where f.id  = query1.main_func_id
   and df.id = query1.diff_func_id
   and f.name != df.name
   and ((min(f.nodes, df.nodes) * 100) / max(f.nodes, df.nodes)) < 50
   %POSTFIX%)SOFFSQL",
        },
        {
            "Same rare basic block mnemonics list",
            HeuristicCategory::partial,
            RatioMode::ratio_with_minimum,
            heuristic_flag_none,
            0.5,
            R"SOFFSQL(
with main_bblocks as (
select inst.func_id, bb.basic_block_id bb_id, GROUP_CONCAT(inst.mnemonic) as mnemonics_list, count(0) inst_total
  from main.bb_instructions bb,
       main.instructions inst
 where bb.instruction_id = inst.id
   and coalesce(inst.asm_type, '') = ''
 group by bb_id
),
diff_bblocks as (
select inst.func_id, bb.basic_block_id bb_id, GROUP_CONCAT(inst.mnemonic) as mnemonics_list, count(0) inst_total
  from diff.bb_instructions bb,
       diff.instructions inst
 where bb.instruction_id = inst.id
   and coalesce(inst.asm_type, '') = ''
 group by bb_id
),
unique_main_bblocks as (
select func_id, mnemonics_list, count(0) total
  from main_bblocks
 group by mnemonics_list
having count(0) = 1
 order by total asc
)
select  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Same rare basic block mnemonics list' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

  from unique_main_bblocks main_query,
       diff_bblocks diff_query,
       main.functions f,
       diff.functions df
 where main_query.mnemonics_list = diff_query.mnemonics_list
   and f.id = main_query.func_id
   and df.id = diff_query.func_id
   and f.nodes > 3
   and df.nodes > 3
   and diff_query.inst_total >= 6
   and ((min(f.nodes, df.nodes) * 100) / max(f.nodes, df.nodes)) < 50
   %POSTFIX%)SOFFSQL",
        },
        {
            "Loop count",
            HeuristicCategory::partial,
            RatioMode::ratio_with_minimum,
            heuristic_flag_slow,
            0.49,
            R"SOFFSQL(select  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Loop count' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

       from functions f,
            diff.functions df
      where f.loops = df.loops
        and df.loops > 1
        and f.nodes >= 3 and df.nodes >= 3
        %POSTFIX%
      order by f.source_file = df.source_file)SOFFSQL",
        },
        {
            "Same graph",
            HeuristicCategory::unreliable,
            RatioMode::ratio_with_minimum,
            heuristic_flag_none,
            0.5,
            R"SOFFSQL( select  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Same graph' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

        from functions f,
             diff.functions df
       where f.nodes = df.nodes 
         and f.edges = df.edges
         and f.indegree = df.indegree
         and f.outdegree = df.outdegree
         and f.cyclomatic_complexity = df.cyclomatic_complexity
         and f.strongly_connected = df.strongly_connected
         and f.loops = df.loops
         and f.tarjan_topological_sort = df.tarjan_topological_sort
         and f.strongly_connected_spp = df.strongly_connected_spp
         and f.nodes > 5 and df.nodes > 5
         %POSTFIX%
       order by
             case when f.size = df.size then 1 else 0 end +
             case when f.instructions = df.instructions then 1 else 0 end +
             case when f.mnemonics = df.mnemonics then 1 else 0 end +
             case when f.names = df.names then 1 else 0 end +
             case when f.prototype2 = df.prototype2 then 1 else 0 end +
             case when f.primes_value = df.primes_value then 1 else 0 end +
             case when f.bytes_hash = df.bytes_hash then 1 else 0 end +
             case when f.pseudocode_hash1 = df.pseudocode_hash1 then 1 else 0 end +
             case when f.pseudocode_primes = df.pseudocode_primes then 1 else 0 end +
             case when f.pseudocode_hash2 = df.pseudocode_hash2 then 1 else 0 end +
             case when f.pseudocode_hash3 = df.pseudocode_hash3 then 1 else 0 end DESC)SOFFSQL",
        },
        {
            "Strongly connected components",
            HeuristicCategory::unreliable,
            RatioMode::ratio_with_minimum,
            heuristic_flag_slow,
            0.8,
            R"SOFFSQL(
     select  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Strongly connected components' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

       from functions f,
            diff.functions df
      where f.strongly_connected = df.strongly_connected
        and df.strongly_connected > 1
        and f.nodes > 5 and df.nodes > 5
        and f.strongly_connected_spp > 1
        and df.strongly_connected_spp > 1
        %POSTFIX%
      order by f.source_file = df.source_file)SOFFSQL",
        },
        {
            "Nodes, edges, complexity and mnemonics",
            HeuristicCategory::unreliable,
            RatioMode::ratio,
            heuristic_flag_slow,
            0.0,
            R"SOFFSQL( select distinct  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Nodes, edges, complexity and mnemonics' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

        from functions f,
             diff.functions df
       where f.nodes = df.nodes
         and f.edges = df.edges
         and f.mnemonics = df.mnemonics
         and f.cyclomatic_complexity = df.cyclomatic_complexity
         and f.nodes > 1 and f.edges > 0
         %POSTFIX%
       order by f.source_file = df.source_file)SOFFSQL",
        },
        {
            "Nodes, edges, complexity and prototype",
            HeuristicCategory::unreliable,
            RatioMode::ratio,
            heuristic_flag_slow,
            0.0,
            R"SOFFSQL( select distinct  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Nodes, edges, complexity and prototype' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

        from functions f,
             diff.functions df
       where f.nodes = df.nodes
         and f.edges = df.edges
         and f.prototype2 = df.prototype2
         and f.cyclomatic_complexity = df.cyclomatic_complexity
         and f.prototype2 != 'int()'
         %POSTFIX%
       order by f.source_file = df.source_file)SOFFSQL",
        },
        {
            "Nodes, edges, complexity, in-degree and out-degree",
            HeuristicCategory::unreliable,
            RatioMode::ratio,
            heuristic_flag_slow,
            0.0,
            R"SOFFSQL( select distinct  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Nodes, edges, complexity, in-degree and out-degree' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

        from functions f,
             diff.functions df
       where f.nodes = df.nodes
         and f.edges = df.edges
         and f.cyclomatic_complexity = df.cyclomatic_complexity
         and f.nodes >= 3 and f.edges > 2
         and f.indegree = df.indegree
         and f.outdegree = df.outdegree
         %POSTFIX%
       order by f.source_file = df.source_file)SOFFSQL",
        },
        {
            "Nodes, edges and complexity",
            HeuristicCategory::unreliable,
            RatioMode::ratio,
            heuristic_flag_slow,
            0.0,
            R"SOFFSQL( select distinct  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Nodes, edges and complexity' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

        from functions f,
             diff.functions df
       where f.nodes = df.nodes
         and f.edges = df.edges
         and f.cyclomatic_complexity = df.cyclomatic_complexity
         and f.nodes > 1 and f.edges > 0
         %POSTFIX%
       order by f.source_file = df.source_file)SOFFSQL",
        },
        {
            "Same high complexity",
            HeuristicCategory::unreliable,
            RatioMode::ratio,
            heuristic_flag_slow,
            0.0,
            R"SOFFSQL(select  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Same high complexity' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

       from functions f,
            diff.functions df
      where f.cyclomatic_complexity = df.cyclomatic_complexity
        and f.cyclomatic_complexity >= 50
        %POSTFIX%
      order by f.source_file = df.source_file)SOFFSQL",
        },
        {
            "Topological sort hash",
            HeuristicCategory::unreliable,
            RatioMode::ratio,
            heuristic_flag_none,
            0.0,
            R"SOFFSQL(select  f.address ea, f.name name1, df.address ea2, df.name name2,
                  'Topological sort hash' description,
                  f.pseudocode pseudo1, df.pseudocode pseudo2,
                  f.assembly asm1, df.assembly asm2,
                  f.pseudocode_primes pseudo_primes1, df.pseudocode_primes pseudo_primes2,
                  f.nodes nodes1, df.nodes nodes2,
                  cast(f.md_index as real) md1, cast(df.md_index as real) md2,
                  f.clean_assembly clean_assembly1, df.clean_assembly clean_assembly2,
                  f.clean_pseudo clean_pseudo1, df.clean_pseudo clean_pseudo2,
                  f.mangled_function mangled1, df.mangled_function mangled2,
                  f.clean_microcode clean_micro1, df.clean_microcode clean_micro2,
                  f.bytes_hash bytes_hash1, df.bytes_hash bytes_hash2,
                  f.edges edges1, df.edges edges2,
                  f.indegree indegree1, df.indegree indegree2,
                  f.outdegree outdegree1, df.outdegree outdegree2,
                  f.instructions instructions1, df.instructions instructions2,
                  f.cyclomatic_complexity cc1, df.cyclomatic_complexity cc2,
                  f.strongly_connected strongly_connected1,
                  df.strongly_connected strongly_connected2,
                  f.loops loops1, df.loops loops2,
                  f.constants_count constants_count1,
                  df.constants_count constants_count2,
                  f.size size1, df.size size2,
                  f.kgh_hash kgh_hash1, df.kgh_hash kgh_hash2

       from functions f,
            diff.functions df
      where f.strongly_connected = df.strongly_connected
        and f.tarjan_topological_sort = df.tarjan_topological_sort
        and f.strongly_connected >= 3
        and f.nodes > 10
        %POSTFIX%
      order by f.source_file = df.source_file)SOFFSQL",
        },
    };
    return heuristics;
}

std::vector<std::string> validate_builtin_heuristics()
{
    std::vector<std::string> issues;
    std::unordered_set<std::string_view> names;

    for (const auto& heuristic : builtin_heuristics()) {
        if (!names.insert(heuristic.name).second) {
            issues.emplace_back(std::string("duplicate heuristic name: ") + std::string(heuristic.name));
        }

        const bool ratio_with_minimum = heuristic.ratio_mode == RatioMode::ratio_with_minimum
            || heuristic.ratio_mode == RatioMode::trusted_ratio_with_minimum;
        if (ratio_with_minimum && heuristic.minimum_ratio <= 0.0) {
            issues.emplace_back(std::string("missing minimum ratio: ") + std::string(heuristic.name));
        }

        if (heuristic.sql.find("%POSTFIX%") == std::string_view::npos) {
            issues.emplace_back(std::string("missing %POSTFIX% marker: ") + std::string(heuristic.name));
        }

        const auto fields = required_fields(heuristic);
        if (fields.empty()) {
            issues.emplace_back(std::string("heuristic has no required function fields: ") + std::string(heuristic.name));
        }
        for (const auto& field : fields) {
            if (!is_function_column(field)) {
                std::ostringstream stream;
                stream << "unknown functions field in " << heuristic.name << ": " << field;
                issues.emplace_back(stream.str());
            }
        }

        if (has_flag(heuristic, heuristic_flag_same_cpu) && has_flag(heuristic, heuristic_flag_slow)) {
            issues.emplace_back(std::string("unexpected same-cpu and slow flag combination: ") + std::string(heuristic.name));
        }
    }

    return issues;
}

} // namespace soff::diff
