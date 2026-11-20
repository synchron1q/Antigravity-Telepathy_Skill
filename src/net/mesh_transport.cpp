#include "agmesh/net/mesh_transport.hpp"

#include <nlohmann/json.hpp>
#include <openssl/rand.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <deque>
#include <iomanip>
#include <sstream>

namespace agmesh::net {

namespace {

std::vector<std::uint8_t> str_to_bytes(const std::string& s) {
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

std::string bytes_to_str(const std::vector<std::uint8_t>& b) {
    return std::string(b.begin(), b.end());
}

nlohmann::json hlc_to_json(const core::HlcTimestamp& h) {
    return {{"p", h.physical_ms}, {"l", h.logical}, {"o", h.origin_node_id}};
}

core::HlcTimestamp json_to_hlc(const nlohmann::json& j) {
    core::HlcTimestamp h;
    if (!j.is_object()) return h;
    h.physical_ms = j.value("p", 0ULL);
    h.logical = j.value("l", 0U);
    h.origin_node_id = j.value("o", "");
    return h;
}

nlohmann::json session_to_json(const core::SessionRecord& s) {
    return {
        {"session_id", s.session_id},
        {"node_id", s.node_id},
        {"workspace_path", s.workspace_path},
        {"repo_name", s.repo_name},
        {"git_branch", s.git_branch},
        {"title", s.title},
        {"status", s.status},
        {"is_private", s.is_private},
        {"source_adapter", s.source_adapter},
        {"tags", s.tags},
        {"created_at_ms", s.created_at_ms},
        {"updated_at_ms", s.updated_at_ms},
        {"hlc", hlc_to_json(s.hlc)}
    };
}

core::SessionRecord json_to_session(const nlohmann::json& j) {
    core::SessionRecord s;
    s.session_id = j.value("session_id", "");
    s.node_id = j.value("node_id", "");
    s.workspace_path = j.value("workspace_path", "");
    s.repo_name = j.value("repo_name", "");
    s.git_branch = j.value("git_branch", "");
    s.title = j.value("title", "");
    s.status = j.value("status", "active");
    s.is_private = j.value("is_private", false);
    s.source_adapter = j.value("source_adapter", "remote");
    s.tags = j.value("tags", std::vector<std::string>{});
    s.created_at_ms = j.value("created_at_ms", 0LL);
    s.updated_at_ms = j.value("updated_at_ms", 0LL);
    if (j.contains("hlc")) s.hlc = json_to_hlc(j["hlc"]);
    return s;
}

nlohmann::json message_to_json(const core::MessageRecord& m) {
    return {
        {"message_id", m.message_id},
        {"session_id", m.session_id},
        {"node_id", m.node_id},
        {"seq_index", m.seq_index},
        {"role", m.role},
        {"content", m.content},
        {"touched_files", m.touched_files},
        {"content_sha256", m.content_sha256},
        {"created_at_ms", m.created_at_ms},
        {"hlc", hlc_to_json(m.hlc)}
    };
}

core::MessageRecord json_to_message(const nlohmann::json& j) {
    core::MessageRecord m;
    m.message_id = j.value("message_id", "");
    m.session_id = j.value("session_id", "");
    m.node_id = j.value("node_id", "");
    m.seq_index = j.value("seq_index", 0LL);
    m.role = j.value("role", "user");
    m.content = j.value("content", "");
    m.touched_files = j.value("touched_files", std::vector<std::string>{});
    m.content_sha256 = j.value("content_sha256", "");
    m.created_at_ms = j.value("created_at_ms", 0LL);
    if (j.contains("hlc")) m.hlc = json_to_hlc(j["hlc"]);
    return m;
}

nlohmann::json summary_to_json(const core::SessionSummary& sm) {
    return {
        {"session_id", sm.session_id},
        {"node_id", sm.node_id},
        {"current_goal", sm.current_goal},
        {"decisions", sm.decisions},
        {"modified_files", sm.modified_files},
        {"bugs_found", sm.bugs_found},
        {"open_questions", sm.open_questions},
        {"summary_text", sm.summary_text},
        {"updated_at_ms", sm.updated_at_ms},
        {"hlc", hlc_to_json(sm.hlc)}
    };
}

core::SessionSummary json_to_summary(const nlohmann::json& j) {
    core::SessionSummary sm;
    sm.session_id = j.value("session_id", "");
    sm.node_id = j.value("node_id", "");
    sm.current_goal = j.value("current_goal", "");
    sm.decisions = j.value("decisions", std::vector<std::string>{});
    sm.modified_files = j.value("modified_files", std::vector<std::string>{});
    sm.bugs_found = j.value("bugs_found", std::vector<std::string>{});
    sm.open_questions = j.value("open_questions", std::vector<std::string>{});
    sm.summary_text = j.value("summary_text", "");
    sm.updated_at_ms = j.value("updated_at_ms", 0LL);
    if (j.contains("hlc")) sm.hlc = json_to_hlc(j["hlc"]);
    return sm;
}

nlohmann::json workspace_to_json(const core::WorkspaceInfo& w) {
    return {
        {"workspace_id", w.workspace_id},
        {"node_id", w.node_id},
        {"root_path", w.root_path},
        {"repo_name", w.repo_name},
        {"git_branch", w.git_branch},
        {"git_commit", w.git_commit},
        {"is_open", w.is_open},
        {"active_task", w.active_task},
        {"last_active_ms", w.last_active_ms},
        {"hlc", hlc_to_json(w.hlc)}
    };
}

core::WorkspaceInfo json_to_workspace(const nlohmann::json& j) {
    core::WorkspaceInfo w;
    w.workspace_id = j.value("workspace_id", "");
    w.node_id = j.value("node_id", "");
    w.root_path = j.value("root_path", "");
    w.repo_name = j.value("repo_name", "");
    w.git_branch = j.value("git_branch", "");
    w.git_commit = j.value("git_commit", "");
    w.is_open = j.value("is_open", false);
    w.active_task = j.value("active_task", "");
    w.last_active_ms = j.value("last_active_ms", 0LL);
    if (j.contains("hlc")) w.hlc = json_to_hlc(j["hlc"]);
    return w;
}

nlohmann::json handoff_to_json(const core::HandoffPacket& h) {
    return {
        {"handoff_id", h.handoff_id},
        {"from_node_id", h.from_node_id},
        {"target_node_id", h.target_node_id},
        {"session_id", h.session_id},
        {"claimed_by_node", h.claimed_by_node},
        {"claimed_session_id", h.claimed_session_id},
        {"repo_name", h.repo_name},
        {"workspace_path", h.workspace_path},
        {"git_branch", h.git_branch},
        {"git_commit", h.git_commit},
        {"task_description", h.task_description},
        {"uncommitted_diff", h.uncommitted_diff},
        {"blockers", h.blockers},
        {"next_steps", h.next_steps},
        {"artifact_refs", h.artifact_refs},
        {"status", h.status},
        {"created_at_ms", h.created_at_ms},
        {"updated_at_ms", h.updated_at_ms},
        {"hlc", hlc_to_json(h.hlc)}
    };
}

core::HandoffPacket json_to_handoff(const nlohmann::json& j) {
    core::HandoffPacket h;
    h.handoff_id = j.value("handoff_id", "");
    h.from_node_id = j.value("from_node_id", "");
    h.target_node_id = j.value("target_node_id", "");
    h.session_id = j.value("session_id", "");
    h.claimed_by_node = j.value("claimed_by_node", "");
    h.claimed_session_id = j.value("claimed_session_id", "");
    h.repo_name = j.value("repo_name", "");
    h.workspace_path = j.value("workspace_path", "");
    h.git_branch = j.value("git_branch", "");
    h.git_commit = j.value("git_commit", "");
    h.task_description = j.value("task_description", "");
    h.uncommitted_diff = j.value("uncommitted_diff", "");
    h.blockers = j.value("blockers", std::vector<std::string>{});
    h.next_steps = j.value("next_steps", std::vector<std::string>{});
    h.artifact_refs = j.value("artifact_refs", std::vector<std::string>{});
    h.status = j.value("status", "pending");
    h.created_at_ms = j.value("created_at_ms", 0LL);
    h.updated_at_ms = j.value("updated_at_ms", 0LL);
    if (j.contains("hlc")) h.hlc = json_to_hlc(j["hlc"]);
    return h;
}

nlohmann::json lock_to_json(const core::DistributedLock& l) {
    return {
        {"lock_key", l.lock_key},
        {"owner_node_id", l.owner_node_id},
        {"owner_session_id", l.owner_session_id},
        {"reason", l.reason},
        {"acquired_at_ms", l.acquired_at_ms},
        {"expires_at_ms", l.expires_at_ms},
        {"released", l.released},
        {"hlc", hlc_to_json(l.hlc)}
    };
}

core::DistributedLock json_to_lock(const nlohmann::json& j) {
    core::DistributedLock l;
    l.lock_key = j.value("lock_key", "");
    l.owner_node_id = j.value("owner_node_id", "");
    l.owner_session_id = j.value("owner_session_id", "");
    l.reason = j.value("reason", "");
    l.acquired_at_ms = j.value("acquired_at_ms", 0LL);
    l.expires_at_ms = j.value("expires_at_ms", 0LL);
    l.released = j.value("released", false);
    if (j.contains("hlc")) l.hlc = json_to_hlc(j["hlc"]);
    return l;
}

nlohmann::json tombstone_to_json(const core::TombstoneRecord& t) {
    return {
        {"target_type", t.target_type},
        {"target_id", t.target_id},
        {"purged_at_ms", t.purged_at_ms},
        {"hlc", hlc_to_json(t.hlc)}
    };
}

core::TombstoneRecord json_to_tombstone(const nlohmann::json& j) {
    core::TombstoneRecord t;
    t.target_type = j.value("target_type", "");
    t.target_id = j.value("target_id", "");
    t.purged_at_ms = j.value("purged_at_ms", 0LL);
    if (j.contains("hlc")) t.hlc = json_to_hlc(j["hlc"]);
    return t;
}

std::string build_delta_json(const std::vector<core::SessionRecord>& sessions,
                             const std::vector<core::MessageRecord>& messages,
                             const std::vector<core::SessionSummary>& summaries,
                             const std::vector<core::WorkspaceInfo>& workspaces,
                             const std::vector<core::HandoffPacket>& handoffs,
                             const std::vector<core::DistributedLock>& locks,
                             const std::vector<core::TombstoneRecord>& tombstones) {
    nlohmann::json root;
    root["sessions"] = nlohmann::json::array();
    for (const auto& s : sessions) root["sessions"].push_back(session_to_json(s));
    root["messages"] = nlohmann::json::array();
    for (const auto& m : messages) root["messages"].push_back(message_to_json(m));
    root["summaries"] = nlohmann::json::array();
    for (const auto& sm : summaries) root["summaries"].push_back(summary_to_json(sm));
    root["workspaces"] = nlohmann::json::array();
    for (const auto& w : workspaces) root["workspaces"].push_back(workspace_to_json(w));
    root["handoffs"] = nlohmann::json::array();
    for (const auto& h : handoffs) root["handoffs"].push_back(handoff_to_json(h));
    root["locks"] = nlohmann::json::array();
    for (const auto& l : locks) root["locks"].push_back(lock_to_json(l));
    root["tombstones"] = nlohmann::json::array();
    for (const auto& t : tombstones) root["tombstones"].push_back(tombstone_to_json(t));
    return root.dump();
}

std::string canonicalize_pin(const std::string& raw) {
    std::string digits;
    for (unsigned char c : raw) {
        if (std::isdigit(c)) digits.push_back(static_cast<char>(c));
    }
    return digits;
}

std::vector<std::uint8_t> derive_pair_auth_key(const std::string& secret_or_pin) {
    const std::string hash_hex = core::CrdtResolver::sha256_hex("agmesh-pair-v1:" + secret_or_pin);
    return CryptoEngine::hex_to_bytes(hash_hex);
}

std::array<std::uint8_t, 32> derive_pair_welcome_key(const std::array<std::uint8_t, 32>& my_eph_priv,
                                                     const std::array<std::uint8_t, 32>& peer_eph_pub,
                                                     const std::vector<std::uint8_t>& auth_key) {
    std::array<std::uint8_t, 32> dh{};
    CryptoEngine::compute_x25519_shared_secret(my_eph_priv, peer_eph_pub, dh);
    std::vector<std::uint8_t> ikm;
    ikm.insert(ikm.end(), dh.begin(), dh.end());
    ikm.insert(ikm.end(), auth_key.begin(), auth_key.end());
    std::vector<std::uint8_t> out;
    CryptoEngine::hkdf_sha256(ikm, {}, str_to_bytes("agmesh-pair-welcome-v1"), out, 32);
    std::array<std::uint8_t, 32> key{};
    std::copy_n(out.begin(), 32, key.begin());
    return key;
}

std::vector<std::uint8_t> build_pair_request_transcript(const std::string& joiner_id,
                                                        const std::string& ed_pub_hex,
                                                        const std::string& eph_pub_hex,
                                                        std::uint16_t listen_port,
                                                        std::int64_t ts_ms) {
    const std::string s = joiner_id + "|" + ed_pub_hex + "|" + eph_pub_hex + "|" +
                          std::to_string(listen_port) + "|" + std::to_string(ts_ms);
    return str_to_bytes(s);
}

std::vector<std::uint8_t> build_peer_gossip_sign_input(const std::string& introducer_id,
                                                       const core::PeerConfig& p,
                                                       std::int64_t ts_ms) {
    const std::string s = introducer_id + "|" + p.node_id + "|" + p.pubkey_fingerprint + "|" +
                          p.address + "|" + std::to_string(p.port) + "|" + std::to_string(ts_ms);
    return str_to_bytes(s);
}

bool parse_host_port(const std::string& input, std::uint16_t default_port, std::string& host_out, std::uint16_t& port_out) {
    if (input.empty()) return false;
    if (input.front() == '[') {
        const auto close = input.find(']');
        if (close == std::string::npos) return false;
        host_out = input.substr(1, close - 1);
        if (close + 1 < input.size() && input[close + 1] == ':') {
            try {
                port_out = static_cast<std::uint16_t>(std::stoul(input.substr(close + 2)));
            } catch (...) {
                return false;
            }
        } else {
            port_out = default_port;
        }
        return !host_out.empty();
    }
    const auto first_colon = input.find(':');
    const auto last_colon = input.rfind(':');
    if (first_colon != std::string::npos && first_colon == last_colon) {
        host_out = input.substr(0, first_colon);
        try {
            port_out = static_cast<std::uint16_t>(std::stoul(input.substr(first_colon + 1)));
        } catch (...) {
            return false;
        }
        return !host_out.empty() && port_out > 0;
    }
    host_out = input;
    port_out = default_port;
    return true;
}

} // namespace

class PeerSession : public std::enable_shared_from_this<PeerSession> {
public:
    enum class State {
        HandshakeInitWait,
        HandshakeRespWait,
        HandshakeFinishWait,
        Established,
        Closed
    };

