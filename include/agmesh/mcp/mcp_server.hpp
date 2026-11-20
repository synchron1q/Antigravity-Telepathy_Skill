#pragma once

#include "agmesh/adapter/antigravity_adapter.hpp"
#include "agmesh/core/config.hpp"
#include "agmesh/core/hlc_crdt.hpp"
#include "agmesh/net/mesh_transport.hpp"
#include "agmesh/sanitizer/secret_sanitizer.hpp"
#include "agmesh/storage/sqlite_store.hpp"

#include <asio.hpp>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace agmesh::mcp {

struct McpToolCallResult {
    bool is_error{false};
    std::string content_text;
};

// Embedded Model Context Protocol (MCP) 2025-03-26 JSON-RPC 2.0 Server
// Supports stdio transport AND local Streamable HTTP / SSE (/mcp, /sse, /healthz, /metrics).
class McpServer {
public:
    McpServer(core::ConfigManager& config_mgr,
              storage::SqliteStore& store,
              core::HybridLogicalClock& hlc,
              sanitizer::SecretSanitizer& sanitizer,
              adapter::IAntigravityAdapter& adapter,
              net::MeshTransport* transport_or_null);
    ~McpServer();

    McpServer(const McpServer&) = delete;
    McpServer& operator=(const McpServer&) = delete;

    // Runs synchronous stdio JSON-RPC loop until EOF or stop signal.
    int run_stdio();

    // Starts non-blocking HTTP/SSE server on 127.0.0.1:<port>.
    bool start_http(std::string& error_out);
    void stop_http();
    [[nodiscard]] bool is_http_running() const noexcept;
    [[nodiscard]] std::uint16_t http_bound_port() const noexcept;

    // Directly evaluates a single JSON-RPC 2.0 request string (used by stdio, HTTP, and tests).
    [[nodiscard]] std::string handle_jsonrpc_message(const std::string& raw_json);

    // Exposes tool definitions JSON for schemas and `ag-mesh init`.
    [[nodiscard]] static std::string tools_list_json();

private:
    std::string handle_initialize(const std::string& id_json);
    std::string handle_tools_call(const std::string& id_json, const std::string& params_json);
    std::string handle_resources_list(const std::string& id_json);
    std::string handle_resources_read(const std::string& id_json, const std::string& params_json);

    McpToolCallResult tool_cluster_get_overview(const std::string& args_json);
    McpToolCallResult tool_cluster_list_sessions(const std::string& args_json);
    McpToolCallResult tool_cluster_search_context(const std::string& args_json);
    McpToolCallResult tool_cluster_read_session(const std::string& args_json);
    McpToolCallResult tool_cluster_publish_update(const std::string& args_json);
    McpToolCallResult tool_cluster_create_handoff(const std::string& args_json);
    McpToolCallResult tool_cluster_claim_handoff(const std::string& args_json);
    McpToolCallResult tool_cluster_manage_lock(const std::string& args_json);
    McpToolCallResult tool_cluster_start_pairing(const std::string& args_json);
    McpToolCallResult tool_cluster_join_with_token(const std::string& args_json);
    McpToolCallResult tool_cluster_pairing_status(const std::string& args_json);

    std::string render_prometheus_metrics() const;
    std::string render_healthz_json() const;

    void start_http_accept();

    core::ConfigManager& config_mgr_;
    storage::SqliteStore& store_;
    core::HybridLogicalClock& hlc_;
    sanitizer::SecretSanitizer& sanitizer_;
    adapter::IAntigravityAdapter& adapter_;
    net::MeshTransport* transport_;

    asio::io_context http_io_;
    std::unique_ptr<asio::ip::tcp::acceptor> http_acceptor_;
    std::thread http_thread_;
    std::atomic<bool> http_running_{false};
    std::atomic<std::uint16_t> http_port_{0};
};

} // namespace agmesh::mcp
