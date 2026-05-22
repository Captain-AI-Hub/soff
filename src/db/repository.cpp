#include "soff/db/repository.hpp"

#include "soff/db/schema.hpp"

#include <charconv>
#include <map>
#include <sstream>
#include <stdexcept>

namespace soff {
namespace {

std::string address_to_text(Address address)
{
    return std::to_string(address);
}

std::string json_string_array(const std::vector<std::string>& values)
{
    std::ostringstream out;
    out << '[';
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        out << '"';
        for (const char ch : values[i]) {
            if (ch == '"' || ch == '\\') {
                out << '\\';
            }
            out << ch;
        }
        out << '"';
    }
    out << ']';
    return out.str();
}

Address parse_address(const std::string& text)
{
    Address address = 0;
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, address, 10);
    if (result.ec == std::errc{} && result.ptr == end) {
        return address;
    }

    std::size_t consumed = 0;
    address = static_cast<Address>(std::stoull(text, &consumed, 0));
    if (consumed != text.size()) {
        throw std::runtime_error("invalid address text: " + text);
    }
    return address;
}

std::string create_index_sql(std::size_t index, const db::IndexDefinition& definition)
{
    std::ostringstream sql;
    sql << "create index if not exists idx_" << index
        << " on " << definition.table
        << "(" << definition.fields << ")";
    return sql.str();
}

std::vector<InstructionFeature> load_instruction_rows(
    db::Database& database,
    const std::string& function_id,
    std::string_view asm_type)
{
    auto statement = database.prepare(
        "select address, coalesce(disasm, ''), coalesce(mnemonic, ''), "
        "coalesce(comment1, ''), coalesce(comment2, ''), coalesce(operand_names, ''), "
        "coalesce(name, ''), coalesce(type, ''), coalesce(pseudocomment, ''), "
        "coalesce(pseudoitp, '') "
        "from instructions where func_id = ? and coalesce(asm_type, '') = ? order by id");
    statement.bind(1, function_id);
    statement.bind(2, asm_type);

    std::vector<InstructionFeature> instructions;
    while (statement.step()) {
        InstructionFeature instruction;
        instruction.address = parse_address(statement.column_text(0));
        instruction.disassembly = statement.column_text(1);
        instruction.mnemonic = statement.column_text(2);
        instruction.comment1 = statement.column_text(3);
        instruction.comment2 = statement.column_text(4);
        instruction.operand_names = statement.column_text(5);
        instruction.name = statement.column_text(6);
        instruction.type = statement.column_text(7);
        instruction.pseudocomment = statement.column_text(8);
        const auto pseudoitp = statement.column_text(9);
        instruction.pseudoitp = pseudoitp.empty() ? 0 : static_cast<std::uint64_t>(std::stoull(pseudoitp));
        instructions.push_back(std::move(instruction));
    }
    return instructions;
}

std::vector<CallReference> load_call_rows(db::Database& database, const std::string& function_id)
{
    auto statement = database.prepare(
        "select coalesce(address, ''), coalesce(type, '') "
        "from callgraph where func_id = ? order by id");
    statement.bind(1, function_id);

    std::vector<CallReference> calls;
    while (statement.step()) {
        const auto address_text = statement.column_text(0);
        if (address_text.empty()) {
            continue;
        }
        calls.push_back({
            parse_address(address_text),
            statement.column_text(1),
        });
    }
    return calls;
}