    PeerSession(MeshTransport& owner,
                asio::ip::tcp::socket socket,
                bool is_initiator,
                std::string expected_peer_id = "")
        : owner_(owner),
          socket_(std::move(socket)),
          strand_(asio::make_strand(owner.io_ctx_)),
          is_initiator_(is_initiator),
          peer_node_id_(std::move(expected_peer_id)),
          last_activity_ms_(core::now_ms()) {
        state_ = is_initiator ? State::HandshakeRespWait : State::HandshakeInitWait;
    }

    void start() {
        auto self = shared_from_this();
        asio::dispatch(strand_, [this, self]() {
            if (is_initiator_) {
                send_handshake_init();
            }
            read_next_header();
        });
    }

    void close() {
        auto self = shared_from_this();
        asio::dispatch(strand_, [this, self]() { close_internal(); });
    }

    void send_frame(FrameType type,
                    std::uint32_t stream_id,
                    const std::vector<std::uint8_t>& plaintext_payload,
                    bool high_priority = false) {
        auto self = shared_from_this();
        asio::post(strand_, [this, self, type, stream_id, plaintext_payload, high_priority]() {
            if (state_ == State::Closed) return;
            const auto cfg = owner_.config_mgr_.snapshot();
            std::vector<std::uint8_t> wire;
            SessionKeys* keys_ptr = (state_ == State::Established || type == FrameType::HandshakeFinish)
                                        ? &session_keys_
                                        : nullptr;
            const std::uint32_t seq = ++stream_seq_[stream_id];
            if (!ProtocolCodec::encode_frame(type,
                                             stream_id,
                                             seq,
                                             kFlagNone,
                                             plaintext_payload,
                                             cfg.network.zstd_threshold_bytes,
                                             cfg.network.zstd_level,
                                             keys_ptr,
                                             wire,
                                             cfg.network.max_attachment_bytes)) {
                return;
            }
            if (high_priority) {
                // Control stream 0 frames jump ahead of bulk data frames to prevent HOL blocking.
                write_queue_.push_front(std::move(wire));
            } else {
                write_queue_.push_back(std::move(wire));
            }
            if (!writing_) {
                do_write();
            }
        });
    }

    void send_heartbeat_ping() {
        nlohmann::json j = {
            {"sender_ts_us", core::now_us()},
            {"node_id", owner_.local_keys_.node_id}
        };
        send_frame(FrameType::HeartbeatPing, 0, str_to_bytes(j.dump()), true);
    }

    [[nodiscard]] std::string peer_node_id() const {
        std::lock_guard<std::mutex> lock(meta_mutex_);
        return peer_node_id_;
    }

    [[nodiscard]] double last_rtt_ms() const noexcept {
        std::lock_guard<std::mutex> lock(meta_mutex_);
        return rtt_ms_;
    }

    [[nodiscard]] std::int64_t last_activity_ms() const noexcept {
        return last_activity_ms_.load();
    }

    [[nodiscard]] bool is_established() const noexcept {
        return established_.load();
    }

    [[nodiscard]] bool is_initiator() const noexcept {
        return is_initiator_;
    }

    [[nodiscard]] std::string remote_endpoint_str() const {
        std::lock_guard<std::mutex> lock(meta_mutex_);
        return remote_ep_;
    }

private:
    void close_internal() {
        if (state_ == State::Closed) return;
        state_ = State::Closed;
        established_.store(false);
        std::error_code ec;
        socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        socket_.close(ec);
        std::string pid;
        {
            std::lock_guard<std::mutex> lock(meta_mutex_);
            pid = peer_node_id_;
        }
        if (!pid.empty()) {
            owner_.unregister_session(pid, this);
        }
    }

    void send_error_and_close(ProtocolErrorCode code, const std::string& msg) {
        nlohmann::json j = {{"code", static_cast<int>(code)}, {"message", msg}};
        std::vector<std::uint8_t> wire;
        ProtocolCodec::encode_frame(
            FrameType::FrameError, 0, 1, kFlagNone, str_to_bytes(j.dump()), 0, 1, nullptr, wire);
        write_queue_.push_back(std::move(wire));
        if (!writing_) do_write();
        owner_.auth_failures_.fetch_add(1);
        close_internal();
    }

    void send_handshake_init() {
        eph_ = CryptoEngine::generate_x25519_ephemeral();
        const auto cfg = owner_.config_mgr_.snapshot();
        const auto psk_bytes = CryptoEngine::hex_to_bytes(cfg.network.cluster_psk_hex);

        std::vector<std::uint8_t> mac_input;
        mac_input.insert(mac_input.end(), eph_.pub.begin(), eph_.pub.end());
        mac_input.insert(mac_input.end(), owner_.local_keys_.node_id.begin(), owner_.local_keys_.node_id.end());
        const auto psk_mac = CryptoEngine::hmac_sha256(psk_bytes, mac_input);

        nlohmann::json j = {
            {"node_id", owner_.local_keys_.node_id},
            {"eph_pub_hex", CryptoEngine::bytes_to_hex(eph_.pub.data(), eph_.pub.size())},
            {"ed25519_pub_hex", owner_.local_keys_.public_key_hex},
            {"psk_hmac_hex", CryptoEngine::bytes_to_hex(psk_mac.data(), psk_mac.size())}
        };
        const std::string payload = j.dump();
        transcript_.insert(transcript_.end(), payload.begin(), payload.end());
        send_frame(FrameType::HandshakeInit, 0, str_to_bytes(payload), true);
    }

    bool verify_peer_whitelist(const std::string& claimed_node_id,
                               const std::array<std::uint8_t, 32>& peer_ed_pub,
                               std::string& diag_err) const {
        const auto cfg = owner_.config_mgr_.snapshot();
        const std::string fp = CryptoEngine::compute_fingerprint(peer_ed_pub);
        for (const auto& p : cfg.peers) {
            if (p.node_id == claimed_node_id) {
                if (!p.pubkey_fingerprint.empty() && p.pubkey_fingerprint == fp) return true;
                if (!p.ed25519_pubkey_hex.empty() &&
                    p.ed25519_pubkey_hex == CryptoEngine::bytes_to_hex(peer_ed_pub.data(), peer_ed_pub.size())) {
                    return true;
                }
                diag_err = "Peer Ed25519 fingerprint mismatch for " + claimed_node_id;
                return false;
            }
        }
        diag_err = "Peer not in authorized whitelist: " + claimed_node_id;
        return false;
    }

    void derive_session_keys(const std::array<std::uint8_t, 32>& peer_eph_pub, bool is_init) {
        std::array<std::uint8_t, 32> dh{};
        CryptoEngine::compute_x25519_shared_secret(eph_.priv, peer_eph_pub, dh);

        const auto cfg = owner_.config_mgr_.snapshot();
        const auto psk = CryptoEngine::hex_to_bytes(cfg.network.cluster_psk_hex);

        std::vector<std::uint8_t> ikm;
        ikm.insert(ikm.end(), dh.begin(), dh.end());
        ikm.insert(ikm.end(), psk.begin(), psk.end());

        std::vector<std::uint8_t> derived;
        CryptoEngine::hkdf_sha256(ikm, {}, str_to_bytes("ag-mesh-v1-session"), derived, 64);

        if (is_init) {
            std::copy_n(derived.data(), 32, session_keys_.tx_key.begin());
            std::copy_n(derived.data() + 32, 32, session_keys_.rx_key.begin());
        } else {
            std::copy_n(derived.data(), 32, session_keys_.rx_key.begin());
            std::copy_n(derived.data() + 32, 32, session_keys_.tx_key.begin());
        }
    }

