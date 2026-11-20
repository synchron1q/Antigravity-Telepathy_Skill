#include "agmesh/core/config.hpp"

#include <nlohmann/json.hpp>
#include <toml++/toml.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <unistd.h>
#include <sys/utsname.h>
#endif

namespace agmesh::core {

namespace {

std::string get_env_str(const char* name) {
#ifdef _WIN32
    char* buf = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&buf, &len, name) == 0 && buf != nullptr) {
        std::string val(buf);
        std::free(buf);
        return val;
    }
    return {};
#else
    const char* val = std::getenv(name);
    return val ? std::string(val) : std::string{};
#endif
}

std::string sanitize_node_id(std::string raw) {
    for (char& ch : raw) {
        const auto uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch) || ch == '-' || ch == '_') {
            ch = static_cast<char>(std::tolower(uch));
        } else {
            ch = '-';
        }
    }
    if (raw.empty()) {
        raw = "node-local";
    }
    return raw;
}

} // namespace

std::filesystem::path ConfigManager::default_config_dir() {
#ifdef _WIN32
    const auto appdata = get_env_str("APPDATA");
    if (!appdata.empty()) {
        return std::filesystem::path(appdata) / "ag-mesh";
    }
    const auto userprofile = get_env_str("USERPROFILE");
    return std::filesystem::path(userprofile.empty() ? "." : userprofile) / ".ag-mesh";
#elif defined(__APPLE__)
    const auto home = get_env_str("HOME");
    return std::filesystem::path(home.empty() ? "." : home) / "Library" / "Application Support" / "ag-mesh";
#else
    const auto xdg = get_env_str("XDG_CONFIG_HOME");
    if (!xdg.empty()) {
        return std::filesystem::path(xdg) / "ag-mesh";
    }
    const auto home = get_env_str("HOME");
    return std::filesystem::path(home.empty() ? "." : home) / ".config" / "ag-mesh";
#endif
}

std::filesystem::path ConfigManager::default_config_path() {
    return default_config_dir() / "config.toml";
}

std::filesystem::path ConfigManager::default_data_dir() {
#ifdef _WIN32
    const auto localappdata = get_env_str("LOCALAPPDATA");
    if (!localappdata.empty()) {
        return std::filesystem::path(localappdata) / "ag-mesh";
    }
    return default_config_dir();
#elif defined(__APPLE__)
    return default_config_dir();
#else
    const auto xdg_data = get_env_str("XDG_DATA_HOME");
    if (!xdg_data.empty()) {
        return std::filesystem::path(xdg_data) / "ag-mesh";
    }
    const auto home = get_env_str("HOME");
    return std::filesystem::path(home.empty() ? "." : home) / ".local" / "share" / "ag-mesh";
#endif
}

std::string ConfigManager::detect_os_name() {
#ifdef _WIN32
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "unix";
#endif
}

std::string ConfigManager::detect_arch() {
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__arm__) || defined(_M_ARM)
    return "arm";
#else
    return "unknown";
#endif
}

std::string ConfigManager::detect_hostname() {
#ifdef _WIN32
    char buf[256]{};
    DWORD size = sizeof(buf);
    if (GetComputerNameA(buf, &size) != 0 && size > 0) {
        return std::string(buf, size);
    }
    return "windows-host";
#else
    char buf[256]{};
    if (::gethostname(buf, sizeof(buf) - 1) == 0) {
        buf[sizeof(buf) - 1] = '\0';
        if (buf[0] != '\0') {
            return std::string(buf);
        }
    }
    return "unix-host";
#endif
}

MeshConfig ConfigManager::create_default_config(const std::string& node_id) {
    MeshConfig cfg;
    cfg.hostname = detect_hostname();
    cfg.node_id = node_id.empty() ? sanitize_node_id(cfg.hostname) : sanitize_node_id(node_id);
    cfg.tags = {detect_os_name(), detect_arch()};

    const auto data_dir = default_data_dir();
    cfg.data_dir = data_dir.string();
    cfg.db_path = (data_dir / "ag-mesh.db").string();
    cfg.key_file = (default_config_dir() / "node.key").string();
#ifdef _WIN32
    cfg.ipc_socket_path = R"(\\.\pipe\ag-mesh-)" + cfg.node_id;
#else
    cfg.ipc_socket_path = (data_dir / "ag-mesh.sock").string();
#endif
    cfg.logging.log_file = (data_dir / "logs" / "ag-mesh.log").string();
    return cfg;
}

