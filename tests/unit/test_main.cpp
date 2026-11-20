#include "agmesh/adapter/antigravity_adapter.hpp"
#include "agmesh/core/config.hpp"
#include "agmesh/core/hlc_crdt.hpp"
#include "agmesh/mcp/mcp_server.hpp"
#include "agmesh/net/crypto_engine.hpp"
#include "agmesh/net/mesh_transport.hpp"
#include "agmesh/net/network_discovery.hpp"
#include "agmesh/net/protocol_codec.hpp"
#include "agmesh/sanitizer/secret_sanitizer.hpp"
#include "agmesh/storage/sqlite_store.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

using namespace agmesh;

namespace {

struct TempDirGuard {
    std::filesystem::path dir;
    explicit TempDirGuard(const std::string& name) {
        const auto unique = std::to_string(core::now_us());
        dir = std::filesystem::temp_directory_path() / ("agmesh_test_" + name + "_" + unique);
        std::filesystem::create_directories(dir);
    }
    ~TempDirGuard() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

} // namespace

TEST_CASE("HybridLogicalClock and VectorClock causal ordering and CRDT LWW", "[core][crdt]") {
    core::HybridLogicalClock hlc_a("node-a");
    core::HybridLogicalClock hlc_b("node-b");

    const auto t1 = hlc_a.tick();
    const auto t2 = hlc_a.tick();
    REQUIRE(t2 > t1);

    core::HlcTimestamp future{t2.physical_ms + 5000, 7, "node-b"};
    const auto witnessed = hlc_a.witness(future);
    REQUIRE(witnessed > future);

    core::VectorClock vc1{{"node-a", 2}, {"node-b", 1}};
    core::VectorClock vc2{{"node-a", 1}, {"node-b", 3}};
    REQUIRE(core::VectorClockOps::compare(vc1, vc2) == core::ClockRelation::Concurrent);

    core::VectorClockOps::merge(vc1, vc2);
    REQUIRE(vc1["node-a"] == 2);
    REQUIRE(vc1["node-b"] == 3);
    REQUIRE(core::VectorClockOps::compare(vc1, vc2) == core::ClockRelation::After);

    core::SessionRecord existing;
    existing.session_id = "s1";
    existing.title = "Old";
    existing.hlc = t1;

    core::SessionRecord incoming;
    incoming.session_id = "s1";
    incoming.title = "New";
    incoming.hlc = t2;

    REQUIRE(core::CrdtResolver::merge_lww(existing, incoming));
    REQUIRE(existing.title == "New");
    REQUIRE_FALSE(core::CrdtResolver::merge_lww(existing, { "s1", "node-a", "", "", "", "Stale", "idle", false, "auto", {}, 0, 0, t1 }));
}

TEST_CASE("SecretSanitizer redacts known tokens, PEM keys, DB URIs, and high-entropy secrets", "[sanitizer]") {
    sanitizer::SecretSanitizer san;

    const std::string aws = "AKIAIOSFODNN7EXAMPLE";
    const std::string gh = "ghp_1234567890abcdef1234567890abcdef1234";
    const std::string tg = "123456789:AAHdqTcvCH1vGWJxfSeofSAs0K5PALDsaw";
    const std::string jwt = "eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxMjM0NTY3ODkwIn0.dozjgNryP4J3jVmNHl0w5N_XgL0n3I9PlFUP0THsR8U";
    const std::string db = "postgres://admin:S3cr3tPassw0rd!@db.internal:5432/prod";
    const std::string pem = "-----BEGIN OPENSSH PRIVATE KEY-----\nb3BlbnNzaC1rZXktdjEAAAA=\n-----END OPENSSH PRIVATE KEY-----";
    const std::string custom = "custom_api_key=Kx9mP2vQ8zR4tL7wN5jB3yF6hC1dV0sA";

    const std::string payload =
        "aws=" + aws + "\n"
        "gh=" + gh + "\n"
        "tg=" + tg + "\n"
        "jwt=" + jwt + "\n"
        "db=" + db + "\n"
        + pem + "\n" + custom;

    const auto res = san.sanitize(payload);
    REQUIRE(res.had_redactions());
    REQUIRE(res.sanitized_text.find(aws) == std::string::npos);
    REQUIRE(res.sanitized_text.find(gh) == std::string::npos);
    REQUIRE(res.sanitized_text.find(tg) == std::string::npos);
    REQUIRE(res.sanitized_text.find(jwt) == std::string::npos);
    REQUIRE(res.sanitized_text.find("S3cr3tPassw0rd!") == std::string::npos);
    REQUIRE(res.sanitized_text.find("b3BlbnNzaC1rZXktdjEAAAA=") == std::string::npos);
    REQUIRE(res.sanitized_text.find("Kx9mP2vQ8zR4tL7wN5jB3yF6hC1dV0sA") == std::string::npos);
    REQUIRE(res.sanitized_text.find("[REDACTED:AWS_ACCESS_KEY:") != std::string::npos);
    REQUIRE(res.sanitized_text.find("[REDACTED:GITHUB_TOKEN:") != std::string::npos);
    REQUIRE(res.sanitized_text.find("[REDACTED:SSH_PRIVATE_KEY:") != std::string::npos);

    // Deterministic redaction markers
    REQUIRE(sanitizer::SecretSanitizer::format_marker("AWS_ACCESS_KEY", aws) ==
            sanitizer::SecretSanitizer::format_marker("AWS_ACCESS_KEY", aws));
}

TEST_CASE("ProtocolCodec binary framing with Zstd compression and ChaCha20-Poly1305 AEAD", "[net][codec]") {
    auto keys_tx = net::CryptoEngine::generate_x25519_ephemeral();
    auto keys_rx = net::CryptoEngine::generate_x25519_ephemeral();
    std::array<std::uint8_t, 32> shared1{};
    std::array<std::uint8_t, 32> shared2{};
    REQUIRE(net::CryptoEngine::compute_x25519_shared_secret(keys_tx.priv, keys_rx.pub, shared1));
    REQUIRE(net::CryptoEngine::compute_x25519_shared_secret(keys_rx.priv, keys_tx.pub, shared2));
    REQUIRE(shared1 == shared2);

    net::SessionKeys tx_keys{shared1, shared1, 0, 0};
    net::SessionKeys rx_keys{shared2, shared2, 0, 0};

    std::string big_text;
    for (int i = 0; i < 64; ++i) {
        big_text += "Sync delta batch event message with compressible repeated structure; ";
    }
    const std::vector<std::uint8_t> plain(big_text.begin(), big_text.end());

    std::vector<std::uint8_t> wire;
    REQUIRE(net::ProtocolCodec::encode_frame(
        net::FrameType::DeltaEventBatch, 3, 42, net::kFlagNone, plain, 128, 3, &tx_keys, wire));
    REQUIRE(wire.size() < plain.size()); // Compressed + AEAD overhead still smaller

    net::ProtocolErrorCode err = net::ProtocolErrorCode::InvalidMagic;
    const auto hdr = net::ProtocolCodec::decode_header(wire.data(), net::kFrameHeaderSize, err);
    REQUIRE(hdr.has_value());
    REQUIRE((hdr->flags & net::kFlagCompressedZstd) != 0);
    REQUIRE((hdr->flags & net::kFlagEncryptedAead) != 0);
    REQUIRE(hdr->stream_id == 3);
    REQUIRE(hdr->seq_no == 42);

    std::vector<std::uint8_t> raw_payload(wire.begin() + net::kFrameHeaderSize, wire.end());
    std::vector<std::uint8_t> decoded;
    REQUIRE(net::ProtocolCodec::decode_payload(*hdr, raw_payload, &rx_keys, decoded, err));
    REQUIRE(decoded == plain);

    // Tamper check must reject corrupted header CRC32C
    wire[8] ^= 0xFFu;
    REQUIRE_FALSE(net::ProtocolCodec::decode_header(wire.data(), net::kFrameHeaderSize, err).has_value());
}

TEST_CASE("SqliteStore WAL + FTS5 Cyrillic & Latin search + deduplication + tombstone purge", "[storage][fts5]") {
    TempDirGuard tmp("store");
    storage::SqliteStore store(tmp.dir / "test.db");
    std::string err;
    REQUIRE(store.open_and_migrate(err));

    core::HybridLogicalClock hlc("node-1");
    core::SessionRecord s;
    s.session_id = "sess-ru-en";
    s.node_id = "node-1";
    s.repo_name = "ag-mesh";
    s.git_branch = "main";
    s.title = "Синхронизация контекста Asio";
    s.created_at_ms = core::now_ms();
    s.updated_at_ms = s.created_at_ms;
    s.hlc = hlc.tick();
    REQUIRE(store.upsert_session(s));

    core::MessageRecord m;
    m.session_id = s.session_id;
    m.node_id = "node-1";
    m.seq_index = 0;
    m.role = "assistant";
    m.content = "Исправлена гонка потоков в PeerSession и добавлен Zstd фрейминг.";
    m.created_at_ms = s.created_at_ms;
    m.content_sha256 = core::CrdtResolver::compute_message_hash(m.session_id, m.role, m.seq_index, m.content);
    m.message_id = "m1";
    m.hlc = hlc.tick();

    bool inserted = false;
    REQUIRE(store.append_message(m, inserted));
    REQUIRE(inserted);

    // Duplicate append must be ignored idempotently
    bool inserted_dup = false;
    REQUIRE(store.append_message(m, inserted_dup));
    REQUIRE_FALSE(inserted_dup);

    // Cyrillic + Latin FTS5 search
    const auto hits_ru = store.search_fts("гонка PeerSession");
    REQUIRE(!hits_ru.empty());
    REQUIRE(hits_ru.front().session_id == "sess-ru-en");

    // Atomic Markdown export
    REQUIRE(store.export_snapshot_markdown(tmp.dir / "snapshot.md", "node-1"));
    REQUIRE(std::filesystem::exists(tmp.dir / "snapshot.md"));

    // Tombstone purge removes session and suppresses re-insertion
    core::TombstoneRecord ts{"session", s.session_id, core::now_ms(), hlc.tick()};
    REQUIRE(store.purge_entity(ts));
    REQUIRE_FALSE(store.get_session(s.session_id).has_value());
    REQUIRE(store.is_tombstoned("session", s.session_id));
}

TEST_CASE("AntigravityAdapter reads JSONL transcripts with partial lines, Markdown artifacts, and SQLite RO", "[adapter]") {
    TempDirGuard tmp("ag");
    const auto brain = tmp.dir / "brain" / "conv-42";
    const auto logs = brain / ".system_generated" / "logs";
    std::filesystem::create_directories(logs);

    {
        std::ofstream ofs(logs / "transcript.jsonl", std::ios::binary);
        ofs << R"({"type":"USER_INPUT","source":"USER_EXPLICIT","content":"Fix TLS handshake"})" << "\n";
        ofs << R"({"type":"PLANNER_RESPONSE","content":"Switched to X25519 + Ed25519 signatures."})" << "\n";
        ofs << R"({"type":"PLANNER_RESPONSE","content":"half-written line without closing brace)" << "\n";
    }
    {
        std::ofstream ofs(brain / "walkthrough.md", std::ios::binary);
        ofs << "# Handshake Walkthrough\nVerified mTLS and Noise_XX.\n";
    }