    void handle_handshake_frame(const FrameHeader& hdr, const std::vector<std::uint8_t>& raw_payload) {
        const auto cfg = owner_.config_mgr_.snapshot();
        const auto psk_bytes = CryptoEngine::hex_to_bytes(cfg.network.cluster_psk_hex);

        if (hdr.type == FrameType::PairRequest && state_ == State::HandshakeInitWait) {
            try {
                const auto j = nlohmann::json::parse(bytes_to_str(raw_payload));
                const std::string joiner_id = j.at("joiner_node_id").get<std::string>();
                const std::uint16_t joiner_port = j.value("joiner_listen_port", static_cast<std::uint16_t>(47890));
                const std::string ed_hex = j.at("joiner_ed25519_pub_hex").get<std::string>();
                const std::string eph_hex = j.at("joiner_eph_pub_hex").get<std::string>();
                const std::string auth_mode = j.value("auth_mode", "secret");
                const std::int64_t ts_ms = j.at("ts_ms").get<std::int64_t>();
                const auto hmac_bytes = CryptoEngine::hex_to_bytes(j.at("hmac_hex").get<std::string>());
                const auto sig_bytes = CryptoEngine::hex_to_bytes(j.at("sig_hex").get<std::string>());
                const auto ed_bytes = CryptoEngine::hex_to_bytes(ed_hex);
                const auto eph_bytes = CryptoEngine::hex_to_bytes(eph_hex);

                if (ed_bytes.size() != 32 || eph_bytes.size() != 32 || hmac_bytes.size() != 32 || sig_bytes.size() != 64) {
                    close_after_write_ = true;
                    send_frame(FrameType::PairReject, 0, str_to_bytes(R"({"reason":"Invalid pairing key sizes"})"), true);
                    return;
                }

                std::array<std::uint8_t, 32> joiner_ed{};
                std::array<std::uint8_t, 32> joiner_eph{};
                std::array<std::uint8_t, 32> recv_hmac{};
                std::array<std::uint8_t, 64> recv_sig{};
                std::copy_n(ed_bytes.begin(), 32, joiner_ed.begin());
                std::copy_n(eph_bytes.begin(), 32, joiner_eph.begin());
                std::copy_n(hmac_bytes.begin(), 32, recv_hmac.begin());
                std::copy_n(sig_bytes.begin(), 64, recv_sig.begin());

                std::string remote_ip = "127.0.0.1";
                {
                    std::error_code ec;
                    const auto ep = socket_.remote_endpoint(ec);
                    if (!ec) remote_ip = ep.address().to_string();
                }

                const auto transcript = build_pair_request_transcript(joiner_id, ed_hex, eph_hex, joiner_port, ts_ms);
                std::string welcome_json;
                std::string reject_reason;
                close_after_write_ = true;
                if (owner_.validate_and_accept_pair_request(
                        joiner_id,
                        joiner_port,
                        joiner_ed,
                        joiner_eph,
                        auth_mode,
                        transcript,
                        recv_hmac,
                        recv_sig,
                        remote_ip,
                        welcome_json,
                        reject_reason)) {
                    send_frame(FrameType::PairWelcome, 0, str_to_bytes(welcome_json), true);
                } else {
                    const std::string rej = nlohmann::json{{"reason", reject_reason}}.dump();
                    send_frame(FrameType::PairReject, 0, str_to_bytes(rej), true);
                }
            } catch (...) {
                close_after_write_ = true;
                send_frame(FrameType::PairReject, 0, str_to_bytes(R"({"reason":"Malformed PairRequest"})"), true);
            }
            return;
        }

        if (hdr.type == FrameType::HandshakeInit && state_ == State::HandshakeInitWait) {
            try {
                const auto j = nlohmann::json::parse(bytes_to_str(raw_payload));
                const std::string remote_id = j.at("node_id").get<std::string>();
                const auto eph_bytes = CryptoEngine::hex_to_bytes(j.at("eph_pub_hex").get<std::string>());
                const auto ed_bytes = CryptoEngine::hex_to_bytes(j.at("ed25519_pub_hex").get<std::string>());
                const auto mac_bytes = CryptoEngine::hex_to_bytes(j.at("psk_hmac_hex").get<std::string>());
                if (eph_bytes.size() != 32 || ed_bytes.size() != 32 || mac_bytes.size() != 32) {
                    send_error_and_close(ProtocolErrorCode::AuthFailed, "Invalid handshake key lengths");
                    return;
                }

                std::vector<std::uint8_t> mac_input;
                mac_input.insert(mac_input.end(), eph_bytes.begin(), eph_bytes.end());
                mac_input.insert(mac_input.end(), remote_id.begin(), remote_id.end());
                const auto expected_mac = CryptoEngine::hmac_sha256(psk_bytes, mac_input);
                if (!CryptoEngine::constant_time_equal(expected_mac.data(), mac_bytes.data(), 32)) {
                    send_error_and_close(ProtocolErrorCode::AuthFailed, "Cluster PSK HMAC mismatch");
                    return;
                }

                std::copy_n(ed_bytes.begin(), 32, peer_ed_pub_.begin());
                std::string wl_err;
                if (!verify_peer_whitelist(remote_id, peer_ed_pub_, wl_err)) {
                    send_error_and_close(ProtocolErrorCode::UnauthorizedPeer, wl_err);
                    return;
                }

                {
                    std::lock_guard<std::mutex> lock(meta_mutex_);
                    peer_node_id_ = remote_id;
                    std::error_code ec;
                    const auto ep = socket_.remote_endpoint(ec);
                    if (!ec) remote_ep_ = ep.address().to_string() + ":" + std::to_string(ep.port());
                }

                transcript_.insert(transcript_.end(), raw_payload.begin(), raw_payload.end());
                eph_ = CryptoEngine::generate_x25519_ephemeral();
                std::array<std::uint8_t, 32> peer_eph{};
                std::copy_n(eph_bytes.begin(), 32, peer_eph.begin());
                derive_session_keys(peer_eph, false);

                std::array<std::uint8_t, 64> sig{};
                CryptoEngine::ed25519_sign(owner_.local_keys_.ed25519_private, transcript_, sig);

                nlohmann::json resp = {
                    {"node_id", owner_.local_keys_.node_id},
                    {"eph_pub_hex", CryptoEngine::bytes_to_hex(eph_.pub.data(), eph_.pub.size())},
                    {"ed25519_pub_hex", owner_.local_keys_.public_key_hex},
                    {"sig_hex", CryptoEngine::bytes_to_hex(sig.data(), sig.size())}
                };
                const std::string resp_str = resp.dump();
                transcript_.insert(transcript_.end(), resp_str.begin(), resp_str.end());
                state_ = State::HandshakeFinishWait;
                send_frame(FrameType::HandshakeResp, 0, str_to_bytes(resp_str), true);
            } catch (...) {
                send_error_and_close(ProtocolErrorCode::AuthFailed, "Malformed HandshakeInit");
            }
            return;
        }

        if (hdr.type == FrameType::HandshakeResp && state_ == State::HandshakeRespWait) {
            try {
                const auto j = nlohmann::json::parse(bytes_to_str(raw_payload));
                const std::string remote_id = j.at("node_id").get<std::string>();
                const auto eph_bytes = CryptoEngine::hex_to_bytes(j.at("eph_pub_hex").get<std::string>());
                const auto ed_bytes = CryptoEngine::hex_to_bytes(j.at("ed25519_pub_hex").get<std::string>());
                const auto sig_bytes = CryptoEngine::hex_to_bytes(j.at("sig_hex").get<std::string>());
                if (eph_bytes.size() != 32 || ed_bytes.size() != 32 || sig_bytes.size() != 64) {
                    send_error_and_close(ProtocolErrorCode::AuthFailed, "Invalid HandshakeResp key sizes");
                    return;
                }

                std::copy_n(ed_bytes.begin(), 32, peer_ed_pub_.begin());
                std::string wl_err;
                if (!verify_peer_whitelist(remote_id, peer_ed_pub_, wl_err)) {
                    send_error_and_close(ProtocolErrorCode::UnauthorizedPeer, wl_err);
                    return;
                }

                std::array<std::uint8_t, 64> sig{};
                std::copy_n(sig_bytes.begin(), 64, sig.begin());
                if (!CryptoEngine::ed25519_verify(peer_ed_pub_, transcript_, sig)) {
                    send_error_and_close(ProtocolErrorCode::AuthFailed, "Invalid Ed25519 responder signature");
                    return;
                }

                transcript_.insert(transcript_.end(), raw_payload.begin(), raw_payload.end());
                std::array<std::uint8_t, 32> peer_eph{};
                std::copy_n(eph_bytes.begin(), 32, peer_eph.begin());
                derive_session_keys(peer_eph, true);

                std::array<std::uint8_t, 64> my_sig{};
                CryptoEngine::ed25519_sign(owner_.local_keys_.ed25519_private, transcript_, my_sig);

                nlohmann::json fin = {
                    {"sig_hex", CryptoEngine::bytes_to_hex(my_sig.data(), my_sig.size())},
                    {"node_id", owner_.local_keys_.node_id}
                };
                {
                    std::lock_guard<std::mutex> lock(meta_mutex_);
                    peer_node_id_ = remote_id;
                    std::error_code ec;
                    const auto ep = socket_.remote_endpoint(ec);
                    if (!ec) remote_ep_ = ep.address().to_string() + ":" + std::to_string(ep.port());
                }
                send_frame(FrameType::HandshakeFinish, 0, str_to_bytes(fin.dump()), true);
                state_ = State::Established;
                established_.store(true);
                owner_.register_authenticated_session(remote_id, shared_from_this());
            } catch (...) {
                send_error_and_close(ProtocolErrorCode::AuthFailed, "Malformed HandshakeResp");
            }
            return;
        }

        if (hdr.type == FrameType::HandshakeFinish && state_ == State::HandshakeFinishWait) {
            std::vector<std::uint8_t> decrypted;
            ProtocolErrorCode perr = ProtocolErrorCode::AuthFailed;
            if (!ProtocolCodec::decode_payload(hdr, raw_payload, &session_keys_, decrypted, perr)) {
                send_error_and_close(perr, "Failed to decrypt HandshakeFinish");
                return;
            }
            try {
                const auto j = nlohmann::json::parse(bytes_to_str(decrypted));
                const auto sig_bytes = CryptoEngine::hex_to_bytes(j.at("sig_hex").get<std::string>());
                if (sig_bytes.size() != 64) {
                    send_error_and_close(ProtocolErrorCode::AuthFailed, "Invalid initiator sig size");
                    return;
                }
                std::array<std::uint8_t, 64> sig{};
                std::copy_n(sig_bytes.begin(), 64, sig.begin());
                if (!CryptoEngine::ed25519_verify(peer_ed_pub_, transcript_, sig)) {
                    send_error_and_close(ProtocolErrorCode::AuthFailed, "Invalid Ed25519 initiator signature");
                    return;
                }
                state_ = State::Established;
                established_.store(true);
                owner_.register_authenticated_session(peer_node_id(), shared_from_this());
            } catch (...) {
                send_error_and_close(ProtocolErrorCode::AuthFailed, "Malformed HandshakeFinish");
            }
            return;
        }

        send_error_and_close(ProtocolErrorCode::AuthFailed, "Unexpected handshake state transition");
    }

    void read_next_header() {
        auto self = shared_from_this();
        asio::async_read(
            socket_,
            asio::buffer(read_hdr_buf_),
            asio::bind_executor(strand_, [this, self](std::error_code ec, std::size_t) {
                if (ec) {
                    close_internal();
                    return;
                }
                ProtocolErrorCode err = ProtocolErrorCode::InvalidMagic;
                auto hdr_opt = ProtocolCodec::decode_header(read_hdr_buf_.data(), read_hdr_buf_.size(), err);
                if (!hdr_opt) {
                    send_error_and_close(err, "Header validation or CRC32C failed");
                    return;
                }
                current_hdr_ = *hdr_opt;
                read_payload_buf_.resize(current_hdr_.payload_len);
                if (current_hdr_.payload_len == 0) {
                    on_frame_received(current_hdr_, read_payload_buf_);
                    read_next_header();
                    return;
                }
                asio::async_read(
                    socket_,
                    asio::buffer(read_payload_buf_),
                    asio::bind_executor(strand_, [this, self](std::error_code pec, std::size_t bytes_read) {
                        if (pec) {
                            close_internal();
                            return;
                        }
                        owner_.frames_received_.fetch_add(1);
                        owner_.bytes_received_.fetch_add(kFrameHeaderSize + bytes_read);
                        last_activity_ms_.store(core::now_ms());
                        on_frame_received(current_hdr_, read_payload_buf_);
                        if (state_ != State::Closed) {
                            read_next_header();
                        }
                    }));
            }));
    }

    void on_frame_received(const FrameHeader& hdr, const std::vector<std::uint8_t>& raw_payload) {
        if (hdr.type == FrameType::FrameError) {
            close_internal();
            return;
        }
        if (state_ != State::Established) {
            handle_handshake_frame(hdr, raw_payload);
            return;
        }

        std::vector<std::uint8_t> plaintext;
        ProtocolErrorCode err = ProtocolErrorCode::DecryptFailed;
        if (!ProtocolCodec::decode_payload(hdr, raw_payload, &session_keys_, plaintext, err)) {
            send_error_and_close(err, "Frame AEAD/Zstd decode failed");
            return;
        }

        if (hdr.type == FrameType::HeartbeatPing) {
            try {
                const auto j = nlohmann::json::parse(bytes_to_str(plaintext));
                nlohmann::json pong = {
                    {"sender_ts_us", j.value("sender_ts_us", 0ULL)},
                    {"node_id", owner_.local_keys_.node_id}
                };
                send_frame(FrameType::HeartbeatPong, 0, str_to_bytes(pong.dump()), true);
            } catch (...) {
            }
            return;
        }

        if (hdr.type == FrameType::HeartbeatPong) {
            try {
                const auto j = nlohmann::json::parse(bytes_to_str(plaintext));
                const std::uint64_t sent_us = j.value("sender_ts_us", 0ULL);
                if (sent_us > 0) {
                    const std::uint64_t cur_us = core::now_us();
                    if (cur_us >= sent_us) {
                        const double rtt = static_cast<double>(cur_us - sent_us) / 1000.0;
                        std::lock_guard<std::mutex> lock(meta_mutex_);
                        rtt_ms_ = rtt;
                    }
                }
            } catch (...) {
            }
            return;
        }

        owner_.handle_decoded_frame(peer_node_id(), shared_from_this(), hdr, plaintext);
    }

