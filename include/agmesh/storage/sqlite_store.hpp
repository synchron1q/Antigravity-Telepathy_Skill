#pragma once

#include "agmesh/core/types.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace agmesh::storage {

struct SessionFilter {
    std::string node_id;
    std::string repo_name;
    std::string status;
    std::string tag;
    std::int64_t since_ms{0};
    bool include_private{false};
    int limit{50};
    int offset{0};
};

struct RedactionLogEntry {
    std::int64_t id{0};
    std::string session_id;
    std::string secret_type;
    std::string redacted_marker;
    std::string source_path;
    std::int64_t detected_at_ms{0};
};

struct StoreStats {
    std::int64_t nodes_count{0};
    std::int64_t workspaces_count{0};
    std::int64_t sessions_count{0};
    std::int64_t messages_count{0};
    std::int64_t artifacts_count{0};
    std::int64_t handoffs_count{0};
    std::int64_t outbox_pending_count{0};
    std::int64_t redactions_count{0};
    std::int64_t db_file_size_bytes{0};
    int schema_version{0};
};

class SqliteStore {
public:
    explicit SqliteStore(std::filesystem::path db_path);
    ~SqliteStore();

    SqliteStore(const SqliteStore&) = delete;
    SqliteStore& operator=(const SqliteStore&) = delete;

    // Opens DB in WAL mode, runs schema & versioned migrations.
    bool open_and_migrate(std::string& error_out);
    void close();
    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] bool check_integrity(std::string& details_out);
    void flush_wal();

    // LWW-Element-Set upserts (conflict resolution deterministic by HLC)
    bool upsert_node(const core::NodeInfo& node);
    bool upsert_workspace(const core::WorkspaceInfo& ws);
    bool upsert_session(const core::SessionRecord& session);
    bool upsert_summary(const core::SessionSummary& summary);
    bool upsert_handoff(const core::HandoffPacket& handoff);
    bool upsert_artifact(const core::ArtifactRecord& artifact);
    bool upsert_lock(const core::DistributedLock& lock);

    // Append-Only Log with SHA-256 deduplication and tombstone suppression
    bool append_message(const core::MessageRecord& msg, bool& inserted_new);

    // Tombstone purge across local store (and propagates via sync)
    bool purge_entity(const core::TombstoneRecord& tombstone);
    [[nodiscard]] bool is_tombstoned(const std::string& target_type, const std::string& target_id);

    // Sync Cursors & Vector Clocks
    bool update_sync_cursor(const std::string& peer_node_id,
                            const core::HlcTimestamp& hlc,
                            const core::VectorClock& vc,
                            std::int64_t sync_ms);
    [[nodiscard]] core::VectorClock get_peer_vector_clock(const std::string& peer_node_id);

    // Persistent Outbox Queue
    bool enqueue_outbox(const core::OutboxEvent& ev);
    [[nodiscard]] std::vector<core::OutboxEvent> fetch_due_outbox(const std::string& target_node_id,
                                                                  std::int64_t now_ms_val,
                                                                  int limit = 100);
    bool mark_outbox_delivered(const std::string& event_id);
    bool mark_outbox_retry(const std::string& event_id, std::int64_t next_retry_ms);

    // Redaction Audit Log
    bool record_redaction(const std::string& session_id,
                          const std::string& secret_type,
                          const std::string& marker,
                          const std::string& source_path);
    [[nodiscard]] std::vector<RedactionLogEntry> list_redactions(int limit = 50);

    // L1 / L2 / L3 Context Queries + FTS5 Search
    [[nodiscard]] core::ClusterOverview get_cluster_overview(const std::string& local_node_id);
    [[nodiscard]] std::vector<core::SessionRecord> list_sessions(const SessionFilter& filter);
    [[nodiscard]] std::optional<core::SessionRecord> get_session(const std::string& session_id);
    [[nodiscard]] std::optional<core::SessionSummary> get_summary(const std::string& session_id);
    [[nodiscard]] std::vector<core::MessageRecord> read_messages(const std::string& session_id,
                                                                 int offset,
                                                                 int limit,
                                                                 std::size_t max_chars);
    [[nodiscard]] std::vector<core::ArtifactRecord> list_artifacts(const std::string& session_id);
    [[nodiscard]] std::optional<core::HandoffPacket> get_handoff(const std::string& handoff_id);
    [[nodiscard]] std::vector<core::HandoffPacket> list_handoffs(const std::string& status_filter = "");
    [[nodiscard]] std::vector<core::DistributedLock> list_active_locks(std::int64_t now_ms_val);

    [[nodiscard]] std::vector<core::SearchHit> search_fts(const std::string& query,
                                                          const std::string& node_filter = "",
                                                          const std::string& repo_filter = "",
                                                          int limit = 20);

    // Delta extraction for peer synchronization
    [[nodiscard]] std::vector<core::SessionRecord> get_sessions_since(std::uint64_t min_hlc_ms, int limit = 200);
    [[nodiscard]] std::vector<core::MessageRecord> get_messages_since(std::uint64_t min_hlc_ms, int limit = 500);
    [[nodiscard]] std::vector<core::SessionSummary> get_summaries_since(std::uint64_t min_hlc_ms, int limit = 200);
    [[nodiscard]] std::vector<core::HandoffPacket> get_handoffs_since(std::uint64_t min_hlc_ms, int limit = 100);
    [[nodiscard]] std::vector<core::DistributedLock> get_locks_since(std::uint64_t min_hlc_ms, int limit = 100);
    [[nodiscard]] std::vector<core::WorkspaceInfo> get_workspaces_since(std::uint64_t min_hlc_ms, int limit = 200);
    [[nodiscard]] std::vector<core::TombstoneRecord> get_tombstones_since(std::uint64_t min_hlc_ms, int limit = 200);

    // Data retention TTL rotation (removes old full message texts while preserving summaries)
    std::size_t rotate_expired_messages(int ttl_days, std::int64_t now_ms_val);

    // Atomic export to static Markdown or JSON snapshot
    bool export_snapshot_json(const std::filesystem::path& dest_path, const std::string& local_node_id);
    bool export_snapshot_markdown(const std::filesystem::path& dest_path, const std::string& local_node_id);

    [[nodiscard]] StoreStats get_stats();
    [[nodiscard]] const std::filesystem::path& db_path() const noexcept { return db_path_; }

private:
    bool exec_sql(const std::string& sql, std::string& error_out);
    void reindex_fts_entity(const std::string& entity_type,
                            const std::string& entity_id,
                            const std::string& session_id,
                            const std::string& node_id,
                            const std::string& repo_name,
                            const std::string& title,
                            const std::string& body);

    std::filesystem::path db_path_;
    mutable std::mutex mutex_;
    sqlite3* db_{nullptr};
};

} // namespace agmesh::storage