    // Also create a real Antigravity conversation_summaries.db + conversations_fts + annotations + presence
    std::filesystem::create_directories(tmp.dir / "annotations");
    std::filesystem::create_directories(tmp.dir / "presence");
    {
        std::ofstream ann(tmp.dir / "annotations" / "conv-sql-1.pbtxt", std::ios::binary);
        ann << "title:\"Annotated SQL Session\" last_user_view_time:{seconds:1795000000 nanos:0}\n";
        std::ofstream prs(tmp.dir / "presence" / "conv-sql-1.lock", std::ios::binary);
        prs << "";
    }
    {
        sqlite3* ag_db = nullptr;
        REQUIRE(sqlite3_open((tmp.dir / "conversation_summaries.db").string().c_str(), &ag_db) == SQLITE_OK);
        const char* ddl =
            "CREATE TABLE conversation_summaries("
            "conversation_id TEXT PRIMARY KEY, title TEXT, preview TEXT, step_count INTEGER, "
            "last_modified_time datetime, workspace_uris TEXT, status TEXT, project_id TEXT, "
            "not_fully_idle numeric, killed numeric);"
            "CREATE VIRTUAL TABLE conversations_fts USING fts5("
            "conversation_id UNINDEXED, chunk_index UNINDEXED, title, workspace, project, user_body, agent_body);"
            "INSERT INTO conversation_summaries VALUES("
            "'conv-sql-1', '', 'Preview summary', 12, '2026-11-20 12:30:00+00:00', '', 'CASCADE_RUN_STATUS_IDLE', 'ag-mesh', 0, 0);"
            "INSERT INTO conversations_fts VALUES('conv-sql-1', 0, 'SQL Session', '', 'ag-mesh', 'User question', 'Agent answer');";
        REQUIRE(sqlite3_exec(ag_db, ddl, nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(ag_db);
    }

    core::AntigravityPathsConfig pcfg;
    pcfg.app_data_dir = tmp.dir.string();
    adapter::AntigravityAdapter ag_adapter(pcfg);

    const auto probe = ag_adapter.run_probe();
    REQUIRE(probe.app_data_exists);
    REQUIRE(probe.app_data_readable);
    REQUIRE(probe.found_transcripts.size() == 1);
    REQUIRE(probe.found_artifacts.size() == 1);
    REQUIRE(probe.found_databases.size() == 1);

    const auto bundles = ag_adapter.collect_updates("node-local", core::PrivacyConfig{});
    REQUIRE(bundles.size() >= 3);
    bool found_sql = false;
    for (const auto& b : bundles) {
        if (b.session.session_id == "conv-sql-1") {
            found_sql = true;
            REQUIRE(b.session.title == "Annotated SQL Session");
            REQUIRE(b.session.status == "active");
            REQUIRE(b.session.repo_name == "ag-mesh");
            REQUIRE(b.messages.size() == 2);
            REQUIRE(b.summary.has_value());
        }
    }
    REQUIRE(found_sql);

    // Simulate a torn/corrupt WAL checkpoint snapshot; collect_updates() must skip silently
    {
        std::ofstream corrupt(tmp.dir / "conversation_summaries.db", std::ios::binary | std::ios::trunc);
        corrupt << "NOT A VALID SQLITE FILE - TORN CHECKPOINT";
    }
    const auto torn_bundles = ag_adapter.collect_updates("node-local", core::PrivacyConfig{});
    for (const auto& b : torn_bundles) {
        REQUIRE(b.session.session_id != "conv-sql-1");
    }
}

TEST_CASE("McpServer JSON-RPC 2.0 routes tools and resources accurately", "[mcp]") {
    TempDirGuard tmp("mcp");
    core::ConfigManager cfg_mgr(tmp.dir / "config.toml");
    storage::SqliteStore store(tmp.dir / "mcp.db");
    std::string err;
    REQUIRE(store.open_and_migrate(err));

    core::HybridLogicalClock hlc("mcp-node");
    sanitizer::SecretSanitizer san;
    adapter::AntigravityAdapter adp;
    mcp::McpServer srv(cfg_mgr, store, hlc, san, adp, nullptr);

    const auto init_resp = nlohmann::json::parse(srv.handle_jsonrpc_message(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})"));
    REQUIRE(init_resp["result"]["serverInfo"]["name"] == "ag-mesh");

    const auto list_resp = nlohmann::json::parse(srv.handle_jsonrpc_message(
        R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})"));
    REQUIRE(list_resp["result"]["tools"].size() == 11);

    const auto pub_resp = nlohmann::json::parse(srv.handle_jsonrpc_message(
        R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"cluster_publish_update","arguments":{"session_id":"s-mcp-1","current_goal":"Deploy auth","summary_text":"Implemented JWT check with token ghp_1234567890abcdef1234567890abcdef1234"}}})"));
    REQUIRE_FALSE(pub_resp["result"]["isError"].get<bool>());

    // Verify published update was sanitized before storing & searchable via MCP
    const auto search_resp = nlohmann::json::parse(srv.handle_jsonrpc_message(
        R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"cluster_search_context","arguments":{"query":"JWT"}}})"));
    const std::string search_txt = search_resp["result"]["content"][0]["text"].get<std::string>();
    REQUIRE(search_txt.find("s-mcp-1") != std::string::npos);
    REQUIRE(search_txt.find("ghp_1234567890abcdef1234567890abcdef1234") == std::string::npos);