    void do_write() {
        if (write_queue_.empty()) {
            writing_ = false;
            if (close_after_write_) {
                close_internal();
            }
            return;
        }
        writing_ = true;
        auto self = shared_from_this();
        const std::size_t frame_sz = write_queue_.front().size();
        owner_.rate_limiter_.consume_or_wait(frame_sz, std::chrono::milliseconds(50));

        asio::async_write(
            socket_,
            asio::buffer(write_queue_.front()),
            asio::bind_executor(strand_, [this, self](std::error_code ec, std::size_t bytes_written) {
                if (ec) {
                    close_internal();
                    return;
                }
                owner_.frames_sent_.fetch_add(1);
                owner_.bytes_sent_.fetch_add(bytes_written);
                write_queue_.pop_front();
                do_write();
            }));
    }

    MeshTransport& owner_;
    asio::ip::tcp::socket socket_;
    asio::strand<asio::io_context::executor_type> strand_;
    bool is_initiator_;
    State state_;
    std::atomic<bool> established_{false};

    mutable std::mutex meta_mutex_;
    std::string peer_node_id_;
    std::string remote_ep_;
    double rtt_ms_{-1.0};
    std::atomic<std::int64_t> last_activity_ms_{0};

    EphemeralX25519 eph_;
    std::array<std::uint8_t, 32> peer_ed_pub_{};
    std::vector<std::uint8_t> transcript_;
    SessionKeys session_keys_;

    std::array<std::uint8_t, kFrameHeaderSize> read_hdr_buf_{};
    FrameHeader current_hdr_;
    std::vector<std::uint8_t> read_payload_buf_;

    std::unordered_map<std::uint32_t, std::uint32_t> stream_seq_;
    std::deque<std::vector<std::uint8_t>> write_queue_;
    bool writing_{false};
    bool close_after_write_{false};
};

MeshTransport::MeshTransport(core::ConfigManager& config_mgr,
                             storage::SqliteStore& store,
                             core::HybridLogicalClock& hlc,
                             NodeKeyPair local_keys)
    : config_mgr_(config_mgr),
      store_(store),
      hlc_(hlc),
      local_keys_(std::move(local_keys)),
      rate_limiter_(config_mgr.snapshot().network.bandwidth_limit_kbps) {}

MeshTransport::~MeshTransport() {
    stop();
}

void MeshTransport::set_event_callback(EventCallback cb) {
    std::lock_guard<std::mutex> lock(cb_mutex_);
    event_callback_ = std::move(cb);
}

void MeshTransport::emit_event(const core::SyncEventNotification& ev) {
    EventCallback cb;
    {
        std::lock_guard<std::mutex> lock(cb_mutex_);
        cb = event_callback_;
    }
    if (cb) cb(ev);
}

void MeshTransport::notify_config_reloaded() {
    const auto cfg = config_mgr_.snapshot();
    rate_limiter_.set_limit_kbps(cfg.network.bandwidth_limit_kbps);
    if (running_.load()) {
        for (const auto& peer : cfg.peers) {
            if (peer.node_id == local_keys_.node_id || peer.address.empty()) continue;
            if (cfg.network.topology == core::TopologyMode::HubAndSpoke &&
                cfg.network.role == core::NodeRole::Peer &&
                !peer.is_relay) {
                continue;
            }
            connect_to_peer(peer);
        }
    }
}

bool MeshTransport::start(std::string& error_out) {
    if (running_.load()) return true;
    const auto cfg = config_mgr_.snapshot();

    try {
        io_ctx_.restart();
        work_guard_ = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(
            asio::make_work_guard(io_ctx_));

        const auto bind_ip = asio::ip::make_address(cfg.network.listen_address);
        asio::ip::tcp::endpoint ep(bind_ip, cfg.network.listen_port);
        acceptor_ = std::make_unique<asio::ip::tcp::acceptor>(io_ctx_);
        acceptor_->open(ep.protocol());
        acceptor_->set_option(asio::ip::tcp::acceptor::reuse_address(true));
        acceptor_->bind(ep);
        acceptor_->listen(asio::socket_base::max_listen_connections);
        bound_port_.store(acceptor_->local_endpoint().port());

        running_.store(true);
        start_accept();

        if (cfg.network.enable_mdns_discovery) {
            start_discovery_listener();
        }

        maintenance_timer_ = std::make_unique<asio::steady_timer>(io_ctx_);
        schedule_maintenance_timer();

        const std::size_t nthreads = std::max<std::size_t>(2, cfg.network.io_threads);
        io_threads_.reserve(nthreads);
        for (std::size_t i = 0; i < nthreads; ++i) {
            io_threads_.emplace_back([this]() { io_ctx_.run(); });
        }

        // Initiate outbound connections to configured peers
        for (const auto& peer : cfg.peers) {
            if (!peer.address.empty() && peer.node_id != local_keys_.node_id) {
                connect_to_peer(peer);
            }
        }
        return true;
    } catch (const std::exception& ex) {
        error_out = ex.what();
        stop();
        return false;
    }
}

void MeshTransport::stop() {
    if (!running_.exchange(false)) return;

    std::error_code ec;
    if (acceptor_) acceptor_->close(ec);
    if (discovery_socket_) discovery_socket_->close(ec);
    if (maintenance_timer_) maintenance_timer_->cancel();

    std::vector<std::shared_ptr<PeerSession>> to_close;
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        to_close.reserve(active_sessions_.size());
        for (auto& [id, sess] : active_sessions_) {
            if (sess) to_close.push_back(sess);
        }
        active_sessions_.clear();
    }
    for (auto& sess : to_close) {
        sess->close();
    }

    work_guard_.reset();
    io_ctx_.stop();
    for (auto& t : io_threads_) {
        if (t.joinable()) t.join();
    }
    io_threads_.clear();
}

bool MeshTransport::is_running() const noexcept {
    return running_.load();
}

std::uint16_t MeshTransport::bound_port() const noexcept {
    return bound_port_.load();
}

void MeshTransport::start_accept() {
    if (!running_.load()) return;
    acceptor_->async_accept([this](std::error_code ec, asio::ip::tcp::socket sock) {
        if (!ec && running_.load()) {
            auto sess = std::make_shared<PeerSession>(*this, std::move(sock), false);
            sess->start();
        }
        if (running_.load()) {
            start_accept();
        }
    });
}

void MeshTransport::connect_to_peer(const core::PeerConfig& peer) {
    if (!running_.load() || peer.address.empty()) return;
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        auto it = active_sessions_.find(peer.node_id);
        if (it != active_sessions_.end() && it->second && it->second->is_established()) {
            return;
        }
        const auto nb = reconnect_not_before_ms_.find(peer.node_id);
        if (nb != reconnect_not_before_ms_.end() && core::now_ms() < nb->second) {
            return;
        }
    }

    auto resolver = std::make_shared<asio::ip::tcp::resolver>(io_ctx_);
    resolver->async_resolve(
        peer.address,
        std::to_string(peer.port),
        [this, peer, resolver](std::error_code ec, asio::ip::tcp::resolver::results_type results) {
            if (ec || !running_.load()) return;
            auto sock = std::make_shared<asio::ip::tcp::socket>(io_ctx_);
            asio::async_connect(
                *sock,
                results,
                [this, peer, sock](std::error_code conn_ec, const asio::ip::tcp::endpoint&) {
                    if (conn_ec) {
                        std::lock_guard<std::mutex> lock(sessions_mutex_);
                        int& att = reconnect_attempts_[peer.node_id];
                        ++att;
                        const std::int64_t backoff = std::min<std::int64_t>(30000, 1000LL * (1LL << std::min(att, 5)));
                        reconnect_not_before_ms_[peer.node_id] = core::now_ms() + backoff;
                        return;
                    }
                    {
                        std::lock_guard<std::mutex> lock(sessions_mutex_);
                        reconnect_attempts_[peer.node_id] = 0;
                        reconnect_not_before_ms_[peer.node_id] = 0;
                    }
                    auto sess = std::make_shared<PeerSession>(*this, std::move(*sock), true, peer.node_id);
                    sess->start();
                });
        });
}

void MeshTransport::start_discovery_listener() {
    try {
        const auto cfg = config_mgr_.snapshot();
        discovery_socket_ = std::make_unique<asio::ip::udp::socket>(io_ctx_);
        const asio::ip::udp::endpoint listen_ep(asio::ip::udp::v4(), cfg.network.discovery_port);
        discovery_socket_->open(listen_ep.protocol());
        discovery_socket_->set_option(asio::ip::udp::socket::reuse_address(true));
        discovery_socket_->bind(listen_ep);

        const auto mcast_addr = asio::ip::make_address(cfg.network.multicast_group);
        discovery_socket_->set_option(asio::ip::multicast::join_group(mcast_addr));

        auto do_recv = std::make_shared<std::function<void()>>();
        *do_recv = [this, do_recv]() {
            if (!running_.load() || !discovery_socket_) return;
            discovery_socket_->async_receive_from(
                asio::buffer(discovery_recv_buf_),
                discovery_remote_ep_,
                [this, do_recv](std::error_code ec, std::size_t n) {
                    if (!ec && n > 0) {
                        try {
                            const std::string raw(reinterpret_cast<const char*>(discovery_recv_buf_.data()), n);
                            const auto j = nlohmann::json::parse(raw);
                            const std::string peer_id = j.value("node_id", "");
                            const std::uint16_t port = j.value("port", static_cast<std::uint16_t>(0));
                            if (!peer_id.empty() && peer_id != local_keys_.node_id && port > 0) {
                                const auto cfg_snap = config_mgr_.snapshot();
                                for (const auto& p : cfg_snap.peers) {
                                    if (p.node_id == peer_id) {
                                        core::PeerConfig discovered = p;
                                        discovered.address = discovery_remote_ep_.address().to_string();
                                        discovered.port = port;
                                        connect_to_peer(discovered);
                                        break;
                                    }
                                }
                            }
                        } catch (...) {
                        }
                    }
                    if (running_.load()) (*do_recv)();
                });
        };
        (*do_recv)();
    } catch (const std::exception& ex) {
        spdlog::warn("UDP/mDNS LAN discovery disabled on bind: {}", ex.what());
    }
}

void MeshTransport::send_discovery_beacon() {
    if (!discovery_socket_ || !discovery_socket_->is_open()) return;
    const auto cfg = config_mgr_.snapshot();
    nlohmann::json beacon = {
        {"magic", "AGMS_DISC"},
        {"node_id", local_keys_.node_id},
        {"port", bound_port_.load()},
        {"fp", local_keys_.fingerprint_sha256}
    };
    const std::string data = beacon.dump();
    std::error_code ec;
    const asio::ip::udp::endpoint target(
        asio::ip::make_address(cfg.network.multicast_group, ec),
        cfg.network.discovery_port);
    if (!ec) {
        discovery_socket_->send_to(asio::buffer(data), target, 0, ec);
    }
}

void MeshTransport::schedule_maintenance_timer() {
    if (!running_.load() || !maintenance_timer_) return;
    const auto cfg = config_mgr_.snapshot();
    maintenance_timer_->expires_after(std::chrono::milliseconds(cfg.network.heartbeat_interval_ms));
    maintenance_timer_->async_wait([this](std::error_code ec) {
        if (ec || !running_.load()) return;

        const auto cfg_snap = config_mgr_.snapshot();
        const std::int64_t now = core::now_ms();

        // 1. Heartbeats & timeout eviction
        std::vector<std::shared_ptr<PeerSession>> timed_out;
        std::vector<std::shared_ptr<PeerSession>> alive;
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            for (const auto& [id, sess] : active_sessions_) {
                if (!sess) continue;
                if (now - sess->last_activity_ms() > static_cast<std::int64_t>(cfg_snap.network.peer_timeout_ms)) {
                    timed_out.push_back(sess);
                } else {
                    alive.push_back(sess);
                }
            }
        }
        for (auto& s : timed_out) {
            emit_event({"peer_timeout", s->peer_node_id(), "", "Peer heartbeat timed out", now});
            s->close();
        }
        for (auto& s : alive) {
            s->send_heartbeat_ping();
        }

        // 2. Reconnect missing peers (or Relay Hub in Hub-and-Spoke mode)
        for (const auto& p : cfg_snap.peers) {
            if (p.node_id == local_keys_.node_id) continue;
            if (cfg_snap.network.topology == core::TopologyMode::HubAndSpoke &&
                cfg_snap.network.role == core::NodeRole::Peer &&
                !p.is_relay) {
                continue;
            }
            if (!p.address.empty()) {
                connect_to_peer(p);
            }
        }

        // 3. Multicast LAN beacon
        if (cfg_snap.network.enable_mdns_discovery) {
            send_discovery_beacon();
        }

        schedule_maintenance_timer();
    });
}

