#include "soff/cli/mcp_server.hpp"
#include "soff/analysis/model.hpp"
#include "soff/db/repository.hpp"
#include "soff/db/result_repository.hpp"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <utility>

namespace {

soff::FunctionFeature mcp_function(
    soff::Address address,
    const char* name,
    const char* assembly,
    const char* pseudocode,
    const char* bytes_hash)
{
    soff::FunctionFeature function;
    function.address = address;
    function.rva = address - 0x400000;
    function.segment_rva = 0x1000;
    function.name = name;
    function.size = 32;
    function.instruction_count = 4;
    function.node_count = 3;
    function.edge_count = 2;
    function.indegree = 1;
    function.outdegree = 1;
    function.cyclomatic_complexity = 1;
    function.strongly_connected = 1;
    function.loops = 0;
    function.mnemonics = "push\nmov\npop\nret";
    function.names = name;
    function.assembly = assembly;
    function.stripped_assembly = assembly;
    function.assembly_addrs = "[\"4198400\",\"4198401\"]";
    function.bytes_hash = bytes_hash;
    function.function_hash = "function-hash";
    function.md_index = "1.25";
    function.primes_value = "3";
    function.tarjan_topological_sort = "[[0]]";
    function.strongly_connected_spp = "1";
    function.mnemonics_spp = "15";
    function.switches = "[]";
    function.bytes_sum = 0x1234;
    function.function_flags = 0x10;
    function.mangled_function = name;
    function.prototype = "int start(void)";
    function.prototype2 = "int start(void)";
    function.comment = "";
    function.pseudocode = pseudocode;
    function.stripped_pseudocode = pseudocode;
    function.pseudocode_lines = 4;
    function.pseudocode_hash1 = "pseudo-hash-1";
    function.pseudocode_hash2 = "pseudo-hash-2";
    function.pseudocode_hash3 = "pseudo-hash-3";
    return function;
}

soff::ProgramSnapshot mcp_snapshot(soff::FunctionFeature function)
{
    soff::ProgramSnapshot snapshot;
    snapshot.input_path = "sample.exe";
    snapshot.architecture = "metapc";
    snapshot.program_data.push_back({"export.total_functions", "integer", "1"});
    snapshot.program_data.push_back({"export.exported_functions", "integer", "1"});
    snapshot.program_data.push_back({"export.skipped_functions", "integer", "0"});
    snapshot.functions.push_back(std::move(function));
    return snapshot;
}

bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

// Paths embedded in request JSON (and expected inside response JSON) must be
// escaped: Windows paths contain backslashes, which are invalid raw in JSON.
std::string json_escape(const std::string& value)
{
    std::string out;
    out.reserve(value.size());
    for (const char ch : value) {
        if (ch == '"' || ch == '\\') {
            out.push_back('\\');
        }
        out.push_back(ch);
    }
    return out;
}

std::string call_tool(const std::string& result_path, const std::string& tool, const std::string& extra_args)
{
    const auto message = "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\",\"params\":{\"name\":\""
        + tool + "\",\"arguments\":{\"result_path\":\"" + json_escape(result_path) + "\"" + extra_args + "}}}";
    const auto response = soff::cli::handle_mcp_message(message);
    assert(response.has_value());
    return *response;
}

} // namespace