    // Handoff create & claim via MCP
    const auto ho_create = nlohmann::json::parse(srv.handle_jsonrpc_message(
        R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"cluster_create_handoff","arguments":{"task_description":"Continue MCP tests","target_node_id":"node-2"}}})"));
    const auto ho_body = nlohmann::json::parse(ho_create["result"]["content"][0]["text"].get<std::string>());
    const std::string ho_id = ho_body["handoff_id"].get<std::string>();
    REQUIRE(!ho_id.empty());

    const auto ho_claim = nlohmann::json::parse(srv.handle_jsonrpc_message(
        nlohmann::json{
            {"jsonrpc", "2.0"},
            {"id", 6},
            {"method", "tools/call"},
            {"params", {{"name", "cluster_claim_handoff"}, {"arguments", {{"handoff_id", ho_id}, {"new_session_id", "s-new"}}}}}
        }.dump()));
    const auto claim_body = nlohmann::json::parse(ho_claim["result"]["content"][0]["text"].get<std::string>());
    REQUIRE(claim_body["status"] == "claimed");

    // Resource read
    const auto res_read = nlohmann::json::parse(srv.handle_jsonrpc_message(
        R"({"jsonrpc":"2.0","id":7,"method":"resources/read","params":{"uri":"agmesh://cluster/overview"}})"));
    REQUIRE(res_read["result"]["contents"][0]["uri"] == "agmesh://cluster/overview");
}

TEST_CASE("NetworkDiscovery classifies VPN/LAN/Public and filters bridges and loopback", "[net][discovery]") {
    const auto wg = net::NetworkDiscovery::classify_interface_address("wg0", "10.8.0.1", 47890);
    REQUIRE(wg.has_value());
    REQUIRE(wg->kind == "vpn");
    REQUIRE(wg->priority == 100);

    const auto ts = net::NetworkDiscovery::classify_interface_address("eth1", "100.64.12.34", 47890);
    REQUIRE(ts.has_value());
    REQUIRE(ts->kind == "vpn");
    REQUIRE(ts->priority == 100);

    const auto wt = net::NetworkDiscovery::classify_interface_address("WireGuard Tunnel", "10.200.0.2", 47890);
    REQUIRE(wt.has_value());
    REQUIRE(wt->kind == "vpn");

    const auto lan = net::NetworkDiscovery::classify_interface_address("eth0", "192.168.1.42", 47890);
    REQUIRE(lan.has_value());
    REQUIRE(lan->kind == "lan");
    REQUIRE(lan->priority == 50);

    const auto pub = net::NetworkDiscovery::classify_interface_address("eth0", "203.0.113.10", 47890);
    REQUIRE(pub.has_value());
    REQUIRE(pub->kind == "public");
    REQUIRE(pub->priority == 10);

    // Must filter loopback, link-local, Docker, WSL, and Hyper-V bridges
    REQUIRE_FALSE(net::NetworkDiscovery::classify_interface_address("lo", "127.0.0.1", 47890).has_value());
    REQUIRE_FALSE(net::NetworkDiscovery::classify_interface_address("lo", "::1", 47890).has_value());
    REQUIRE_FALSE(net::NetworkDiscovery::classify_interface_address("eth0", "169.254.10.20", 47890).has_value());
    REQUIRE_FALSE(net::NetworkDiscovery::classify_interface_address("eth0", "fe80::1", 47890).has_value());
    REQUIRE_FALSE(net::NetworkDiscovery::classify_interface_address("docker0", "172.17.0.1", 47890).has_value());
    REQUIRE_FALSE(net::NetworkDiscovery::classify_interface_address("veth9f12a", "10.244.0.5", 47890).has_value());
    REQUIRE_FALSE(net::NetworkDiscovery::classify_interface_address("br-a1b2c3", "172.18.0.1", 47890).has_value());
    REQUIRE_FALSE(net::NetworkDiscovery::classify_interface_address("virbr0", "192.168.122.1", 47890).has_value());
    REQUIRE_FALSE(net::NetworkDiscovery::classify_interface_address("vEthernet (WSL)", "172.25.16.1", 47890).has_value());
}

TEST_CASE("Pairing invite token round-trip, expired TTL rejection, and 5-attempt PIN lockout", "[net][pairing]") {
    net::PairingInviteToken tok;
    tok.version = 1;
    tok.node_id = "host-alpha";
    tok.fingerprint = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    tok.secret = "deadbeefcafebabe0123456789abcdef";
    tok.endpoints = {
        {"10.8.0.1", 47890, "wg0", "vpn", 100},
        {"192.168.1.10", 47890, "eth0", "lan", 50}
    };

    const std::string uri = net::ProtocolCodec::encode_invite_token(tok);
    REQUIRE(uri.rfind("agmesh://join/v1/", 0) == 0);
    const auto decoded = net::ProtocolCodec::decode_invite_token(uri);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->node_id == tok.node_id);
    REQUIRE(decoded->fingerprint == tok.fingerprint);
    REQUIRE(decoded->secret == tok.secret);
    REQUIRE(decoded->endpoints.size() == 2);
    REQUIRE(decoded->endpoints[0].kind == "vpn");
    REQUIRE_FALSE(net::ProtocolCodec::decode_invite_token("agmesh://invalid/foo").has_value());

    TempDirGuard tmp("pair_sec");
    auto cfg1 = core::ConfigManager::create_default_config("host-1");
    cfg1.data_dir = (tmp.dir / "h1").string();
    cfg1.db_path = (tmp.dir / "h1" / "h1.db").string();
    cfg1.key_file = (tmp.dir / "h1" / "h1.key").string();
    cfg1.network.listen_address = "127.0.0.1";
    cfg1.network.listen_port = 0;
    cfg1.network.enable_mdns_discovery = false;
    cfg1.network.cluster_psk_hex = net::CryptoEngine::generate_random_psk_hex();
    core::ConfigManager::save_to_file(cfg1, tmp.dir / "h1.toml");

    core::ConfigManager mgr1(tmp.dir / "h1.toml");
    storage::SqliteStore store1(cfg1.db_path);
    std::string err;
    REQUIRE(store1.open_and_migrate(err));
    core::HybridLogicalClock hlc1("host-1");
    net::MeshTransport tr1(mgr1, store1, hlc1, net::CryptoEngine::generate_node_keypair("host-1"));
    REQUIRE(tr1.start(err));
    const std::uint16_t port1 = tr1.bound_port();

    auto cfg2 = core::ConfigManager::create_default_config("joiner-1");
    cfg2.data_dir = (tmp.dir / "j1").string();
    cfg2.db_path = (tmp.dir / "j1" / "j1.db").string();
    cfg2.key_file = (tmp.dir / "j1" / "j1.key").string();
    cfg2.network.listen_address = "127.0.0.1";
    cfg2.network.listen_port = 0;
    cfg2.network.enable_mdns_discovery = false;
    core::ConfigManager::save_to_file(cfg2, tmp.dir / "j1.toml");

    core::ConfigManager mgr2(tmp.dir / "j1.toml");
    storage::SqliteStore store2(cfg2.db_path);
    REQUIRE(store2.open_and_migrate(err));
    core::HybridLogicalClock hlc2("joiner-1");
    net::MeshTransport tr2(mgr2, store2, hlc2, net::CryptoEngine::generate_node_keypair("joiner-1"));
    REQUIRE(tr2.start(err));

    // 1. Verify expired TTL (1 second) rejects token
    const auto short_st = tr1.start_pairing_window(
        1, 1, port1, {net::EndpointCandidate{"127.0.0.1", port1, "lo", "lan", 50}});
    std::this_thread::sleep_for(std::chrono::milliseconds(1150));
    const auto expired_res = tr2.join_cluster(short_st.invite_token, "", 3000);
    REQUIRE_FALSE(expired_res.ok);
    REQUIRE(expired_res.error.find("expired") != std::string::npos);

    // 2. Verify 5 bad PIN attempts lock out the pairing window
    const auto pin_st = tr1.start_pairing_window(
        600, 1, port1, {net::EndpointCandidate{"127.0.0.1", port1, "lo", "lan", 50}});
    const std::string wrong_pin = (pin_st.pin == "000-000") ? "111-111" : "000-000";
    const std::string target_ep = "127.0.0.1:" + std::to_string(port1);
    for (int attempt = 1; attempt <= 5; ++attempt) {
        const auto bad_res = tr2.join_cluster(target_ep, wrong_pin, 3000);
        REQUIRE_FALSE(bad_res.ok);
    }
    const auto locked_st = tr1.get_pairing_status();
    REQUIRE(locked_st.locked_out);
    REQUIRE_FALSE(locked_st.active);
    REQUIRE(locked_st.failed_attempts == 5);

    // Even the real token must now be rejected after lockout
    const auto post_lock = tr2.join_cluster(pin_st.invite_token, "", 3000);
    REQUIRE_FALSE(post_lock.ok);

    tr2.stop();
    tr1.stop();
}