void MeshTransport::register_authenticated_session(const std::string& peer_node_id,
                                                   std::shared_ptr<PeerSession> session) {
    std::shared_ptr<PeerSession> displaced_session;
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        // Deterministic asymmetric tie-breaker for simultaneous-open connections:
        // lower node_id keeps its initiator socket; higher node_id keeps its responder socket.
        // Because initiator<->responder maps to the exact same TCP connection on both ends,
        // both hosts converge on keeping the identical socket without mutual disconnect loops.
        auto it = active_sessions_.find(peer_node_id);
        if (it != active_sessions_.end() && it->second && it->second != session && it->second->is_established()) {
            const bool keep_initiator = (local_keys_.node_id < peer_node_id);
            const bool incoming_wins = (session->is_initiator() == keep_initiator);
            if (!incoming_wins) {
                displaced_session = session;
            } else {
                displaced_session = it->second;
                active_sessions_[peer_node_id] = session;
            }
        } else {
            active_sessions_[peer_node_id] = session;
        }
    }
    if (displaced_session) {
        displaced_session->close();
        if (displaced_session == session) {
            return;
        }
    }

    const auto now = core::now_ms();
    core::NodeInfo n;
    n.node_id = peer_node_id;
    n.hostname = peer_node_id;
    n.endpoint = session->remote_endpoint_str();
    n.online_status = core::OnlineStatus::Online;
    n.last_seen_ms = now;
    n.hlc = hlc_.tick();
    store_.upsert_node(n);

    emit_event({"peer_connected", peer_node_id, "", "Authenticated mTLS/Noise session established", now});

    // Send local node metadata + anti-entropy sync clock request + drain outbox
    const auto cfg = config_mgr_.snapshot();
    nlohmann::json hello = {
        {"node_id", cfg.node_id},
        {"hostname", cfg.hostname},
        {"os_name", core::ConfigManager::detect_os_name()},
        {"arch", core::ConfigManager::detect_arch()},
        {"tags", cfg.tags},
        {"pubkey_fp", local_keys_.fingerprint_sha256},
        {"hlc", hlc_to_json(hlc_.tick())}
    };
    session->send_frame(FrameType::NodeHelloMeta, 1, str_to_bytes(hello.dump()));

    const auto vc = store_.get_peer_vector_clock(peer_node_id);
    nlohmann::json sync_req = {
        {"since_hlc_ms", 0},
        {"vector_clock", core::VectorClockOps::to_json(vc)}
    };
    session->send_frame(FrameType::SyncClockReq, 0, str_to_bytes(sync_req.dump()), true);

    flush_outbox_for_peer(peer_node_id, session);

    // Transitive gossip synchronization: advertise known cluster peers to the newly connected peer
    const std::int64_t g_ts = core::now_ms();
    for (const auto& kp : cfg.peers) {
        if (kp.node_id.empty() || kp.node_id == peer_node_id || kp.node_id == local_keys_.node_id) continue;
        if (kp.pubkey_fingerprint.empty() || kp.address.empty()) continue;
        const auto sign_in = build_peer_gossip_sign_input(local_keys_.node_id, kp, g_ts);
        std::array<std::uint8_t, 64> sig{};
        CryptoEngine::ed25519_sign(local_keys_.ed25519_private, sign_in, sig);
        const std::string g_json = nlohmann::json{
            {"introducer_node_id", local_keys_.node_id},
            {"introducer_pubkey_hex", local_keys_.public_key_hex},
            {"ts_ms", g_ts},
            {"sig_hex", CryptoEngine::bytes_to_hex(sig.data(), sig.size())},
            {"peer", {
                {"node_id", kp.node_id},
                {"address", kp.address},
                {"port", kp.port},
                {"pubkey_fingerprint", kp.pubkey_fingerprint},
                {"ed25519_pubkey_hex", kp.ed25519_pubkey_hex}
            }}
        }.dump();
        session->send_frame(FrameType::PeerGossip, 1, str_to_bytes(g_json));
    }
}

void MeshTransport::unregister_session(const std::string& peer_node_id, PeerSession* session_ptr) {
    bool removed = false;
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        auto it = active_sessions_.find(peer_node_id);
        if (it != active_sessions_.end() && it->second.get() == session_ptr) {
            active_sessions_.erase(it);
            removed = true;
        }
    }
    if (removed) {
        core::NodeInfo n;
        n.node_id = peer_node_id;
        n.hostname = peer_node_id;
        n.online_status = core::OnlineStatus::Offline;
        n.last_seen_ms = core::now_ms();
        n.hlc = hlc_.tick();
        store_.upsert_node(n);
        emit_event({"peer_disconnected", peer_node_id, "", "Peer disconnected", core::now_ms()});
    }
}

void MeshTransport::flush_outbox_for_peer(const std::string& peer_node_id,
                                          const std::shared_ptr<PeerSession>& session) {
    const auto due = store_.fetch_due_outbox(peer_node_id, core::now_ms(), 100);
    for (const auto& ev : due) {
        session->send_frame(static_cast<FrameType>(ev.frame_type), 1, str_to_bytes(ev.payload_json));
        store_.mark_outbox_delivered(ev.event_id);
    }
}

void MeshTransport::handle_decoded_frame(const std::string& peer_node_id,
                                         const std::shared_ptr<PeerSession>& session,
                                         const FrameHeader& header,
                                         const std::vector<std::uint8_t>& payload) {
    const std::string text = bytes_to_str(payload);

    if (header.type == FrameType::NodeHelloMeta) {
        try {
            const auto j = nlohmann::json::parse(text);
            core::NodeInfo n;
            n.node_id = j.value("node_id", peer_node_id);
            n.hostname = j.value("hostname", peer_node_id);
            n.os_name = j.value("os_name", "unknown");
            n.arch = j.value("arch", "unknown");
            n.tags = j.value("tags", std::vector<std::string>{});
            n.pubkey_fingerprint = j.value("pubkey_fp", "");
            n.endpoint = session->remote_endpoint_str();
            n.online_status = core::OnlineStatus::Online;
            n.rtt_ms = session->last_rtt_ms();
            n.last_seen_ms = core::now_ms();
            if (j.contains("hlc")) {
                const auto rh = json_to_hlc(j["hlc"]);
                hlc_.witness(rh);
                n.hlc = rh;
            } else {
                n.hlc = hlc_.tick();
            }
            store_.upsert_node(n);
        } catch (...) {
        }
        return;
    }

    if (header.type == FrameType::SyncClockReq) {
        try {
            const auto j = nlohmann::json::parse(text);
            const std::uint64_t since_ms = j.value("since_hlc_ms", 0ULL);
            const auto sessions = store_.get_sessions_since(since_ms, 200);
            const auto messages = store_.get_messages_since(since_ms, 500);
            const auto summaries = store_.get_summaries_since(since_ms, 200);
            const auto workspaces = store_.get_workspaces_since(since_ms, 200);
            const auto handoffs = store_.get_handoffs_since(since_ms, 100);
            const auto locks = store_.get_locks_since(since_ms, 100);
            const auto tombstones = store_.get_tombstones_since(since_ms, 200);

            if (!sessions.empty() || !messages.empty() || !summaries.empty() ||
                !workspaces.empty() || !handoffs.empty() || !locks.empty() || !tombstones.empty()) {
                const std::string delta_json = build_delta_json(
                    sessions, messages, summaries, workspaces, handoffs, locks, tombstones);
                session->send_frame(FrameType::DeltaEventBatch, 1, str_to_bytes(delta_json));
            }
        } catch (...) {
        }
        return;
    }

    if (header.type == FrameType::DeltaEventBatch ||
        header.type == FrameType::HandoffPacket ||
        header.type == FrameType::LockOp ||
        header.type == FrameType::TombstonePurge) {
        apply_incoming_delta_json(peer_node_id, text);
        last_replication_ms_.store(core::now_ms());
        return;
    }

    if (header.type == FrameType::PeerGossip) {
        try {
            const auto g = nlohmann::json::parse(text);
            const std::string introducer_id = g.at("introducer_node_id").get<std::string>();
            if (introducer_id != peer_node_id) return;

            const auto intro_ed = CryptoEngine::hex_to_bytes(g.at("introducer_pubkey_hex").get<std::string>());
            const auto sig_bytes = CryptoEngine::hex_to_bytes(g.at("sig_hex").get<std::string>());
            if (intro_ed.size() != 32 || sig_bytes.size() != 64) return;

            const auto cfg = config_mgr_.snapshot();
            bool trusted_introducer = false;
            const std::string intro_fp = CryptoEngine::bytes_to_hex(intro_ed.data(), 32);
            for (const auto& p : cfg.peers) {
                if (p.node_id == introducer_id) {
                    std::array<std::uint8_t, 32> tmp_arr{};
                    std::copy_n(intro_ed.begin(), 32, tmp_arr.begin());
                    const std::string computed_fp = CryptoEngine::compute_fingerprint(tmp_arr);
                    if (p.pubkey_fingerprint == computed_fp || p.ed25519_pubkey_hex == intro_fp) {
                        trusted_introducer = true;
                        break;
                    }
                }
            }
            if (!trusted_introducer) return;

            const auto& pj = g.at("peer");
            core::PeerConfig np;
            np.node_id = pj.at("node_id").get<std::string>();
            np.address = pj.value("address", "");
            np.port = pj.value("port", static_cast<std::uint16_t>(47890));
            np.pubkey_fingerprint = pj.at("pubkey_fingerprint").get<std::string>();
            np.ed25519_pubkey_hex = pj.value("ed25519_pubkey_hex", "");
            const std::int64_t ts_ms = g.at("ts_ms").get<std::int64_t>();

            if (np.node_id.empty() || np.node_id == local_keys_.node_id) return;

            std::array<std::uint8_t, 32> intro_ed_arr{};
            std::array<std::uint8_t, 64> sig_arr{};
            std::copy_n(intro_ed.begin(), 32, intro_ed_arr.begin());
            std::copy_n(sig_bytes.begin(), 64, sig_arr.begin());
            const auto sign_in = build_peer_gossip_sign_input(introducer_id, np, ts_ms);
            if (!CryptoEngine::ed25519_verify(intro_ed_arr, sign_in, sig_arr)) {
                return;
            }

            bool already_known = false;
            for (const auto& ep : cfg.peers) {
                if (ep.node_id == np.node_id &&
                    ep.address == np.address &&
                    ep.port == np.port &&
                    ep.pubkey_fingerprint == np.pubkey_fingerprint) {
                    already_known = true;
                    break;
                }
            }
            if (!already_known) {
                std::string perr;
                if (config_mgr_.add_or_update_peer(np, perr)) {
                    {
                        std::lock_guard<std::mutex> lock(sessions_mutex_);
                        reconnect_attempts_[np.node_id] = 0;
                        reconnect_not_before_ms_[np.node_id] = 0;
                    }
                    emit_event({"peer_gossip_added", np.node_id, "", "Added peer via gossip from " + introducer_id, core::now_ms()});
                    if (!np.address.empty()) {
                        connect_to_peer(np);
                    }
                }
            }
        } catch (...) {
        }
        return;
    }

    if (header.type == FrameType::RelayEnvelope) {
        // Hub-and-Spoke zero-knowledge encrypted envelope routing
        try {
            const auto env = nlohmann::json::parse(text);
            const std::string target_id = env.at("target_node_id").get<std::string>();
            const std::string sender_id = env.at("sender_node_id").get<std::string>();
            const auto sealed_bytes = CryptoEngine::hex_to_bytes(env.at("sealed_hex").get<std::string>());

            if (target_id == local_keys_.node_id) {
                // Decrypt end-to-end sealed payload using shared cluster key derived between sender & receiver
                const auto cfg = config_mgr_.snapshot();
                const auto psk = CryptoEngine::hex_to_bytes(cfg.network.cluster_psk_hex);
                std::vector<std::uint8_t> e2ee_key_vec;
                CryptoEngine::hkdf_sha256(psk, {}, str_to_bytes("ag-mesh-e2ee-relay"), e2ee_key_vec, 32);
                std::array<std::uint8_t, 32> e2ee_key{};
                std::copy_n(e2ee_key_vec.begin(), 32, e2ee_key.begin());

                std::vector<std::uint8_t> inner_plain;
                if (CryptoEngine::aead_decrypt(e2ee_key, str_to_bytes(sender_id + "->" + target_id), sealed_bytes, inner_plain)) {
                    apply_incoming_delta_json(sender_id, bytes_to_str(inner_plain));
                    last_replication_ms_.store(core::now_ms());
                }
            } else {
                // We are a Relay Coordinator: forward opaque ciphertext without decrypting
                const auto cfg = config_mgr_.snapshot();
                if (cfg.network.role == core::NodeRole::RelayHub) {
                    std::shared_ptr<PeerSession> dst_sess;
                    {
                        std::lock_guard<std::mutex> lock(sessions_mutex_);
                        auto it = active_sessions_.find(target_id);
                        if (it != active_sessions_.end()) dst_sess = it->second;
                    }
                    if (dst_sess && dst_sess->is_established()) {
                        dst_sess->send_frame(FrameType::RelayEnvelope, 1, payload);
                        relay_forwarded_.fetch_add(1);
                    } else {
                        core::OutboxEvent ev;
                        ev.event_id = "relay-" + core::CrdtResolver::sha256_hex(text).substr(0, 16);
                        ev.target_node_id = target_id;
                        ev.frame_type = static_cast<std::uint8_t>(FrameType::RelayEnvelope);
                        ev.payload_json = text;
                        ev.created_at_ms = core::now_ms();
                        ev.next_retry_ms = ev.created_at_ms;
                        store_.enqueue_outbox(ev);
                    }
                }
            }
        } catch (...) {
        }
    }
}