MeshConfig ConfigManager::load_from_file(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        throw std::runtime_error("Configuration file does not exist: " + path.string());
    }

    const toml::table tbl = toml::parse_file(path.string());
    MeshConfig cfg = create_default_config();

    if (const auto* node = tbl["node"].as_table()) {
        if (auto v = (*node)["node_id"].value<std::string>()) cfg.node_id = *v;
        if (auto v = (*node)["hostname"].value<std::string>()) cfg.hostname = *v;
        if (auto v = (*node)["data_dir"].value<std::string>()) cfg.data_dir = *v;
        if (auto v = (*node)["db_path"].value<std::string>()) cfg.db_path = *v;
        if (auto v = (*node)["key_file"].value<std::string>()) cfg.key_file = *v;
        if (auto v = (*node)["ipc_socket_path"].value<std::string>()) cfg.ipc_socket_path = *v;
        if (const auto* tags = (*node)["tags"].as_array()) {
            cfg.tags.clear();
            for (const auto& elem : *tags) {
                if (auto s = elem.value<std::string>()) cfg.tags.push_back(*s);
            }
        }
    }

    if (const auto* net = tbl["network"].as_table()) {
        if (auto v = (*net)["topology"].value<std::string>()) {
            cfg.network.topology = (*v == "hub-and-spoke" || *v == "hub_and_spoke")
                                       ? TopologyMode::HubAndSpoke
                                       : TopologyMode::P2PMesh;
        }
        if (auto v = (*net)["role"].value<std::string>()) {
            cfg.network.role = (*v == "relay" || *v == "relay-hub" || *v == "hub")
                                   ? NodeRole::RelayHub
                                   : NodeRole::Peer;
        }
        if (auto v = (*net)["listen_address"].value<std::string>()) cfg.network.listen_address = *v;
        if (auto v = (*net)["listen_port"].value<std::int64_t>()) {
            cfg.network.listen_port = static_cast<std::uint16_t>(*v);
        }
        if (auto v = (*net)["relay_endpoint"].value<std::string>()) cfg.network.relay_endpoint = *v;
        if (auto v = (*net)["enable_mdns_discovery"].value<bool>()) cfg.network.enable_mdns_discovery = *v;
        if (auto v = (*net)["multicast_group"].value<std::string>()) cfg.network.multicast_group = *v;
        if (auto v = (*net)["discovery_port"].value<std::int64_t>()) {
            cfg.network.discovery_port = static_cast<std::uint16_t>(*v);
        }
        if (auto v = (*net)["cluster_psk_hex"].value<std::string>()) cfg.network.cluster_psk_hex = *v;
        if (auto v = (*net)["heartbeat_interval_ms"].value<std::int64_t>()) {
            cfg.network.heartbeat_interval_ms = static_cast<std::uint32_t>(*v);
        }
        if (auto v = (*net)["peer_timeout_ms"].value<std::int64_t>()) {
            cfg.network.peer_timeout_ms = static_cast<std::uint32_t>(*v);
        }
        if (auto v = (*net)["zstd_threshold_bytes"].value<std::int64_t>()) {
            cfg.network.zstd_threshold_bytes = static_cast<std::size_t>(*v);
        }
        if (auto v = (*net)["zstd_level"].value<std::int64_t>()) {
            cfg.network.zstd_level = static_cast<int>(*v);
        }
        if (auto v = (*net)["bandwidth_limit_kbps"].value<std::int64_t>()) {
            cfg.network.bandwidth_limit_kbps = static_cast<std::uint64_t>(*v);
        }
        if (auto v = (*net)["max_attachment_bytes"].value<std::int64_t>()) {
            cfg.network.max_attachment_bytes = static_cast<std::size_t>(*v);
        }
        if (auto v = (*net)["io_threads"].value<std::int64_t>()) {
            cfg.network.io_threads = static_cast<std::size_t>(std::max<std::int64_t>(1, *v));
        }
    }

    if (const auto* priv = tbl["privacy"].as_table()) {
        if (auto v = (*priv)["metadata_only"].value<bool>()) cfg.privacy.metadata_only = *v;
        if (auto v = (*priv)["sanitize_secrets"].value<bool>()) cfg.privacy.sanitize_secrets = *v;
        if (auto v = (*priv)["entropy_threshold"].value<double>()) cfg.privacy.entropy_threshold = *v;
        if (auto v = (*priv)["entropy_min_length"].value<std::int64_t>()) {
            cfg.privacy.entropy_min_length = static_cast<std::size_t>(*v);
        }
        if (auto v = (*priv)["full_text_ttl_days"].value<std::int64_t>()) {
            cfg.privacy.full_text_ttl_days = static_cast<int>(*v);
        }
        if (const auto* allow = (*priv)["allow_globs"].as_array()) {
            cfg.privacy.allow_globs.clear();
            for (const auto& elem : *allow) {
                if (auto s = elem.value<std::string>()) cfg.privacy.allow_globs.push_back(*s);
            }
        }
        if (const auto* deny = (*priv)["deny_globs"].as_array()) {
            cfg.privacy.deny_globs.clear();
            for (const auto& elem : *deny) {
                if (auto s = elem.value<std::string>()) cfg.privacy.deny_globs.push_back(*s);
            }
        }
    }

    if (const auto* mcp = tbl["mcp"].as_table()) {
        if (auto v = (*mcp)["enable_http"].value<bool>()) cfg.mcp.enable_http = *v;
        if (auto v = (*mcp)["bind_address"].value<std::string>()) cfg.mcp.bind_address = *v;
        if (auto v = (*mcp)["port"].value<std::int64_t>()) {
            cfg.mcp.port = static_cast<std::uint16_t>(*v);
        }
        if (auto v = (*mcp)["enable_metrics"].value<bool>()) cfg.mcp.enable_metrics = *v;
    }

    if (const auto* ag = tbl["antigravity"].as_table()) {
        if (auto v = (*ag)["app_data_dir"].value<std::string>()) cfg.antigravity.app_data_dir = *v;
        if (auto v = (*ag)["watcher_debounce_ms"].value<std::int64_t>()) {
            cfg.antigravity.watcher_debounce_ms = static_cast<std::uint32_t>(*v);
        }
        if (auto v = (*ag)["poll_interval_ms"].value<std::int64_t>()) {
            cfg.antigravity.poll_interval_ms = static_cast<std::uint32_t>(*v);
        }
        if (const auto* roots = (*ag)["extra_workspace_roots"].as_array()) {
            cfg.antigravity.extra_workspace_roots.clear();
            for (const auto& elem : *roots) {
                if (auto s = elem.value<std::string>()) cfg.antigravity.extra_workspace_roots.push_back(*s);
            }
        }
    }

    if (const auto* log = tbl["logging"].as_table()) {
        if (auto v = (*log)["level"].value<std::string>()) cfg.logging.level = *v;
        if (auto v = (*log)["json_format"].value<bool>()) cfg.logging.json_format = *v;
        if (auto v = (*log)["log_file"].value<std::string>()) cfg.logging.log_file = *v;
        if (auto v = (*log)["max_file_bytes"].value<std::int64_t>()) {
            cfg.logging.max_file_bytes = static_cast<std::size_t>(*v);
        }
        if (auto v = (*log)["max_files"].value<std::int64_t>()) {
            cfg.logging.max_files = static_cast<std::size_t>(*v);
        }
    }

    if (const auto* peers_arr = tbl["peers"].as_array()) {
        for (const auto& item : *peers_arr) {
            const auto* ptbl = item.as_table();
            if (!ptbl) continue;
            PeerConfig p;
            if (auto v = (*ptbl)["node_id"].value<std::string>()) p.node_id = *v;
            if (auto v = (*ptbl)["address"].value<std::string>()) p.address = *v;
            if (auto v = (*ptbl)["port"].value<std::int64_t>()) {
                p.port = static_cast<std::uint16_t>(*v);
            }
            if (auto v = (*ptbl)["pubkey_fingerprint"].value<std::string>()) p.pubkey_fingerprint = *v;
            if (auto v = (*ptbl)["ed25519_pubkey_hex"].value<std::string>()) p.ed25519_pubkey_hex = *v;
            if (auto v = (*ptbl)["is_seed"].value<bool>()) p.is_seed = *v;
            if (auto v = (*ptbl)["is_relay"].value<bool>()) p.is_relay = *v;
            if (!p.node_id.empty()) {
                cfg.peers.push_back(std::move(p));
            }
        }
    }

    return cfg;
}

