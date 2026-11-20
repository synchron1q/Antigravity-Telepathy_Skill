#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace agmesh::core {

enum class TopologyMode {
    P2PMesh,
    HubAndSpoke
};

enum class NodeRole {
    Peer,
    RelayHub
};

enum class OnlineStatus {
    Online,
    Degraded,
    Offline
};

struct HlcTimestamp {
    std::uint64_t physical_ms{0};
    std::uint32_t logical{0};
    std::string origin_node_id;

    bool operator==(const HlcTimestamp& other) const noexcept = default;

    [[nodiscard]] int compare(const HlcTimestamp& other) const noexcept {
        if (physical_ms != other.physical_ms) {
            return physical_ms < other.physical_ms ? -1 : 1;
        }
        if (logical != other.logical) {
            return logical < other.logical ? -1 : 1;
        }
        if (origin_node_id != other.origin_node_id) {
            return origin_node_id < other.origin_node_id ? -1 : 1;
        }
        return 0;
    }

    bool operator<(const HlcTimestamp& other) const noexcept { return compare(other) < 0; }
    bool operator>(const HlcTimestamp& other) const noexcept { return compare(other) > 0; }
    bool operator<=(const HlcTimestamp& other) const noexcept { return compare(other) <= 0; }
    bool operator>=(const HlcTimestamp& other) const noexcept { return compare(other) >= 0; }
};

using VectorClock = std::map<std::string, std::uint64_t>;

struct NodeInfo {
    std::string node_id;
    std::string hostname;
    std::string os_name;
    std::string arch;
    std::vector<std::string> tags;
    std::vector<std::string> projects;
    std::string pubkey_fingerprint;
    std::string endpoint;
    OnlineStatus online_status{OnlineStatus::Offline};
    double rtt_ms{-1.0};
    std::int64_t last_seen_ms{0};
    HlcTimestamp hlc;
};

struct WorkspaceInfo {
    std::string workspace_id;
    std::string node_id;
    std::string root_path;
    std::string repo_name;
    std::string git_branch;
    std::string git_commit;
    bool is_open{false};
    std::string active_task;
    std::int64_t last_active_ms{0};
    HlcTimestamp hlc;
};

struct SessionRecord {
    std::string session_id;
    std::string node_id;
    std::string workspace_path;
    std::string repo_name;
    std::string git_branch;
    std::string title;
    std::string status{"active"}; // active | idle | completed | archived
    bool is_private{false};
    std::string source_adapter{"auto"};
    std::vector<std::string> tags;
    std::int64_t created_at_ms{0};
    std::int64_t updated_at_ms{0};
    HlcTimestamp hlc;
};

struct MessageRecord {
    std::string message_id;
    std::string session_id;
    std::string node_id;
    std::int64_t seq_index{0};
    std::string role{"user"}; // user | assistant | system | tool
    std::string content;
    std::vector<std::string> touched_files;
    std::string content_sha256;
    std::int64_t created_at_ms{0};
    HlcTimestamp hlc;
};

struct SessionSummary {
    std::string session_id;
    std::string node_id;
    std::string current_goal;
    std::vector<std::string> decisions;
    std::vector<std::string> modified_files;
    std::vector<std::string> bugs_found;
    std::vector<std::string> open_questions;
    std::string summary_text;
    std::int64_t updated_at_ms{0};
    HlcTimestamp hlc;
};

struct HandoffPacket {
    std::string handoff_id;
    std::string from_node_id;
    std::string target_node_id;
    std::string session_id;
    std::string claimed_by_node;
    std::string claimed_session_id;
    std::string repo_name;
    std::string workspace_path;
    std::string git_branch;
    std::string git_commit;
    std::string task_description;
    std::string uncommitted_diff;
    std::vector<std::string> blockers;
    std::vector<std::string> next_steps;
    std::vector<std::string> artifact_refs;
    std::string status{"pending"}; // pending | claimed | completed | cancelled
    std::int64_t created_at_ms{0};
    std::int64_t updated_at_ms{0};
    HlcTimestamp hlc;
};

struct ArtifactRecord {
    std::string artifact_id;
    std::string session_id;
    std::string node_id;
    std::string relative_path;
    std::string mime_type{"text/markdown"};
    std::string content_text;
    std::int64_t size_bytes{0};
    std::string content_sha256;
    std::int64_t updated_at_ms{0};
    HlcTimestamp hlc;
};

struct DistributedLock {
    std::string lock_key;
    std::string owner_node_id;
    std::string owner_session_id;
    std::string reason;
    std::int64_t acquired_at_ms{0};
    std::int64_t expires_at_ms{0};
    bool released{false};
    HlcTimestamp hlc;
};

struct TombstoneRecord {
    std::string target_type; // session | artifact | handoff
    std::string target_id;
    std::int64_t purged_at_ms{0};
    HlcTimestamp hlc;
};

struct OutboxEvent {
    std::string event_id;
    std::string target_node_id; // "*" for broadcast
    std::uint8_t frame_type{0};
    std::string payload_json;
    std::int64_t created_at_ms{0};
    int attempts{0};
    std::int64_t next_retry_ms{0};
};

struct SearchHit {
    std::string entity_type;
    std::string entity_id;
    std::string session_id;
    std::string node_id;
    std::string repo_name;
    std::string title;
    std::string snippet;
    double score{0.0};
    std::int64_t timestamp_ms{0};
};

struct ClusterOverview {
    std::string local_node_id;
    std::int64_t generated_at_ms{0};
    std::vector<NodeInfo> nodes;
    std::vector<WorkspaceInfo> active_workspaces;
    std::vector<SessionRecord> recent_sessions;
    std::vector<SessionSummary> recent_summaries;
    std::vector<HandoffPacket> pending_handoffs;
    std::vector<DistributedLock> active_locks;
};

struct SyncEventNotification {
    std::string event_type;
    std::string node_id;
    std::string session_id;
    std::string summary;
    std::int64_t timestamp_ms{0};
};

[[nodiscard]] inline std::int64_t now_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] inline std::uint64_t now_us() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

[[nodiscard]] inline std::string to_string(OnlineStatus st) {
    switch (st) {
        case OnlineStatus::Online: return "online";
        case OnlineStatus::Degraded: return "degraded";
        case OnlineStatus::Offline: return "offline";
    }
    return "offline";
}

[[nodiscard]] inline OnlineStatus online_status_from_string(const std::string& s) {
    if (s == "online") return OnlineStatus::Online;
    if (s == "degraded") return OnlineStatus::Degraded;
    return OnlineStatus::Offline;
}

} // namespace agmesh::core