void MeshTransport::apply_incoming_delta_json(const std::string& peer_node_id, const std::string& json_str) {
    try {
        const auto j = nlohmann::json::parse(json_str);
        int applied = 0;

        if (j.contains("tombstones") && j["tombstones"].is_array()) {
            for (const auto& item : j["tombstones"]) {
                auto t = json_to_tombstone(item);
                hlc_.witness(t.hlc);
                store_.purge_entity(t);
                ++applied;
            }
        }
        if (j.contains("workspaces") && j["workspaces"].is_array()) {
            for (const auto& item : j["workspaces"]) {
                auto w = json_to_workspace(item);
                hlc_.witness(w.hlc);
                store_.upsert_workspace(w);
                ++applied;
            }
        }
        if (j.contains("sessions") && j["sessions"].is_array()) {
            for (const auto& item : j["sessions"]) {
                auto s = json_to_session(item);
                hlc_.witness(s.hlc);
                store_.upsert_session(s);
                ++applied;
            }
        }
        if (j.contains("messages") && j["messages"].is_array()) {
            for (const auto& item : j["messages"]) {
                auto m = json_to_message(item);
                hlc_.witness(m.hlc);
                bool is_new = false;
                store_.append_message(m, is_new);
                if (is_new) ++applied;
            }
        }
        if (j.contains("summaries") && j["summaries"].is_array()) {
            for (const auto& item : j["summaries"]) {
                auto sm = json_to_summary(item);
                hlc_.witness(sm.hlc);
                store_.upsert_summary(sm);
                ++applied;
            }
        }
        if (j.contains("handoffs") && j["handoffs"].is_array()) {
            for (const auto& item : j["handoffs"]) {
                auto h = json_to_handoff(item);
                hlc_.witness(h.hlc);
                store_.upsert_handoff(h);
                emit_event({"handoff_sync", peer_node_id, h.session_id, h.task_description, core::now_ms()});
                ++applied;
            }
        }
        if (j.contains("locks") && j["locks"].is_array()) {
            for (const auto& item : j["locks"]) {
                auto l = json_to_lock(item);
                hlc_.witness(l.hlc);
                store_.upsert_lock(l);
                emit_event({"lock_sync", peer_node_id, l.owner_session_id, l.lock_key, core::now_ms()});
                ++applied;
            }
        }

        if (applied > 0) {
            auto vc = store_.get_peer_vector_clock(peer_node_id);
            core::VectorClockOps::increment(vc, peer_node_id);
            store_.update_sync_cursor(peer_node_id, hlc_.current(), vc, core::now_ms());
            emit_event({"delta_replicated", peer_node_id, "", "Applied " + std::to_string(applied) + " CRDT updates", core::now_ms()});
        }
    } catch (const std::exception& ex) {
        spdlog::warn("Failed to parse incoming delta JSON from {}: {}", peer_node_id, ex.what());
    }
}

void MeshTransport::broadcast_delta_batch(const std::vector<core::SessionRecord>& sessions,
                                          const std::vector<core::MessageRecord>& messages,
                                          const std::vector<core::SessionSummary>& summaries,
                                          const std::vector<core::WorkspaceInfo>& workspaces,
                                          const std::vector<core::HandoffPacket>& handoffs,
                                          const std::vector<core::DistributedLock>& locks,
                                          const std::vector<core::TombstoneRecord>& tombstones) {
    if (sessions.empty() && messages.empty() && summaries.empty() &&
        workspaces.empty() && handoffs.empty() && locks.empty() && tombstones.empty()) {
        return;
    }

    const std::string delta_json = build_delta_json(
        sessions, messages, summaries, workspaces, handoffs, locks, tombstones);
    const auto cfg = config_mgr_.snapshot();

    std::unordered_map<std::string, std::shared_ptr<PeerSession>> current_sessions;
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        current_sessions = active_sessions_;
    }

    // In Hub-and-Spoke peer mode, wrap end-to-end encrypted relay envelopes for each remote spoke via Hub
    if (cfg.network.topology == core::TopologyMode::HubAndSpoke &&
        cfg.network.role == core::NodeRole::Peer) {
        std::shared_ptr<PeerSession> relay_sess;
        for (const auto& p : cfg.peers) {
            if (p.is_relay) {
                auto it = current_sessions.find(p.node_id);
                if (it != current_sessions.end() && it->second && it->second->is_established()) {
                    relay_sess = it->second;
                    break;
                }
            }
        }
        const auto psk = CryptoEngine::hex_to_bytes(cfg.network.cluster_psk_hex);
        std::vector<std::uint8_t> e2ee_key_vec;
        CryptoEngine::hkdf_sha256(psk, {}, str_to_bytes("ag-mesh-e2ee-relay"), e2ee_key_vec, 32);
        std::array<std::uint8_t, 32> e2ee_key{};
        std::copy_n(e2ee_key_vec.begin(), 32, e2ee_key.begin());

        for (const auto& p : cfg.peers) {
            if (p.node_id == local_keys_.node_id || p.is_relay) continue;
            std::vector<std::uint8_t> sealed;
            CryptoEngine::aead_encrypt(
                e2ee_key,
                static_cast<std::uint64_t>(core::now_us()),
                str_to_bytes(local_keys_.node_id + "->" + p.node_id),
                str_to_bytes(delta_json),
                sealed);
            nlohmann::json env = {
                {"sender_node_id", local_keys_.node_id},
                {"target_node_id", p.node_id},
                {"sealed_hex", CryptoEngine::bytes_to_hex(sealed.data(), sealed.size())}
            };
            const std::string env_str = env.dump();
            if (relay_sess) {
                relay_sess->send_frame(FrameType::RelayEnvelope, 1, str_to_bytes(env_str));
            } else {
                core::OutboxEvent ev;
                ev.event_id = "out-" + p.node_id + "-" + core::CrdtResolver::sha256_hex(env_str).substr(0, 12);
                ev.target_node_id = p.node_id;
                ev.frame_type = static_cast<std::uint8_t>(FrameType::RelayEnvelope);
                ev.payload_json = env_str;
                ev.created_at_ms = core::now_ms();
                ev.next_retry_ms = ev.created_at_ms;
                store_.enqueue_outbox(ev);
            }
        }
        last_replication_ms_.store(core::now_ms());
        return;
    }

    // P2P Mesh mode: send directly to connected peers and persist to disk outbox for offline peers
    for (const auto& p : cfg.peers) {
        if (p.node_id == local_keys_.node_id) continue;
        auto it = current_sessions.find(p.node_id);
        if (it != current_sessions.end() && it->second && it->second->is_established()) {
            it->second->send_frame(FrameType::DeltaEventBatch, 1, str_to_bytes(delta_json));
        } else {
            core::OutboxEvent ev;
            ev.event_id = "out-" + p.node_id + "-" + core::CrdtResolver::sha256_hex(delta_json).substr(0, 12);
            ev.target_node_id = p.node_id;
            ev.frame_type = static_cast<std::uint8_t>(FrameType::DeltaEventBatch);
            ev.payload_json = delta_json;
            ev.created_at_ms = core::now_ms();
            ev.next_retry_ms = ev.created_at_ms;
            store_.enqueue_outbox(ev);
        }
    }
    last_replication_ms_.store(core::now_ms());
}

void MeshTransport::broadcast_local_hello() {
    const auto cfg = config_mgr_.snapshot();
    nlohmann::json hello = {
        {"node_id", cfg.node_id},
        {"hostname", cfg.hostname},
        {"os_name", core::ConfigManager::detect_os_name()},
        {"arch", core::ConfigManager::detect_arch()},
        {"tags", cfg.tags},
        {"pubkey_fp", local_keys_.fingerprint_sha256},
        {"hlc", hlc_to_json(hlc_.tick())}
    };
    const auto bytes = str_to_bytes(hello.dump());
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    for (auto& [id, sess] : active_sessions_) {
        if (sess && sess->is_established()) {
            sess->send_frame(FrameType::NodeHelloMeta, 1, bytes);
        }
    }
}

void MeshTransport::trigger_sync_all() {
    std::unordered_map<std::string, std::shared_ptr<PeerSession>> snap;
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        snap = active_sessions_;
    }
    for (auto& [id, sess] : snap) {
        if (sess && sess->is_established()) {
            nlohmann::json req = {{"since_hlc_ms", 0}};
            sess->send_frame(FrameType::SyncClockReq, 0, str_to_bytes(req.dump()), true);
            flush_outbox_for_peer(id, sess);
        }
    }
}

std::vector<core::NodeInfo> MeshTransport::connected_peers() const {
    std::vector<core::NodeInfo> out;
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    for (const auto& [id, sess] : active_sessions_) {
        if (!sess || !sess->is_established()) continue;
        core::NodeInfo n;
        n.node_id = id;
        n.hostname = id;
        n.endpoint = sess->remote_endpoint_str();
        n.online_status = core::OnlineStatus::Online;
        n.rtt_ms = sess->last_rtt_ms();
        n.last_seen_ms = sess->last_activity_ms();
        out.push_back(std::move(n));
    }
    return out;
}

TransportMetrics MeshTransport::metrics() const {
    TransportMetrics m;
    m.frames_sent = frames_sent_.load();
    m.frames_received = frames_received_.load();
    m.bytes_sent = bytes_sent_.load();
    m.bytes_received = bytes_received_.load();
    m.auth_failures = auth_failures_.load();
    m.relay_packets_forwarded = relay_forwarded_.load();
    m.last_replication_ms = last_replication_ms_.load();
    return m;
}

bool MeshTransport::probe_peer_tcp(const std::string& host,
                                   std::uint16_t port,
                                   double& rtt_ms_out,
                                   std::string& err) {
    try {
        asio::io_context tmp_io;
        asio::ip::tcp::resolver resolver(tmp_io);
        const auto eps = resolver.resolve(host, std::to_string(port));
        asio::ip::tcp::socket sock(tmp_io);
        const auto t0 = std::chrono::steady_clock::now();
        asio::connect(sock, eps);
        const auto t1 = std::chrono::steady_clock::now();
        rtt_ms_out = std::chrono::duration<double, std::milli>(t1 - t0).count();
        return true;
    } catch (const std::exception& ex) {
        err = ex.what();
        return false;
    }
}