void ConfigManager::save_to_file(const MeshConfig& config, const std::filesystem::path& path) {
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }

    toml::table root;

    toml::table node;
    node.insert("node_id", config.node_id);
    node.insert("hostname", config.hostname);
    node.insert("data_dir", config.data_dir);
    node.insert("db_path", config.db_path);
    node.insert("key_file", config.key_file);
    node.insert("ipc_socket_path", config.ipc_socket_path);
    toml::array tags_arr;
    for (const auto& t : config.tags) tags_arr.push_back(t);
    node.insert("tags", std::move(tags_arr));
    root.insert("node", std::move(node));

    toml::table net;
    net.insert("topology", config.network.topology == TopologyMode::HubAndSpoke ? "hub-and-spoke" : "p2p-mesh");
    net.insert("role", config.network.role == NodeRole::RelayHub ? "relay" : "peer");
    net.insert("listen_address", config.network.listen_address);
    net.insert("listen_port", static_cast<std::int64_t>(config.network.listen_port));
    net.insert("relay_endpoint", config.network.relay_endpoint);
    net.insert("enable_mdns_discovery", config.network.enable_mdns_discovery);
    net.insert("multicast_group", config.network.multicast_group);
    net.insert("discovery_port", static_cast<std::int64_t>(config.network.discovery_port));
    net.insert("cluster_psk_hex", config.network.cluster_psk_hex);
    net.insert("heartbeat_interval_ms", static_cast<std::int64_t>(config.network.heartbeat_interval_ms));
    net.insert("peer_timeout_ms", static_cast<std::int64_t>(config.network.peer_timeout_ms));
    net.insert("zstd_threshold_bytes", static_cast<std::int64_t>(config.network.zstd_threshold_bytes));
    net.insert("zstd_level", static_cast<std::int64_t>(config.network.zstd_level));
    net.insert("bandwidth_limit_kbps", static_cast<std::int64_t>(config.network.bandwidth_limit_kbps));
    net.insert("max_attachment_bytes", static_cast<std::int64_t>(config.network.max_attachment_bytes));
    net.insert("io_threads", static_cast<std::int64_t>(config.network.io_threads));
    root.insert("network", std::move(net));

    toml::table priv;
    priv.insert("metadata_only", config.privacy.metadata_only);
    priv.insert("sanitize_secrets", config.privacy.sanitize_secrets);
    priv.insert("entropy_threshold", config.privacy.entropy_threshold);
    priv.insert("entropy_min_length", static_cast<std::int64_t>(config.privacy.entropy_min_length));
    priv.insert("full_text_ttl_days", static_cast<std::int64_t>(config.privacy.full_text_ttl_days));
    toml::array allow_arr;
    for (const auto& g : config.privacy.allow_globs) allow_arr.push_back(g);
    priv.insert("allow_globs", std::move(allow_arr));
    toml::array deny_arr;
    for (const auto& g : config.privacy.deny_globs) deny_arr.push_back(g);
    priv.insert("deny_globs", std::move(deny_arr));
    root.insert("privacy", std::move(priv));

    toml::table mcp;
    mcp.insert("enable_http", config.mcp.enable_http);
    mcp.insert("bind_address", config.mcp.bind_address);
    mcp.insert("port", static_cast<std::int64_t>(config.mcp.port));
    mcp.insert("enable_metrics", config.mcp.enable_metrics);
    root.insert("mcp", std::move(mcp));

    toml::table ag;
    ag.insert("app_data_dir", config.antigravity.app_data_dir);
    ag.insert("watcher_debounce_ms", static_cast<std::int64_t>(config.antigravity.watcher_debounce_ms));
    ag.insert("poll_interval_ms", static_cast<std::int64_t>(config.antigravity.poll_interval_ms));
    toml::array roots_arr;
    for (const auto& r : config.antigravity.extra_workspace_roots) roots_arr.push_back(r);
    ag.insert("extra_workspace_roots", std::move(roots_arr));
    root.insert("antigravity", std::move(ag));

    toml::table log;
    log.insert("level", config.logging.level);
    log.insert("json_format", config.logging.json_format);
    log.insert("log_file", config.logging.log_file);
    log.insert("max_file_bytes", static_cast<std::int64_t>(config.logging.max_file_bytes));
    log.insert("max_files", static_cast<std::int64_t>(config.logging.max_files));
    root.insert("logging", std::move(log));

    toml::array peers_arr;
    for (const auto& p : config.peers) {
        toml::table ptbl;
        ptbl.insert("node_id", p.node_id);
        ptbl.insert("address", p.address);
        ptbl.insert("port", static_cast<std::int64_t>(p.port));
        ptbl.insert("pubkey_fingerprint", p.pubkey_fingerprint);
        ptbl.insert("ed25519_pubkey_hex", p.ed25519_pubkey_hex);
        ptbl.insert("is_seed", p.is_seed);
        ptbl.insert("is_relay", p.is_relay);
        peers_arr.push_back(std::move(ptbl));
    }
    root.insert("peers", std::move(peers_arr));

    const auto tmp_path = path.string() + ".tmp";
    {
        std::ofstream ofs(tmp_path, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            throw std::runtime_error("Failed to open temp config file for writing: " + tmp_path);
        }
        ofs << root << "\n";
        ofs.flush();
        if (!ofs) {
            throw std::runtime_error("Failed to flush config file: " + tmp_path);
        }
    }
    std::filesystem::rename(tmp_path, path);
}