void test_mcp_server()
{
    const auto build_dir = std::filesystem::absolute(std::filesystem::path("build"));
    std::filesystem::create_directories(build_dir);
    const auto main_db = build_dir / "soff_mcp_primary.sqlite";
    const auto diff_db = build_dir / "soff_mcp_secondary.sqlite";
    const auto result_path = build_dir / "soff_mcp_server.soff";
    std::error_code ec;
    std::filesystem::remove(main_db, ec);
    std::filesystem::remove(diff_db, ec);
    std::filesystem::remove(result_path, ec);

    auto primary = mcp_function(
        0x401000,
        "start",
        "push rbp\nmov rbp, rsp\npop rbp\nret",
        "int start(void)\n{\n  return 0;\n}",
        "primary-bytes");
    auto secondary = mcp_function(
        0x401000,
        "start",
        "push rbp\npop rbp\nret",
        "int start(void)\n{\n  return 1;\n}",
        "secondary-bytes");

    soff::SnapshotRepository repository;
    repository.save(mcp_snapshot(std::move(primary)), main_db);
    repository.save(mcp_snapshot(std::move(secondary)), diff_db);

    // Stored as bare filenames: the tools must resolve them against the
    // directory that holds the .soff result database.
    soff::db::DiffResultSet results;
    results.main_db = main_db.filename().string();
    results.diff_db = diff_db.filename().string();
    {
        soff::db::ResultMatch match;
        match.kind = soff::db::ResultKind::best;
        match.line = 1;
        match.primary = 0x401000;
        match.primary_name = "start";
        match.secondary = 0x401000;
        match.secondary_name = "start";
        match.ratio = 0.9;
        match.primary_nodes = 3;
        match.secondary_nodes = 2;
        match.description = "Best matched by pseudo hash";
        results.matches.push_back(match);
    }
    {
        soff::db::UnmatchedFunction gone;
        gone.kind = soff::db::UnmatchedKind::primary;
        gone.line = 2;
        gone.address = 0x402000;
        gone.name = "gone_primary";
        results.unmatched.push_back(gone);
    }
    {
        soff::db::UnmatchedFunction added;
        added.kind = soff::db::UnmatchedKind::secondary;
        added.line = 3;
        added.address = 0x403000;
        added.name = "added_secondary";
        results.unmatched.push_back(added);
    }
    assert(soff::db::ResultRepository{}.save(results, result_path));

    // initialize: echoes the requested protocol version and reports server info.
    {
        const auto response = soff::cli::handle_mcp_message(
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":"
            "{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},\"clientInfo\":{\"name\":\"test\"}}}");
        assert(response.has_value());
        assert(contains(*response, "\"protocolVersion\":\"2025-06-18\""));
        assert(contains(*response, "\"soff-mcp\""));
        std::cout << "mcp: initialize passed\n";
    }

    // notifications are never answered.
    {
        const auto response = soff::cli::handle_mcp_message(
            "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
        assert(!response.has_value());
        std::cout << "mcp: notification ignored\n";
    }

    // ping echoes the id and answers with an empty result.
    {
        const auto response = soff::cli::handle_mcp_message(
            "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"ping\"}");
        assert(response.has_value());
        assert(contains(*response, "\"id\":2"));
        assert(contains(*response, "\"result\":{}"));
        std::cout << "mcp: ping passed\n";
    }

    // tools/list advertises all four read-only tools with schemas.
    {
        const auto response = soff::cli::handle_mcp_message(
            "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/list\"}");
        assert(response.has_value());
        for (const auto* tool : {"soff_diff_results", "soff_diff_unmatched", "soff_diff_asm", "soff_diff_pseudo"}) {
            assert(contains(*response, std::string("\"") + tool + "\""));
        }
        assert(contains(*response, "\"inputSchema\""));
        std::cout << "mcp: tools/list passed\n";
    }

    // soff_diff_results returns the match row and resolved db paths.
    {
        const auto response = call_tool(result_path.string(), "soff_diff_results", ",\"match_type\":\"best\"");
        assert(contains(response, "\"total\":1"));
        assert(contains(response, "\"primary_name\":\"start\""));
        assert(contains(response, "\"ratio\":0.9"));
        assert(contains(response, "\"nodes1\":3"));
        assert(contains(response, json_escape(main_db.string())));
        assert(!contains(response, "\"isError\":true"));
        std::cout << "mcp: soff_diff_results passed\n";
    }

    // limit 0 returns an empty item list but keeps the total.
    {
        const auto response = call_tool(result_path.string(), "soff_diff_results", ",\"limit\":0");
        assert(contains(response, "\"total\":1"));
        assert(contains(response, "\"items\":[]"));
        std::cout << "mcp: soff_diff_results limit 0 passed\n";
    }

    // soff_diff_unmatched filters by side.
    {
        const auto all = call_tool(result_path.string(), "soff_diff_unmatched", "");
        assert(contains(all, "\"total\":2"));
        assert(contains(all, "\"side\":\"primary\""));
        assert(contains(all, "gone_primary"));
        const auto secondary_only = call_tool(
            result_path.string(), "soff_diff_unmatched", ",\"side\":\"secondary\"");
        assert(contains(secondary_only, "\"total\":1"));
        assert(contains(secondary_only, "added_secondary"));
        assert(!contains(secondary_only, "gone_primary"));
        std::cout << "mcp: soff_diff_unmatched passed\n";
    }

    // soff_diff_asm normalizes 0x/trailing-h addresses and unifies the diff.
    {
        const auto response = call_tool(
            result_path.string(), "soff_diff_asm",
            ",\"primary_addr\":\"0x401000\",\"secondary_addr\":\"401000h\"");
        assert(contains(response, "\"primary_addr\":\"4198400\""));
        assert(contains(response, "-mov rbp, rsp"));
        assert(contains(response, " push rbp"));
        assert(!contains(response, "\"isError\":true"));
        std::cout << "mcp: soff_diff_asm passed\n";
    }

    // soff_diff_pseudo marks changed return statements.
    {
        const auto response = call_tool(
            result_path.string(), "soff_diff_pseudo",
            ",\"primary_addr\":\"4198400\",\"secondary_addr\":\"4198400\"");
        assert(contains(response, "-  return 0;"));
        assert(contains(response, "+  return 1;"));
        std::cout << "mcp: soff_diff_pseudo passed\n";
    }

    // Missing database: tool failure is reported in-band with isError.
    {
        const auto response = call_tool("/nonexistent/does-not-exist.soff", "soff_diff_results", "");
        assert(contains(response, "\"isError\":true"));
        assert(contains(response, "does not exist"));
        std::cout << "mcp: missing result database reported\n";
    }

    // Unknown tool and unknown method map to JSON-RPC errors.
    {
        const auto response = soff::cli::handle_mcp_message(
            "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"tools/call\",\"params\":"
            "{\"name\":\"does_not_exist\",\"arguments\":{}}}");
        assert(response.has_value());
        assert(contains(*response, "-32602"));
        const auto method = soff::cli::handle_mcp_message(
            "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"resources/read\",\"params\":{}}");
        assert(method.has_value());
        assert(contains(*method, "-32601"));
        std::cout << "mcp: unknown tool/method errors passed\n";
    }

    // Malformed JSON gets a parse error with a null id.
    {
        const auto response = soff::cli::handle_mcp_message("{\"jsonrpc\":");
        assert(response.has_value());
        assert(contains(*response, "\"id\":null"));
        assert(contains(*response, "-32700"));
        std::cout << "mcp: parse error passed\n";
    }

    std::cout << "mcp: all server tests passed\n";
}