std::vector<BasicBlock> load_block_rows(
    db::Database& database,
    const std::string& function_id,
    std::string_view asm_type)
{
    auto block_statement = database.prepare(
        "select bb.id, coalesce(bb.address, '') "
        "from function_bblocks fb "
        "join basic_blocks bb on bb.id = fb.basic_block_id "
        "where fb.function_id = ? "
        "and coalesce(fb.asm_type, '') = ? "
        "and coalesce(bb.asm_type, '') = ? "
        "order by coalesce(bb.num, 0), bb.id");
    block_statement.bind(1, function_id);
    block_statement.bind(2, asm_type);
    block_statement.bind(3, asm_type);

    struct BlockRow
    {
        std::int64_t id = 0;
        BasicBlock block;
    };

    std::vector<BlockRow> rows;
    std::map<std::int64_t, std::size_t> index_by_id;
    while (block_statement.step()) {
        const auto block_id = block_statement.column_int64(0);
        const auto address_text = block_statement.column_text(1);
        BasicBlock block;
        block.start = address_text.empty() ? 0 : parse_address(address_text);
        rows.push_back({block_id, std::move(block)});
        index_by_id.emplace(block_id, rows.size() - 1);
    }

    for (auto& row : rows) {
        auto instruction_statement = database.prepare(
            "select coalesce(i.address, '') "
            "from bb_instructions bi "
            "join instructions i on i.id = bi.instruction_id "
            "where bi.basic_block_id = ? "
            "order by bi.id");
        instruction_statement.bind(1, std::to_string(row.id));
        while (instruction_statement.step()) {
            const auto address_text = instruction_statement.column_text(0);
            if (!address_text.empty()) {
                row.block.instructions.push_back(parse_address(address_text));
            }
        }
        if (!row.block.instructions.empty()) {
            row.block.end = row.block.instructions.back() + 1;
        } else {
            row.block.end = row.block.start;
        }
    }

    auto edge_statement = database.prepare(
        "select br.parent_id, br.child_id "
        "from bb_relations br "
        "join function_bblocks fp on fp.basic_block_id = br.parent_id "
        "join function_bblocks fc on fc.basic_block_id = br.child_id "
        "where fp.function_id = ? and fc.function_id = ? "
        "and coalesce(fp.asm_type, '') = ? and coalesce(fc.asm_type, '') = ? "
        "order by br.id");
    edge_statement.bind(1, function_id);
    edge_statement.bind(2, function_id);
    edge_statement.bind(3, asm_type);
    edge_statement.bind(4, asm_type);
    while (edge_statement.step()) {
        const auto parent_id = edge_statement.column_int64(0);
        const auto child_id = edge_statement.column_int64(1);
        const auto parent = index_by_id.find(parent_id);
        const auto child = index_by_id.find(child_id);
        if (parent == index_by_id.end() || child == index_by_id.end()) {
            continue;
        }
        rows[parent->second].block.successors.push_back(rows[child->second].block.start);
    }

    std::vector<BasicBlock> blocks;
    blocks.reserve(rows.size());
    for (auto& row : rows) {
        blocks.push_back(std::move(row.block));
    }
    return blocks;
}

} // namespace

SnapshotRepository::SnapshotRepository(SnapshotVersionPolicy version_policy)
    : version_policy_(version_policy)
{
}

std::string_view SnapshotRepository::export_version() const noexcept
{
    switch (version_policy_) {
    case SnapshotVersionPolicy::soff:
        return soff_version_value;
    case SnapshotVersionPolicy::diaphora_34:
        return diaphora_version_value;
    }
    return soff_version_value;
}

void SnapshotRepository::create_schema(const std::filesystem::path& path) const
{
    db::Database database;
    database.open(path);
    database.execute("PRAGMA foreign_keys = ON");

    const auto& schema = db::diaphora_compatible_schema();
    for (const auto table : schema.tables) {
        database.execute(table);
    }

    if (database.query_int("select count(*) from version") == 0) {
        database.execute("insert into version values (?)", {std::string(export_version())});
    }
}

void SnapshotRepository::create_indices(const std::filesystem::path& path) const
{
    db::Database database;
    database.open(path);

    const auto& indexes = db::diaphora_compatible_schema().indexes;
    for (std::size_t i = 0; i < indexes.size(); ++i) {
        database.execute(create_index_sql(i, indexes[i]));
    }
    database.execute("analyze");
}

void SnapshotRepository::attach_diff(
    db::Database& database,
    const std::filesystem::path& diff_path,
    std::string_view schema_name) const
{
    database.attach(diff_path, schema_name);
}

