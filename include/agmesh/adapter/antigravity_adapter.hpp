#pragma once

#include "agmesh/core/config.hpp"
#include "agmesh/core/types.hpp"

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace agmesh::adapter {

struct ParsedConversationBundle {
    core::SessionRecord session;
    std::vector<core::MessageRecord> messages;
    std::optional<core::SessionSummary> summary;
    std::vector<core::ArtifactRecord> artifacts;
    std::optional<core::WorkspaceInfo> workspace;
};

struct ParserProbeInfo {
    std::string parser_name;
    std::string detected_format;
    bool compatible{false};
    std::size_t discovered_items{0};
    std::string diagnostic;
};

struct ProbeReport {
    std::string os_name;
    std::filesystem::path app_data_dir;
    bool app_data_exists{false};
    bool app_data_readable{false};
    std::vector<std::filesystem::path> found_databases;
    std::vector<std::filesystem::path> found_transcripts;
    std::vector<std::filesystem::path> found_artifacts;
    std::vector<std::filesystem::path> found_state_files;
    std::vector<ParserProbeInfo> parsers;
    std::vector<core::WorkspaceInfo> active_workspaces;
    std::vector<std::string> warnings;
};

// Plugin parser interface for Antigravity storage backends (SQLite RO, JSONL, Protobuf, Markdown, MCP hook).
class IAntigravityParser {
public:
    virtual ~IAntigravityParser() = default;
    [[nodiscard]] virtual std::string name() const = 0;
    [[nodiscard]] virtual bool can_parse(const std::filesystem::path& app_data_dir) const = 0;
    [[nodiscard]] virtual ParserProbeInfo probe(const std::filesystem::path& app_data_dir) const = 0;
    virtual std::vector<ParsedConversationBundle> scan(
        const std::filesystem::path& app_data_dir,
        const std::string& local_node_id,
        const core::PrivacyConfig& privacy) const = 0;
};

// Abstract adapter isolating ag-mesh from Antigravity internal storage changes.
class IAntigravityAdapter {
public:
    virtual ~IAntigravityAdapter() = default;

    [[nodiscard]] virtual std::filesystem::path detect_app_data_dir() const = 0;
    [[nodiscard]] virtual ProbeReport run_probe() const = 0;
    [[nodiscard]] virtual std::vector<ParsedConversationBundle> collect_updates(
        const std::string& local_node_id,
        const core::PrivacyConfig& privacy) const = 0;
    [[nodiscard]] virtual std::vector<core::WorkspaceInfo> detect_active_workspaces(
        const std::string& local_node_id) const = 0;

    // Fallback ingestion channel fed directly via MCP hooks / explicit agent calls.
    virtual void ingest_mcp_hook_event(const ParsedConversationBundle& bundle) = 0;
};

class AntigravityAdapter final : public IAntigravityAdapter {
public:
    explicit AntigravityAdapter(core::AntigravityPathsConfig paths_config = {});

    [[nodiscard]] std::filesystem::path detect_app_data_dir() const override;
    [[nodiscard]] ProbeReport run_probe() const override;
    [[nodiscard]] std::vector<ParsedConversationBundle> collect_updates(
        const std::string& local_node_id,
        const core::PrivacyConfig& privacy) const override;
    [[nodiscard]] std::vector<core::WorkspaceInfo> detect_active_workspaces(
        const std::string& local_node_id) const override;
    void ingest_mcp_hook_event(const ParsedConversationBundle& bundle) override;

    // Registers an additional custom format parser in the plugin chain.
    void register_parser(std::shared_ptr<IAntigravityParser> parser);

    // Git repository inspection helper (resolves repo name, current branch, HEAD commit).
    static bool inspect_git_repo(const std::filesystem::path& workspace_path,
                                 std::string& repo_name_out,
                                 std::string& branch_out,
                                 std::string& commit_out);

private:
    core::AntigravityPathsConfig paths_config_;
    mutable std::mutex hook_mutex_;
    std::vector<ParsedConversationBundle> mcp_hook_buffer_;
    std::vector<std::shared_ptr<IAntigravityParser>> parsers_;
};

} // namespace agmesh::adapter