PairingWindowStatus MeshTransport::start_pairing_window(
    int ttl_seconds,
    int max_uses,
    std::uint16_t port_override,
    const std::vector<EndpointCandidate>& explicit_endpoints) {
    const std::uint16_t effective_port =
        port_override > 0 ? port_override : (bound_port_.load() > 0 ? bound_port_.load() : config_mgr_.snapshot().network.listen_port);

    std::array<std::uint8_t, 16> raw_secret{};
    RAND_bytes(raw_secret.data(), static_cast<int>(raw_secret.size()));
    const std::string secret_hex = CryptoEngine::bytes_to_hex(raw_secret.data(), raw_secret.size());

    std::uint32_t raw_pin = 0;
    RAND_bytes(reinterpret_cast<std::uint8_t*>(&raw_pin), sizeof(raw_pin));
    const std::uint32_t pin_num = raw_pin % 1000000u;
    std::ostringstream pin_digits_ss;
    pin_digits_ss << std::setw(6) << std::setfill('0') << pin_num;
    const std::string pin_digits = pin_digits_ss.str();
    const std::string pin_formatted = pin_digits.substr(0, 3) + "-" + pin_digits.substr(3, 3);

    std::vector<EndpointCandidate> eps = explicit_endpoints.empty()
                                             ? NetworkDiscovery::discover_local_endpoints(effective_port)
                                             : explicit_endpoints;
    for (auto& ep : eps) {
        if (ep.port == 0) ep.port = effective_port;
    }
    if (eps.empty()) {
        eps.push_back({"127.0.0.1", effective_port, "loopback", "lan", 10});
    }

    PairingInviteToken tok;
    tok.version = 1;
    tok.node_id = local_keys_.node_id;
    tok.fingerprint = local_keys_.fingerprint_sha256;
    tok.endpoints = eps;
    tok.secret = secret_hex;

    const std::string token_uri = ProtocolCodec::encode_invite_token(tok);
    const std::int64_t now = core::now_ms();
    const int clamped_ttl = std::max(1, ttl_seconds);
    const int clamped_uses = std::clamp(max_uses, 1, 10);

    {
        std::lock_guard<std::mutex> lock(pairing_mutex_);
        pairing_window_.active = true;
        pairing_window_.locked_out = false;
        pairing_window_.secret_hex = secret_hex;
        pairing_window_.pin_digits = pin_digits;
        pairing_window_.pin_formatted = pin_formatted;
        pairing_window_.invite_token = token_uri;
        pairing_window_.created_at_ms = now;
        pairing_window_.expires_at_ms = now + static_cast<std::int64_t>(clamped_ttl) * 1000LL;
        pairing_window_.max_uses = clamped_uses;
        pairing_window_.current_uses = 0;
        pairing_window_.failed_attempts = 0;
        pairing_window_.endpoints = std::move(eps);
        pairing_window_.joined_node_ids.clear();
    }

    emit_event({"pairing_window_opened", local_keys_.node_id, "", "Pairing window opened (PIN=" + pin_formatted + ")", now});
    return get_pairing_status();
}

PairingWindowStatus MeshTransport::get_pairing_status() const {
    std::lock_guard<std::mutex> lock(pairing_mutex_);
    PairingWindowStatus st;
    const std::int64_t now = core::now_ms();
    const bool expired = pairing_window_.active && (now > pairing_window_.expires_at_ms);
    st.active = pairing_window_.active && !expired && !pairing_window_.locked_out &&
                (pairing_window_.current_uses < pairing_window_.max_uses);
    st.locked_out = pairing_window_.locked_out;
    st.invite_token = pairing_window_.invite_token;
    st.pin = pairing_window_.pin_formatted;
    st.secret_hex = pairing_window_.secret_hex;
    st.created_at_ms = pairing_window_.created_at_ms;
    st.expires_at_ms = pairing_window_.expires_at_ms;
    st.remaining_ttl_seconds = st.active ? static_cast<int>(std::max<std::int64_t>(0, (pairing_window_.expires_at_ms - now) / 1000LL)) : 0;
    st.max_uses = pairing_window_.max_uses;
    st.current_uses = pairing_window_.current_uses;
    st.failed_attempts = pairing_window_.failed_attempts;
    st.endpoints = pairing_window_.endpoints;
    st.joined_node_ids = pairing_window_.joined_node_ids;
    return st;
}

void MeshTransport::stop_pairing_window() {
    std::lock_guard<std::mutex> lock(pairing_mutex_);
    pairing_window_.active = false;
}

bool MeshTransport::validate_and_accept_pair_request(const std::string& joiner_node_id,
                                                     std::uint16_t joiner_listen_port,
                                                     const std::array<std::uint8_t, 32>& joiner_ed_pub,
                                                     const std::array<std::uint8_t, 32>& joiner_eph_pub,
                                                     const std::string& auth_mode,
                                                     const std::vector<std::uint8_t>& transcript,
                                                     const std::array<std::uint8_t, 32>& recv_hmac,
                                                     const std::array<std::uint8_t, 64>& recv_sig,
                                                     const std::string& remote_ip,
                                                     std::string& welcome_envelope_json_out,
                                                     std::string& reject_reason_out) {
    if (joiner_node_id.empty() || joiner_node_id == local_keys_.node_id) {
        reject_reason_out = "Invalid joiner node_id";
        return false;
    }

    if (!CryptoEngine::ed25519_verify(joiner_ed_pub, transcript, recv_sig)) {
        reject_reason_out = "Invalid joiner Ed25519 signature";
        return false;
    }

    std::vector<std::uint8_t> auth_key;
    {
        std::lock_guard<std::mutex> lock(pairing_mutex_);
        const std::int64_t now = core::now_ms();
        if (!pairing_window_.active) {
            reject_reason_out = "Pairing window is not active";
            return false;
        }
        if (pairing_window_.locked_out || pairing_window_.failed_attempts >= 5) {
            pairing_window_.active = false;
            pairing_window_.locked_out = true;
            reject_reason_out = "Pairing window locked out after 5 failed attempts";
            return false;
        }
        if (now > pairing_window_.expires_at_ms) {
            pairing_window_.active = false;
            reject_reason_out = "Pairing window expired";
            return false;
        }
        if (pairing_window_.current_uses >= pairing_window_.max_uses) {
            pairing_window_.active = false;
            reject_reason_out = "Pairing window max uses reached";
            return false;
        }

        const std::string& credential = (auth_mode == "pin") ? pairing_window_.pin_digits : pairing_window_.secret_hex;
        auth_key = derive_pair_auth_key(credential);
        const auto expected_hmac = CryptoEngine::hmac_sha256(auth_key, transcript);
        if (!CryptoEngine::constant_time_equal(expected_hmac.data(), recv_hmac.data(), 32)) {
            ++pairing_window_.failed_attempts;
            auth_failures_.fetch_add(1);
            if (pairing_window_.failed_attempts >= 5) {
                pairing_window_.locked_out = true;
                pairing_window_.active = false;
                reject_reason_out = "Invalid pairing secret/PIN; window locked after 5 failed attempts";
            } else {
                reject_reason_out = "Invalid pairing secret/PIN (" +
                                    std::to_string(5 - pairing_window_.failed_attempts) + " attempts remaining)";
            }
            return false;
        }

        ++pairing_window_.current_uses;
        if (pairing_window_.current_uses >= pairing_window_.max_uses) {
            pairing_window_.active = false;
        }
        pairing_window_.joined_node_ids.push_back(joiner_node_id);
    }

    // Ensure cluster PSK exists
    auto cfg = config_mgr_.snapshot();
    std::string psk_hex = cfg.network.cluster_psk_hex;
    if (psk_hex.size() != 64) {
        psk_hex = CryptoEngine::generate_random_psk_hex();
        std::string psk_err;
        config_mgr_.apply_joined_cluster_config(psk_hex, cfg.peers, psk_err);
        cfg = config_mgr_.snapshot();
    }

    core::PeerConfig new_peer;
    new_peer.node_id = joiner_node_id;
    new_peer.address = remote_ip;
    new_peer.port = joiner_listen_port > 0 ? joiner_listen_port : 47890;
    new_peer.pubkey_fingerprint = CryptoEngine::compute_fingerprint(joiner_ed_pub);
    new_peer.ed25519_pubkey_hex = CryptoEngine::bytes_to_hex(joiner_ed_pub.data(), 32);

    std::string cfg_err;
    if (!config_mgr_.add_or_update_peer(new_peer, cfg_err)) {
        reject_reason_out = "Failed to persist new peer: " + cfg_err;
        return false;
    }

    cfg = config_mgr_.snapshot();
    nlohmann::json peers_arr = nlohmann::json::array();
    const std::uint16_t my_port = bound_port_.load() > 0 ? bound_port_.load() : cfg.network.listen_port;
    peers_arr.push_back({
        {"node_id", local_keys_.node_id},
        {"address", ""},
        {"port", my_port},
        {"pubkey_fingerprint", local_keys_.fingerprint_sha256},
        {"ed25519_pubkey_hex", local_keys_.public_key_hex}
    });
    for (const auto& p : cfg.peers) {
        if (p.node_id == joiner_node_id || p.node_id == local_keys_.node_id) continue;
        peers_arr.push_back({
            {"node_id", p.node_id},
            {"address", p.address},
            {"port", p.port},
            {"pubkey_fingerprint", p.pubkey_fingerprint},
            {"ed25519_pubkey_hex", p.ed25519_pubkey_hex},
            {"is_relay", p.is_relay}
        });
    }

    const nlohmann::json welcome_plain = {
        {"host_node_id", local_keys_.node_id},
        {"host_fingerprint", local_keys_.fingerprint_sha256},
        {"host_ed25519_pub_hex", local_keys_.public_key_hex},
        {"cluster_psk_hex", psk_hex},
        {"peers", std::move(peers_arr)}
    };

    const auto host_eph = CryptoEngine::generate_x25519_ephemeral();
    const auto pair_key = derive_pair_welcome_key(host_eph.priv, joiner_eph_pub, auth_key);
    const std::string host_eph_hex = CryptoEngine::bytes_to_hex(host_eph.pub.data(), 32);
    const std::string joiner_eph_hex = CryptoEngine::bytes_to_hex(joiner_eph_pub.data(), 32);
    const auto aad = str_to_bytes(host_eph_hex + "|" + local_keys_.public_key_hex);

    std::vector<std::uint8_t> sealed_welcome;
    if (!CryptoEngine::aead_encrypt(pair_key, 1, aad, str_to_bytes(welcome_plain.dump()), sealed_welcome)) {
        reject_reason_out = "Failed to encrypt PAIR_WELCOME payload";
        return false;
    }

    const std::string sealed_hex = CryptoEngine::bytes_to_hex(sealed_welcome.data(), sealed_welcome.size());
    const auto sig_input = str_to_bytes(host_eph_hex + "|" + joiner_eph_hex + "|" + sealed_hex);
    std::array<std::uint8_t, 64> sig_h{};
    CryptoEngine::ed25519_sign(local_keys_.ed25519_private, sig_input, sig_h);

    welcome_envelope_json_out = nlohmann::json{
        {"host_eph_pub_hex", host_eph_hex},
        {"host_ed25519_pub_hex", local_keys_.public_key_hex},
        {"sealed_welcome_hex", sealed_hex},
        {"sig_hex", CryptoEngine::bytes_to_hex(sig_h.data(), sig_h.size())}
    }.dump();

    emit_event({"peer_paired", joiner_node_id, "", "Paired new host " + joiner_node_id + " (" + remote_ip + ")", core::now_ms()});
    broadcast_peer_gossip(new_peer);
    notify_config_reloaded();
    return true;
}

void MeshTransport::broadcast_peer_gossip(const core::PeerConfig& new_peer) {
    const std::int64_t ts_ms = core::now_ms();
    const auto sign_in = build_peer_gossip_sign_input(local_keys_.node_id, new_peer, ts_ms);
    std::array<std::uint8_t, 64> sig{};
    CryptoEngine::ed25519_sign(local_keys_.ed25519_private, sign_in, sig);

    const std::string gossip_json = nlohmann::json{
        {"introducer_node_id", local_keys_.node_id},
        {"introducer_pubkey_hex", local_keys_.public_key_hex},
        {"ts_ms", ts_ms},
        {"sig_hex", CryptoEngine::bytes_to_hex(sig.data(), sig.size())},
        {"peer", {
            {"node_id", new_peer.node_id},
            {"address", new_peer.address},
            {"port", new_peer.port},
            {"pubkey_fingerprint", new_peer.pubkey_fingerprint},
            {"ed25519_pubkey_hex", new_peer.ed25519_pubkey_hex}
        }}
    }.dump();

    const auto bytes = str_to_bytes(gossip_json);
    std::unordered_map<std::string, std::shared_ptr<PeerSession>> snap;
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        snap = active_sessions_;
    }
    for (auto& [id, sess] : snap) {
        if (id != new_peer.node_id && sess && sess->is_established()) {
            sess->send_frame(FrameType::PeerGossip, 1, bytes);
        }
    }
}