void insert_function_rows(db::Database& database, const FunctionFeature& function)
{
    database.execute(
        "insert into functions (name, address, rva, segment_rva, nodes, edges, size, instructions, "
        "indegree, outdegree, cyclomatic_complexity, primes_value, strongly_connected, loops, "
        "tarjan_topological_sort, strongly_connected_spp, mnemonics_spp, switches, mnemonics, names, "
        "prototype, mangled_function, bytes_hash, assembly, prototype2, function_flags, "
        "clean_assembly, function_hash, bytes_sum, md_index, constants_count, constants, "
        "assembly_addrs, kgh_hash, source_file, userdata, export_time, comment, pseudocode, clean_pseudo, pseudocode_lines, pseudocode_hash1, "
        "pseudocode_hash2, pseudocode_hash3, pseudocode_primes, microcode, clean_microcode, microcode_spp) "
        "values (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, "
        "nullif(?, ''), nullif(?, ''), ?, nullif(?, ''), nullif(?, ''), nullif(?, ''), nullif(?, ''), "
        "nullif(?, ''), nullif(?, ''), ?)",
        {
            function.name,
            address_to_text(function.address),
            address_to_text(function.rva),
            address_to_text(function.segment_rva),
            std::to_string(function.node_count != 0 ? function.node_count : function.blocks.size()),
            std::to_string(function.edge_count),
            std::to_string(function.size),
            std::to_string(function.instruction_count),
            std::to_string(function.indegree),
            std::to_string(function.outdegree),
            std::to_string(function.cyclomatic_complexity),
            function.primes_value,
            std::to_string(function.strongly_connected),
            std::to_string(function.loops),
            function.tarjan_topological_sort,
            function.strongly_connected_spp,
            function.mnemonics_spp,
            function.switches,
            function.mnemonics,
            function.names,
            function.prototype,
            function.mangled_function,
            function.bytes_hash,
            function.assembly,
            function.prototype2,
            std::to_string(function.function_flags),
            function.stripped_assembly,
            function.function_hash,
            std::to_string(function.bytes_sum),
            function.md_index,
            std::to_string(function.constants.size()),
            json_string_array(function.constants),
            function.assembly_addrs,
            function.kgh_hash,
            function.source_file,
            function.userdata,
            std::to_string(function.export_time),
            function.comment,
            function.pseudocode,
            function.stripped_pseudocode,
            std::to_string(function.pseudocode_lines),
            function.pseudocode_hash1,
            function.pseudocode_hash2,
            function.pseudocode_hash3,
            function.pseudocode_primes,
            function.microcode,
            function.stripped_microcode,
            function.microcode_spp,
        });
    const auto function_id = database.query_int("select last_insert_rowid()");

    const auto save_instruction_rows = [&database, function_id](
                                           const std::vector<InstructionFeature>& instructions,
                                           std::string_view asm_type) {
        std::vector<std::int64_t> ids;
        ids.reserve(instructions.size());
        for (const auto& instruction : instructions) {
            database.execute(
                "insert into instructions (func_id, address, disasm, mnemonic, comment1, comment2, "
                "operand_names, name, type, pseudocomment, pseudoitp, asm_type) "
                "values (?, ?, ?, ?, nullif(?, ''), nullif(?, ''), nullif(?, ''), nullif(?, ''), "
                "nullif(?, ''), nullif(?, ''), nullif(?, ''), ?)",
                {
                    std::to_string(function_id),
                    address_to_text(instruction.address),
                    instruction.disassembly,
                    instruction.mnemonic,
                    instruction.comment1,
                    instruction.comment2,
                    instruction.operand_names,
                    instruction.name,
                    instruction.type,
                    instruction.pseudocomment,
                    instruction.pseudocomment.empty() ? "" : std::to_string(instruction.pseudoitp),
                    std::string(asm_type),
                });
            ids.push_back(database.query_int("select last_insert_rowid()"));
        }
        return ids;
    };

    const auto save_block_rows = [&database, function_id](
                                     const std::vector<BasicBlock>& blocks,
                                     const std::vector<std::int64_t>& instruction_ids,
                                     std::string_view asm_type) {
        std::map<Address, std::int64_t> block_ids;
        for (std::size_t block_index = 0; block_index < blocks.size(); ++block_index) {
            const auto& block = blocks[block_index];
            database.execute(
                "insert into basic_blocks (num, address, asm_type) values (?, ?, ?)",
                {
                    std::to_string(block_index),
                    address_to_text(block.start),
                    std::string(asm_type),
                });
            const auto block_id = database.query_int("select last_insert_rowid()");
            block_ids[block.start] = block_id;
            database.execute(
                "insert into function_bblocks (function_id, basic_block_id, asm_type) values (?, ?, ?)",
                {
                    std::to_string(function_id),
                    std::to_string(block_id),
                    std::string(asm_type),
                });

            for (const auto instruction_address : block.instructions) {
                const auto index = static_cast<std::size_t>(instruction_address);
                if (index >= instruction_ids.size()) {
                    continue;
                }
                database.execute(
                    "insert into bb_instructions (basic_block_id, instruction_id) values (?, ?)",
                    {
                        std::to_string(block_id),
                        std::to_string(instruction_ids[index]),
                    });
            }
        }

        for (const auto& block : blocks) {
            const auto parent = block_ids.find(block.start);
            if (parent == block_ids.end()) {
                continue;
            }
            for (const auto successor : block.successors) {
                const auto child = block_ids.find(successor);
                if (child == block_ids.end()) {
                    continue;
                }
                database.execute(
                    "insert into bb_relations (parent_id, child_id) values (?, ?)",
                    {
                        std::to_string(parent->second),
                        std::to_string(child->second),
                    });
            }
        }
    };

    const auto native_instruction_ids = save_instruction_rows(function.instruction_details, "");
    std::map<Address, std::size_t> native_instruction_indices;
    for (std::size_t i = 0; i < function.instruction_details.size(); ++i) {
        native_instruction_indices[function.instruction_details[i].address] = i;
    }
    auto native_blocks = function.blocks;
    for (auto& block : native_blocks) {
        for (auto& instruction_address : block.instructions) {
            const auto instruction = native_instruction_indices.find(instruction_address);
            instruction_address = instruction == native_instruction_indices.end()
                ? static_cast<Address>(native_instruction_ids.size())
                : static_cast<Address>(instruction->second);
        }
    }
    save_block_rows(native_blocks, native_instruction_ids, "");

    const auto microcode_instruction_ids = save_instruction_rows(function.microcode_instruction_details, "microcode");
    save_block_rows(function.microcode_blocks, microcode_instruction_ids, "microcode");

    for (const auto& call : function.call_references) {
        database.execute(
            "insert into callgraph (func_id, address, type) values (?, ?, ?)",
            {
                std::to_string(function_id),
                address_to_text(call.address),
                call.type,
            });
    }

    for (const auto& constant : function.constants) {
        database.execute(
            "insert into constants (func_id, constant) values (?, ?)",
            {
                std::to_string(function_id),
                constant,
            });
    }
}

