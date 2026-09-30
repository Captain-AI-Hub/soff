#pragma once

#include <optional>
#include <string>

namespace soff::cli {

/// Runs the stdio MCP server: reads newline-delimited JSON-RPC 2.0 messages
/// from stdin until EOF and writes one response line per request to stdout.
/// Diagnostics go to stderr only, keeping stdout protocol-clean. Returns a
/// process exit code (0 on clean EOF).
int run_mcp_server();

/// Handles a single JSON-RPC message. Returns the response line for requests,
/// nullopt for notifications and unanswerable input. Exposed for testing.
std::optional<std::string> handle_mcp_message(const std::string& message);

} // namespace soff::cli