ConfigValidationResult ConfigManager::validate(const MeshConfig& config) {
    ConfigValidationResult res;
    if (config.node_id.empty()) {
        res.errors.push_back("node.node_id must not be empty");
    }
    if (config.db_path.empty()) {
        res.errors.push_back("node.db_path must not be empty");
    }
    if (config.network.listen_port == 0) {
        res.errors.push_back("network.listen_port must be in 1..65535");
    }
    if (config.network.cluster_psk_hex.empty()) {
        res.warnings.push_back("network.cluster_psk_hex is empty; cluster peer authentication requires a PSK");
    } else if (config.network.cluster_psk_hex.size() != 64) {
        res.errors.push_back("network.cluster_psk_hex must be 64 hex characters (256-bit key)");
    }
    if (config.network.peer_timeout_ms <= config.network.heartbeat_interval_ms) {
        res.errors.push_back("network.peer_timeout_ms must exceed network.heartbeat_interval_ms");
    }
    if (config.network.topology == TopologyMode::HubAndSpoke &&
        config.network.role == NodeRole::Peer &&
        config.network.relay_endpoint.empty()) {
        res.warnings.push_back("hub-and-spoke topology configured on peer without network.relay_endpoint");
    }
    if (config.privacy.entropy_threshold < 1.0 || config.privacy.entropy_threshold > 8.0) {
        res.errors.push_back("privacy.entropy_threshold must be between 1.0 and 8.0");
    }
    if (config.privacy.full_text_ttl_days < 0) {
        res.errors.push_back("privacy.full_text_ttl_days must be >= 0");
    }
    for (const auto& p : config.peers) {
        if (p.node_id.empty()) {
            res.errors.push_back("Every peer entry must define node_id");
        }
        if (p.pubkey_fingerprint.empty() && p.ed25519_pubkey_hex.empty()) {
            res.errors.push_back("Peer '" + p.node_id + "' must specify pubkey_fingerprint or ed25519_pubkey_hex");
        }
    }
    res.ok = res.errors.empty();
    return res;
}