bool SnapshotRepository::save(const ProgramSnapshot& snapshot, const std::filesystem::path& path) const
{
    begin_incremental_save(snapshot, path, true);
    append_functions(snapshot.functions, path);
    replace_program_data(snapshot.program_data, path);
    finalize_incremental_save(path);
    return true;
}

void SnapshotRepository::begin_incremental_save(
    const ProgramSnapshot& snapshot,
    const std::filesystem::path& path,
    bool replace) const
{
    if (replace && std::filesystem::exists(path)) {
        std::filesystem::remove(path);
    }

    create_schema(path);

    db::Database database;
    database.open(path);
    database.execute("begin immediate");
    try {
        if (database.query_int("select count(*) from program") == 0) {
            database.execute(
                "insert into program (processor, md5sum) values (?, ?)",
                {snapshot.architecture, ""});
        }
        database.execute("commit");
    } catch (...) {
        database.execute("rollback");
        throw;
    }
}

void SnapshotRepository::append_functions(
    const std::vector<FunctionFeature>& functions,
    const std::filesystem::path& path) const
{
    if (functions.empty()) {
        return;
    }

    db::Database database;
    database.open(path);
    database.execute("begin immediate");
    try {
        for (const auto& function : functions) {
            insert_function_rows(database, function);
        }
        database.execute("commit");
    } catch (...) {
        database.execute("rollback");
        throw;
    }
}

