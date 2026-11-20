#pragma once

#include "agmesh/core/config.hpp"
#include "agmesh/core/hlc_crdt.hpp"
#include "agmesh/core/types.hpp"
#include "agmesh/net/crypto_engine.hpp"
#include "agmesh/net/network_discovery.hpp"
#include "agmesh/net/protocol_codec.hpp"
#include "agmesh/storage/sqlite_store.hpp"

#include <asio.hpp>
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace agmesh::net {

struct TransportMetrics {
    std::uint64_t frames_sent{0};
    std::uint64_t frames_received{0};
    std::uint64_t bytes_sent{0};
    std::uint64_t bytes_received{0};
    std::uint64_t auth_failures{0};
    std::uint64_t relay_packets_forwarded{0};
    std::int64_t last_replication_ms{0};
};

struct PairingWindowStatus {
    bool active{false};
    bool locked_out{false};
    std::string invite_token;
    std::string pin;
    std::string secret_hex;
    std::int64_t created_at_ms{0};
    std::int64_t expires_at_ms{0};
    int remaining_ttl_seconds{0};
    int max_uses{1};
    int current_uses{0};
    int failed_attempts{0};
    std::vector<EndpointCandidate> endpoints;
    std::vector<std::string> joined_node_ids;
};

struct JoinClusterResult {
    bool ok{false};
    std::string error;
    std::string host_node_id;
    std::string winning_route_ip;
    std::uint16_t winning_route_port{0};
    std::string winning_route_kind; // "vpn" | "lan" | "public"
    double rtt_ms{-1.0};
    std::size_t peers_configured{0};
};

class PeerSession;

class MeshTransport {
public:
    using EventCallback = std::function<void(const core::SyncEventNotification&)>;

    MeshTransport(core::ConfigManager& config_mgr,
                  storage::SqliteStore& store,
                  core::HybridLogicalClock& hlc,
                  NodeKeyPair local_keys);
    ~MeshTransport();

    MeshTransport(const MeshTransport&) = delete;
    MeshTransport& operator=(const MeshTransport&) = delete;

    bool start(std::string& error_out);
    void stop();
    [[nodiscard]] bool is_running() const noexcept;

    [[nodiscard]] std::uint16_t bound_port() const noexcept;
    void set_event_callback(EventCallback cb);
    void notify_config_reloaded();

    // Enqueues and broadcasts CRDT deltas to all connected peers (or outbox for offline ones).
    void broadcast_delta_batch(const std::vector<core::SessionRecord>& sessions,
                               const std::vector<core::MessageRecord>& messages,
                               const std::vector<core::SessionSummary>& summaries,
                               const std::vector<core::WorkspaceInfo>& workspaces,
                               const std::vector<core::HandoffPacket>& handoffs,
                               const std::vector<core::DistributedLock>& locks,
                               const std::vector<core::TombstoneRecord>& tombstones);

    void broadcast_local_hello();
    void trigger_sync_all();

    [[nodiscard]] std::vector<core::NodeInfo> connected_peers() const;
    [[nodiscard]] TransportMetrics metrics() const;

    // Directly verifies peer connectivity for `ag-mesh doctor`.
    bool probe_peer_tcp(const std::string& host, std::uint16_t port, double& rtt_ms_out, std::string& err);

    // Zero-Touch Pairing Window & Join Cluster API
    [[nodiscard]] PairingWindowStatus start_pairing_window(
        int ttl_seconds = 600,
        int max_uses = 1,
        std::uint16_t port_override = 0,
        const std::vector<EndpointCandidate>& explicit_endpoints = {});
    [[nodiscard]] PairingWindowStatus get_pairing_status() const;
    void stop_pairing_window();

    [[nodiscard]] JoinClusterResult join_cluster(const std::string& token_or_ip,
                                                 const std::string& pin = "",
                                                 int timeout_ms = 8000);

    // Broadcasts a signed PEER_GOSSIP (0x24) update so existing online peers mesh with a new joiner.
    void broadcast_peer_gossip(const core::PeerConfig& new_peer);

private:
    friend class PeerSession;

    void start_accept();
    void start_discovery_listener();
    void send_discovery_beacon();
    void schedule_maintenance_timer();
    void connect_to_peer(const core::PeerConfig& peer);

    void register_authenticated_session(const std::string& peer_node_id, std::shared_ptr<PeerSession> session);
    void unregister_session(const std::string& peer_node_id, PeerSession* session_ptr);
    void handle_decoded_frame(const std::string& peer_node_id,
                              const std::shared_ptr<PeerSession>& session,
                              const FrameHeader& header,
                              const std::vector<std::uint8_t>& payload);

    void flush_outbox_for_peer(const std::string& peer_node_id, const std::shared_ptr<PeerSession>& session);
    void apply_incoming_delta_json(const std::string& peer_node_id, const std::string& json_str);
    void emit_event(const core::SyncEventNotification& ev);

    bool validate_and_accept_pair_request(const std::string& joiner_node_id,
                                         std::uint16_t joiner_listen_port,
                                         const std::array<std::uint8_t, 32>& joiner_ed_pub,
                                         const std::array<std::uint8_t, 32>& joiner_eph_pub,
                                         const std::string& auth_mode,
                                         const std::vector<std::uint8_t>& transcript,
                                         const std::array<std::uint8_t, 32>& recv_hmac,
                                         const std::array<std::uint8_t, 64>& recv_sig,
                                         const std::string& remote_ip,
                                         std::string& welcome_envelope_json_out,
                                         std::string& reject_reason_out);

    struct PairingWindowInternal {
        bool active{false};
        bool locked_out{false};
        std::string secret_hex;
        std::string pin_digits;
        std::string pin_formatted;
        std::string invite_token;
        std::int64_t created_at_ms{0};
        std::int64_t expires_at_ms{0};
        int max_uses{1};
        int current_uses{0};
        int failed_attempts{0};
        std::vector<EndpointCandidate> endpoints;
        std::vector<std::string> joined_node_ids;
    };

    core::ConfigManager& config_mgr_;
    storage::SqliteStore& store_;
    core::HybridLogicalClock& hlc_;
    NodeKeyPair local_keys_;

    mutable std::mutex pairing_mutex_;
    PairingWindowInternal pairing_window_;

    asio::io_context io_ctx_;
    std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> work_guard_;
    std::unique_ptr<asio::ip::tcp::acceptor> acceptor_;
    std::unique_ptr<asio::ip::udp::socket> discovery_socket_;
    std::unique_ptr<asio::steady_timer> maintenance_timer_;
    asio::ip::udp::endpoint discovery_remote_ep_;
    std::array<std::uint8_t, 2048> discovery_recv_buf_{};

    std::vector<std::thread> io_threads_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint16_t> bound_port_{0};

    mutable std::mutex sessions_mutex_;
    std::unordered_map<std::string, std::shared_ptr<PeerSession>> active_sessions_;
    std::unordered_map<std::string, std::int64_t> reconnect_not_before_ms_;
    std::unordered_map<std::string, int> reconnect_attempts_;

    TokenBucketLimiter rate_limiter_;
    mutable std::mutex cb_mutex_;
    EventCallback event_callback_;

    std::atomic<std::uint64_t> frames_sent_{0};
    std::atomic<std::uint64_t> frames_received_{0};
    std::atomic<std::uint64_t> bytes_sent_{0};
    std::atomic<std::uint64_t> bytes_received_{0};
    std::atomic<std::uint64_t> auth_failures_{0};
    std::atomic<std::uint64_t> relay_forwarded_{0};
    std::atomic<std::int64_t> last_replication_ms_{0};
};

} // namespace agmesh::net
