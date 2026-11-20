#pragma once

#include "agmesh/core/types.hpp"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace agmesh::core {

struct PeerConfig {
    std::string node_id;
    std::string address;
    std::uint16_t port{47890};
    std::string pubkey_fingerprint;
    std::string ed25519_pubkey_hex;
    bool is_seed{false};
    bool is_relay{false};
};

struct PrivacyConfig {
    bool metadata_only{false};
    std::vector<std::string> allow_globs;
    std::vector<std::string> deny_globs;
    bool sanitize_secrets{true};
    double entropy_threshold{4.5};
    std::size_t entropy_min_length{20};
    int full_text_ttl_days{30};
};

struct NetworkConfig {
    TopologyMode topology{TopologyMode::P2PMesh};
    NodeRole role{NodeRole::Peer};
    std::string listen_address{"0.0.0.0"};
    std::uint16_t listen_port{47890};
    std::string relay_endpoint;
    bool enable_mdns_discovery{true};
    std::string multicast_group{"239.255.77.89"};
    std::uint16_t discovery_port{47891};
    std::string cluster_psk_hex;
    std::uint32_t heartbeat_interval_ms{3000};
    std::uint32_t peer_timeout_ms{12000};
    std::size_t zstd_threshold_bytes{512};
    int zstd_level{3};
    std::uint64_t bandwidth_limit_kbps{0}; // 0 = unlimited
    std::size_t max_attachment_bytes{8 * 1024 * 1024};
    std::size_t io_threads{4};
};

struct McpHttpConfig {
    bool enable_http{true};
    std::string bind_address{"127.0.0.1"};
    std::uint16_t port{47895};
    bool enable_metrics{true};
};

struct AntigravityPathsConfig {
    std::string app_data_dir; // Empty = auto-detect OS default
    std::vector<std::string> extra_workspace_roots;
    std::uint32_t watcher_debounce_ms{350};
    std::uint32_t poll_interval_ms{5000};
};

struct LoggingConfig {
    std::string level{"info"};
    bool json_format{false};
    std::string log_file;
    std::size_t max_file_bytes{10 * 1024 * 1024};
    std::size_t max_files{5};
};

struct MeshConfig {
    std::string node_id;
    std::string hostname;
    std::vector<std::string> tags;
    std::string data_dir;
    std::string db_path;
    std::string key_file;
    std::string ipc_socket_path;

    NetworkConfig network;
    PrivacyConfig privacy;
    McpHttpConfig mcp;
    AntigravityPathsConfig antigravity;
    LoggingConfig logging;
    std::vector<PeerConfig> peers;
};

struct ConfigValidationResult {
    bool ok{true};
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

class ConfigManager {
public:
    static std::filesystem::path default_config_dir();
    static std::filesystem::path default_config_path();
    static std::filesystem::path default_data_dir();
    static std::string detect_os_name();
    static std::string detect_arch();
    static std::string detect_hostname();

    static MeshConfig create_default_config(const std::string& node_id = "");
    static MeshConfig load_from_file(const std::filesystem::path& path);
    static void save_to_file(const MeshConfig& config, const std::filesystem::path& path);
    static ConfigValidationResult validate(const MeshConfig& config);

    // Evaluates allow_globs and deny_globs against a workspace or file path.
    [[nodiscard]] static bool is_path_allowed(const PrivacyConfig& privacy, const std::string& path);
    [[nodiscard]] static bool glob_match(const std::string& pattern, const std::string& text);

    explicit ConfigManager(std::filesystem::path config_path);

    [[nodiscard]] MeshConfig snapshot() const;
    bool reload(std::string& error_out);
    bool add_or_update_peer(const PeerConfig& peer, std::string& error_out);
    bool remove_peer(const std::string& node_id, std::string& error_out);
    bool apply_joined_cluster_config(const std::string& psk_hex,
                                     const std::vector<PeerConfig>& peers,
                                     std::string& error_out);
    [[nodiscard]] const std::filesystem::path& config_path() const noexcept { return config_path_; }

private:
    std::filesystem::path config_path_;
    mutable std::mutex mutex_;
    MeshConfig current_;
};

} // namespace agmesh::core