void SnapshotRepository::replace_program_data(
    const std::vector<ProgramDataItem>& program_data,
    const std::filesystem::path& path) const
{
    db::Database database;
    database.open(path);
    database.execute("begin immediate");
    try {
        database.execute("delete from program_data");
        for (const auto& item : program_data) {
            database.execute(
                "insert into program_data (name, type, value) values (?, ?, ?)",
                {item.name, item.type, item.value});
        }
        database.execute("commit");
    } catch (...) {
        database.execute("rollback");
        throw;
    }
}

void SnapshotRepository::update_callgraph_primes(
    const std::string& primes,
    const std::string& all_primes,
    const std::filesystem::path& path) const
{
    db::Database database;
    database.open(path);
    database.execute(
        "update program set callgraph_primes = ?, callgraph_all_primes = ?",
        {primes, all_primes});
}

void SnapshotRepository::finalize_incremental_save(const std::filesystem::path& path) const
{
    create_indices(path);
}

void SnapshotRepository::save_compilation_units(const std::filesystem::path& path) const
{
    db::Database database;
    database.open(path);

    const auto rows = database.query_rows(
        "select distinct source_file from functions "
        "where source_file is not null and source_file != '' "
        "order by source_file");

    if (rows.empty()) return;

    database.execute("delete from compilation_units");
    database.execute("delete from compilation_unit_functions");

    int cu_id = 1;
    for (const auto& row : rows) {
        if (row.empty() || row[0].empty()) continue;
        const auto& name = row[0];

        const auto func_rows = database.query_rows(
            "select address, min(address), max(address) from functions "
            "where source_file = '" + name + "' "
            "group by source_file");
        if (func_rows.empty()) continue;

        std::int64_t start_ea = 0, end_ea = 0;
        if (func_rows.front().size() >= 3) {
            try { start_ea = std::stoll(func_rows.front()[1]); } catch (...) {}
            try { end_ea = std::stoll(func_rows.front()[2]); } catch (...) {}
        }

        database.execute(
            "insert into compilation_units (id, name, start_ea, end_ea, "
            "module_name, is_named, functions_count) values ("
            + std::to_string(cu_id) + ", '" + name + "', "
            + std::to_string(start_ea) + ", " + std::to_string(end_ea) + ", "
            "'" + name + "', 1, "
            "(select count(*) from functions where source_file = '" + name + "'))");

        const auto funcs = database.query_rows(
            "select address from functions where source_file = '" + name + "'");
        for (const auto& f : funcs) {
            if (f.empty()) continue;
            database.execute(
                "insert into compilation_unit_functions (cu_id, func_id, address) values ("
                + std::to_string(cu_id) + ", " + f[0] + ", " + f[0] + ")");
        }
        ++cu_id;
    }
}