bool ConfigManager::glob_match(const std::string& pattern, const std::string& text) {
    // Normalize path separators to '/' for consistent glob matching across Windows/POSIX.
    auto norm = [](std::string s) {
        std::replace(s.begin(), s.end(), '\\', '/');
        return s;
    };
    const std::string p = norm(pattern);
    const std::string t = norm(text);

    std::size_t pi = 0;
    std::size_t ti = 0;
    std::size_t star_p = std::string::npos;
    std::size_t star_t = 0;

    while (ti < t.size()) {
        if (pi < p.size() && (p[pi] == '?' || p[pi] == t[ti])) {
            ++pi;
            ++ti;
        } else if (pi < p.size() && p[pi] == '*') {
            star_p = pi++;
            star_t = ti;
        } else if (star_p != std::string::npos) {
            pi = star_p + 1;
            ti = ++star_t;
        } else {
            return false;
        }
    }
    while (pi < p.size() && p[pi] == '*') {
        ++pi;
    }
    return pi == p.size();
}

bool ConfigManager::is_path_allowed(const PrivacyConfig& privacy, const std::string& path) {
    for (const auto& deny : privacy.deny_globs) {
        if (glob_match(deny, path)) {
            return false;
        }
    }
    if (privacy.allow_globs.empty()) {
        return true;
    }
    for (const auto& allow : privacy.allow_globs) {
        if (glob_match(allow, path)) {
            return true;
        }
    }
    return false;
}