JoinClusterResult MeshTransport::join_cluster(const std::string& token_or_ip,
                                              const std::string& pin,
                                              int timeout_ms) {
    JoinClusterResult res;
    std::vector<EndpointCandidate> candidates;
    std::string expected_fp;
    std::string auth_mode;
    std::string credential;

    const auto cfg_initial = config_mgr_.snapshot();
    if (token_or_ip.rfind("agmesh://join/v1/", 0) == 0) {
        const auto tok_opt = ProtocolCodec::decode_invite_token(token_or_ip);
        if (!tok_opt) {
            res.error = "Invalid agmesh://join/v1/ invite token";
            return res;
        }
        candidates = tok_opt->endpoints;
        expected_fp = tok_opt->fingerprint;
        auth_mode = "secret";
        credential = tok_opt->secret;
    } else {
        std::string host;
        std::uint16_t port = 47890;
        if (!parse_host_port(token_or_ip, 47890, host, port)) {
            res.error = "Invalid host[:port] endpoint: " + token_or_ip;
            return res;
        }
        const std::string pin_digits = canonicalize_pin(pin);
        if (pin_digits.size() != 6) {
            res.error = "A 6-digit PIN (format XXX-XXX) is required when joining by IP";
            return res;
        }
        auto cand_opt = NetworkDiscovery::classify_interface_address("manual", host, port);
        if (cand_opt) {
            candidates.push_back(std::move(*cand_opt));
        } else {
            candidates.push_back({host, port, "manual", "lan", 50});
        }
        auth_mode = "pin";
        credential = pin_digits;
    }

    if (candidates.empty()) {
        res.error = "No target endpoints available to dial";
        return res;
    }

    // Happy Eyeballs parallel staggered dialing (150ms step between sorted VPN / LAN candidates)
    std::unique_ptr<asio::ip::tcp::socket> winning_sock;
    EndpointCandidate winning_ep;
    double connect_rtt_ms = -1.0;

    try {
        asio::io_context dial_io;
        asio::steady_timer deadline_timer(dial_io, std::chrono::milliseconds(std::max(1000, timeout_ms)));
        std::vector<std::shared_ptr<asio::ip::tcp::socket>> sockets;
        std::vector<std::shared_ptr<asio::steady_timer>> stagger_timers;
        std::atomic<bool> won{false};
        std::size_t pending_probes = candidates.size();

        auto cancel_all = [&]() {
            deadline_timer.cancel();
            for (auto& t : stagger_timers) t->cancel();
            for (auto& s : sockets) {
                std::error_code ec;
                s->close(ec);
            }
        };

        deadline_timer.async_wait([&](std::error_code ec) {
            if (!ec) cancel_all();
        });

        for (std::size_t i = 0; i < candidates.size(); ++i) {
            auto s = std::make_shared<asio::ip::tcp::socket>(dial_io);
            auto t = std::make_shared<asio::steady_timer>(dial_io, std::chrono::milliseconds(i * 150));
            sockets.push_back(s);
            stagger_timers.push_back(t);

            const auto cand = candidates[i];
            t->async_wait([&, s, cand](std::error_code tec) {
                if (tec || won.load()) {
                    if (pending_probes > 0) --pending_probes;
                    return;
                }
                std::error_code addr_ec;
                const auto ip_addr = asio::ip::make_address(cand.ip, addr_ec);
                if (addr_ec) {
                    if (--pending_probes == 0 && !won.load()) cancel_all();
                    return;
                }
                const auto t0 = std::chrono::steady_clock::now();
                s->async_connect(asio::ip::tcp::endpoint(ip_addr, cand.port), [&, s, cand, t0](std::error_code cec) {
                    if (!cec && !won.exchange(true)) {
                        connect_rtt_ms = std::chrono::duration<double, std::milli>(
                                             std::chrono::steady_clock::now() - t0).count();
                        winning_sock = std::make_unique<asio::ip::tcp::socket>(std::move(*s));
                        winning_ep = cand;
                        cancel_all();
                    } else if (--pending_probes == 0 && !won.load()) {
                        cancel_all();
                    }
                });
            });
        }
        dial_io.run();
    } catch (const std::exception& ex) {
        res.error = std::string("Connection error: ") + ex.what();
        return res;
    }

    if (!winning_sock || !winning_sock->is_open()) {
        res.error = "Unable to connect to any candidate endpoint (timed out)";
        return res;
    }

    // Perform cryptographic PairRequest -> PairWelcome exchange over winning socket
    const auto joiner_eph = CryptoEngine::generate_x25519_ephemeral();
    const std::string joiner_eph_hex = CryptoEngine::bytes_to_hex(joiner_eph.pub.data(), 32);
    const std::uint16_t my_listen_port =
        bound_port_.load() > 0 ? bound_port_.load() : cfg_initial.network.listen_port;
    const std::int64_t ts_ms = core::now_ms();

    const auto transcript = build_pair_request_transcript(
        local_keys_.node_id, local_keys_.public_key_hex, joiner_eph_hex, my_listen_port, ts_ms);
    const auto auth_key = derive_pair_auth_key(credential);
    const auto hmac_j = CryptoEngine::hmac_sha256(auth_key, transcript);
    std::array<std::uint8_t, 64> sig_j{};
    CryptoEngine::ed25519_sign(local_keys_.ed25519_private, transcript, sig_j);

    const std::string req_json = nlohmann::json{
        {"joiner_node_id", local_keys_.node_id},
        {"joiner_listen_port", my_listen_port},
        {"joiner_ed25519_pub_hex", local_keys_.public_key_hex},
        {"joiner_eph_pub_hex", joiner_eph_hex},
        {"auth_mode", auth_mode},
        {"ts_ms", ts_ms},
        {"hmac_hex", CryptoEngine::bytes_to_hex(hmac_j.data(), hmac_j.size())},
        {"sig_hex", CryptoEngine::bytes_to_hex(sig_j.data(), sig_j.size())}
    }.dump();

    std::vector<std::uint8_t> req_wire;
    ProtocolCodec::encode_frame(
        FrameType::PairRequest, 0, 1, kFlagNone, str_to_bytes(req_json), 0, 1, nullptr, req_wire);

    std::error_code io_ec;
    asio::write(*winning_sock, asio::buffer(req_wire), io_ec);
    if (io_ec) {
        res.error = "Failed to transmit PairRequest: " + io_ec.message();
        return res;
    }

    std::array<std::uint8_t, kFrameHeaderSize> resp_hdr_buf{};
    asio::read(*winning_sock, asio::buffer(resp_hdr_buf), io_ec);
    if (io_ec) {
        res.error = "Failed to read PairWelcome header: " + io_ec.message();
        return res;
    }

    ProtocolErrorCode perr = ProtocolErrorCode::InvalidMagic;
    const auto hdr_opt = ProtocolCodec::decode_header(resp_hdr_buf.data(), resp_hdr_buf.size(), perr);
    if (!hdr_opt) {
        res.error = "Invalid pairing response header";
        return res;
    }

    std::vector<std::uint8_t> resp_payload(hdr_opt->payload_len);
    if (hdr_opt->payload_len > 0) {
        asio::read(*winning_sock, asio::buffer(resp_payload), io_ec);
        if (io_ec) {
            res.error = "Failed to read pairing response payload: " + io_ec.message();
            return res;
        }
    }

    std::vector<std::uint8_t> plain_resp;
    if (!ProtocolCodec::decode_payload(*hdr_opt, resp_payload, nullptr, plain_resp, perr)) {
        res.error = "Failed to decode pairing response payload";
        return res;
    }

    try {
        const auto rj = nlohmann::json::parse(bytes_to_str(plain_resp));
        if (hdr_opt->type == FrameType::PairReject) {
            res.error = rj.value("reason", "Pairing rejected by host");
            return res;
        }
        if (hdr_opt->type != FrameType::PairWelcome) {
            res.error = "Unexpected pairing response frame";
            return res;
        }

        const std::string host_eph_hex = rj.at("host_eph_pub_hex").get<std::string>();
        const std::string host_ed_hex = rj.at("host_ed25519_pub_hex").get<std::string>();
        const std::string sealed_hex = rj.at("sealed_welcome_hex").get<std::string>();
        const std::string sig_hex = rj.at("sig_hex").get<std::string>();

        const auto host_eph_bytes = CryptoEngine::hex_to_bytes(host_eph_hex);
        const auto host_ed_bytes = CryptoEngine::hex_to_bytes(host_ed_hex);
        const auto sealed_bytes = CryptoEngine::hex_to_bytes(sealed_hex);
        const auto sig_bytes = CryptoEngine::hex_to_bytes(sig_hex);
        if (host_eph_bytes.size() != 32 || host_ed_bytes.size() != 32 || sig_bytes.size() != 64) {
            res.error = "Invalid key lengths in PairWelcome";
            return res;
        }

        std::array<std::uint8_t, 32> host_ed_arr{};
        std::array<std::uint8_t, 32> host_eph_arr{};
        std::array<std::uint8_t, 64> sig_h_arr{};
        std::copy_n(host_ed_bytes.begin(), 32, host_ed_arr.begin());
        std::copy_n(host_eph_bytes.begin(), 32, host_eph_arr.begin());
        std::copy_n(sig_bytes.begin(), 64, sig_h_arr.begin());

        const std::string actual_fp = CryptoEngine::compute_fingerprint(host_ed_arr);
        if (!expected_fp.empty() && actual_fp != expected_fp) {
            res.error = "MITM protection: Host fingerprint mismatch (" + actual_fp + " != " + expected_fp + ")";
            return res;
        }

        const auto sig_input = str_to_bytes(host_eph_hex + "|" + joiner_eph_hex + "|" + sealed_hex);
        if (!CryptoEngine::ed25519_verify(host_ed_arr, sig_input, sig_h_arr)) {
            res.error = "Host Ed25519 signature verification failed on PairWelcome";
            return res;
        }

        const auto pair_key = derive_pair_welcome_key(joiner_eph.priv, host_eph_arr, auth_key);
        const auto aad = str_to_bytes(host_eph_hex + "|" + host_ed_hex);
        std::vector<std::uint8_t> welcome_plain_bytes;
        if (!CryptoEngine::aead_decrypt(pair_key, aad, sealed_bytes, welcome_plain_bytes)) {
            res.error = "Failed to decrypt PairWelcome AEAD payload";
            return res;
        }

        const auto wj = nlohmann::json::parse(bytes_to_str(welcome_plain_bytes));
        const std::string host_node_id = wj.at("host_node_id").get<std::string>();
        const std::string cluster_psk = wj.at("cluster_psk_hex").get<std::string>();

        std::vector<core::PeerConfig> peers_to_save;
        if (wj.contains("peers") && wj["peers"].is_array()) {
            for (const auto& pj : wj["peers"]) {
                core::PeerConfig pc;
                pc.node_id = pj.value("node_id", "");
                pc.address = pj.value("address", "");
                pc.port = pj.value("port", static_cast<std::uint16_t>(47890));
                pc.pubkey_fingerprint = pj.value("pubkey_fingerprint", "");
                pc.ed25519_pubkey_hex = pj.value("ed25519_pubkey_hex", "");
                pc.is_relay = pj.value("is_relay", false);
                if (pc.node_id == host_node_id) {
                    // Always record the winning VPN/LAN route that actually succeeded
                    pc.address = winning_ep.ip;
                    pc.port = winning_ep.port;
                }
                if (!pc.node_id.empty() && pc.node_id != local_keys_.node_id) {
                    peers_to_save.push_back(std::move(pc));
                }
            }
        }

        std::string save_err;
        if (!config_mgr_.apply_joined_cluster_config(cluster_psk, peers_to_save, save_err)) {
            res.error = "Failed to save cluster config: " + save_err;
            return res;
        }

        // Apply on the fly and immediately initiate authenticated mesh + delta sync
        notify_config_reloaded();
        trigger_sync_all();

        res.ok = true;
        res.host_node_id = host_node_id;
        res.winning_route_ip = winning_ep.ip;
        res.winning_route_port = winning_ep.port;
        res.winning_route_kind = winning_ep.kind;
        res.rtt_ms = connect_rtt_ms;
        res.peers_configured = peers_to_save.size();
        emit_event({"cluster_joined", host_node_id, "", "Joined cluster via " + winning_ep.ip + ":" + std::to_string(winning_ep.port), core::now_ms()});
        return res;
    } catch (const std::exception& ex) {
        res.error = std::string("Malformed PairWelcome: ") + ex.what();
        return res;
    }
}

} // namespace agmesh::net