ProgramSnapshot SnapshotRepository::load(const std::filesystem::path& path) const
{
    db::Database database;
    database.open(path);

    ProgramSnapshot snapshot;
    snapshot.architecture = database.query_text("select processor from program limit 1");
    snapshot.callgraph_primes = database.query_text("select coalesce(callgraph_primes,'') from program limit 1");
    snapshot.callgraph_all_primes = database.query_text("select coalesce(callgraph_all_primes,'') from program limit 1");
    snapshot.input_path = path.string();

    const auto data_rows = database.query_rows("select name, type, value from program_data order by id");
    snapshot.program_data.reserve(data_rows.size());
    for (const auto& row : data_rows) {
        snapshot.program_data.push_back({row[0], row[1], row[2]});
    }

    const auto rows = database.query_rows(
        "select id, address, name, coalesce(size, 0), coalesce(instructions, 0), "
        "coalesce(edges, 0), coalesce(nodes, 0), coalesce(rva, ''), coalesce(segment_rva, ''), "
        "coalesce(mnemonics, ''), coalesce(bytes_hash, ''), coalesce(function_hash, ''), "
        "coalesce(indegree, 0), coalesce(outdegree, 0), coalesce(names, ''), coalesce(assembly, ''), "
        "coalesce(clean_assembly, ''), coalesce(assembly_addrs, ''), coalesce(bytes_sum, 0), "
        "coalesce(function_flags, 0), coalesce(mangled_function, ''), coalesce(prototype, ''), "
        "coalesce(prototype2, ''), coalesce(comment, ''), coalesce(md_index, ''), "
        "coalesce(cyclomatic_complexity, 0), coalesce(primes_value, ''), "
        "coalesce(strongly_connected, 0), coalesce(loops, 0), "
        "coalesce(tarjan_topological_sort, ''), coalesce(strongly_connected_spp, ''), "
        "coalesce(mnemonics_spp, ''), coalesce(switches, ''), "
        "coalesce(pseudocode, ''), coalesce(clean_pseudo, ''), coalesce(pseudocode_lines, 0), "
        "coalesce(pseudocode_hash1, ''), coalesce(pseudocode_hash2, ''), coalesce(pseudocode_hash3, ''), "
        "coalesce(pseudocode_primes, ''), coalesce(microcode, ''), coalesce(clean_microcode, ''), "
        "coalesce(microcode_spp, ''), coalesce(kgh_hash, ''), coalesce(source_file, ''), "
        "coalesce(userdata, ''), coalesce(export_time, 0) "
        "from functions order by id");
    snapshot.functions.reserve(rows.size());
    for (const auto& row : rows) {
        const auto function_id = row[0];
        FunctionFeature function;
        function.address = parse_address(row[1]);
        function.name = row[2];
        function.size = static_cast<std::uint64_t>(std::stoull(row[3]));
        function.instruction_count = static_cast<std::uint64_t>(std::stoull(row[4]));
        function.edge_count = static_cast<std::uint64_t>(std::stoull(row[5]));
        function.node_count = static_cast<std::uint64_t>(std::stoull(row[6]));
        function.rva = row[7].empty() ? 0 : parse_address(row[7]);
        function.segment_rva = row[8].empty() ? 0 : parse_address(row[8]);
        function.mnemonics = row[9];
        function.bytes_hash = row[10];
        function.function_hash = row[11];
        function.indegree = static_cast<std::uint64_t>(std::stoull(row[12]));
        function.outdegree = static_cast<std::uint64_t>(std::stoull(row[13]));
        function.names = row[14];
        function.assembly = row[15];
        function.stripped_assembly = row[16];
        function.assembly_addrs = row[17];
        function.bytes_sum = static_cast<std::uint64_t>(std::stoull(row[18]));
        function.function_flags = static_cast<std::uint64_t>(std::stoull(row[19]));
        function.mangled_function = row[20];
        function.prototype = row[21];
        function.prototype2 = row[22];
        function.comment = row[23];
        function.md_index = row[24];
        function.cyclomatic_complexity = static_cast<std::uint64_t>(std::stoull(row[25]));
        function.primes_value = row[26];
        function.strongly_connected = static_cast<std::uint64_t>(std::stoull(row[27]));
        function.loops = static_cast<std::uint64_t>(std::stoull(row[28]));
        function.tarjan_topological_sort = row[29];
        function.strongly_connected_spp = row[30];
        function.mnemonics_spp = row[31];
        function.switches = row[32];
        function.pseudocode = row[33];
        function.stripped_pseudocode = row[34];
        function.pseudocode_lines = static_cast<std::uint64_t>(std::stoull(row[35]));
        function.pseudocode_hash1 = row[36];
        function.pseudocode_hash2 = row[37];
        function.pseudocode_hash3 = row[38];
        function.pseudocode_primes = row[39];
        function.microcode = row[40];
        function.stripped_microcode = row[41];
        function.microcode_spp = row[42];
        function.kgh_hash = row[43];
        function.source_file = row[44];
        function.userdata = row[45];
        function.export_time = std::stod(row[46]);
        function.instruction_details = load_instruction_rows(database, function_id, "");
        function.microcode_instruction_details = load_instruction_rows(database, function_id, "microcode");
        function.blocks = load_block_rows(database, function_id, "");
        function.microcode_blocks = load_block_rows(database, function_id, "microcode");
        function.call_references = load_call_rows(database, function_id);
        snapshot.functions.push_back(std::move(function));
    }

    return snapshot;
}

} // namespace soff