ConfigManager::ConfigManager(std::filesystem::path config_path)
    : config_path_(std::move(config_path)) {
    if (std::filesystem::exists(config_path_)) {
        current_ = load_from_file(config_path_);
    } else {
        current_ = create_default_config();
    }
}

MeshConfig ConfigManager::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_;
}

bool ConfigManager::reload(std::string& error_out) {
    try {
        MeshConfig loaded = load_from_file(config_path_);
        const auto val = validate(loaded);
        if (!val.ok) {
            error_out = val.errors.front();
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        // Hot-reload peers, privacy, and network thresholds safely.
        current_.peers = std::move(loaded.peers);
        current_.privacy = std::move(loaded.privacy);
        current_.tags = std::move(loaded.tags);
        current_.antigravity.extra_workspace_roots = std::move(loaded.antigravity.extra_workspace_roots);
        return true;
    } catch (const std::exception& ex) {
        error_out = ex.what();
        return false;
    }
}

bool ConfigManager::add_or_update_peer(const PeerConfig& peer, std::string& error_out) {
    if (peer.node_id.empty()) {
        error_out = "peer node_id cannot be empty";
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(current_.peers.begin(), current_.peers.end(), [&](const PeerConfig& p) {
        return p.node_id == peer.node_id;
    });
    if (it != current_.peers.end()) {
        *it = peer;
    } else {
        current_.peers.push_back(peer);
    }
    try {
        save_to_file(current_, config_path_);
        return true;
    } catch (const std::exception& ex) {
        error_out = ex.what();
        return false;
    }
}

bool ConfigManager::apply_joined_cluster_config(const std::string& psk_hex,
                                                const std::vector<PeerConfig>& peers,
                                                std::string& error_out) {
    if (psk_hex.size() != 64) {
        error_out = "Invalid cluster_psk_hex length (expected 64 hex characters)";
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    current_.network.cluster_psk_hex = psk_hex;
    for (const auto& peer : peers) {
        if (peer.node_id.empty() || peer.node_id == current_.node_id) continue;
        auto it = std::find_if(current_.peers.begin(), current_.peers.end(), [&](const PeerConfig& p) {
            return p.node_id == peer.node_id;
        });
        if (it != current_.peers.end()) {
            *it = peer;
        } else {
            current_.peers.push_back(peer);
        }
    }
    try {
        save_to_file(current_, config_path_);
        return true;
    } catch (const std::exception& ex) {
        error_out = ex.what();
        return false;
    }
}

bool ConfigManager::remove_peer(const std::string& node_id, std::string& error_out) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto old_sz = current_.peers.size();
    current_.peers.erase(
        std::remove_if(current_.peers.begin(), current_.peers.end(), [&](const PeerConfig& p) {
            return p.node_id == node_id;
        }),
        current_.peers.end());
    if (current_.peers.size() == old_sz) {
        error_out = "peer not found: " + node_id;
        return false;
    }
    try {
        save_to_file(current_, config_path_);
        return true;
    } catch (const std::exception& ex) {
        error_out = ex.what();
        return false;
    }
}

} // namespace agmesh::core
