#include "agmesh/core/config.hpp"
#include "agmesh/core/hlc_crdt.hpp"
#include "agmesh/net/crypto_engine.hpp"
#include "agmesh/net/mesh_transport.hpp"
#include "agmesh/storage/sqlite_store.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace agmesh;

namespace {

struct ClusterNodeHarness {
    std::string node_id;
    std::filesystem::path root_dir;
    std::filesystem::path config_path;
    net::NodeKeyPair keys;
    std::unique_ptr<core::ConfigManager> cfg_mgr;
    std::unique_ptr<storage::SqliteStore> store;
    std::unique_ptr<core::HybridLogicalClock> hlc;
    std::unique_ptr<net::MeshTransport> transport;
};

bool wait_until(const std::function<bool()>& pred, int timeout_ms = 6000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    return pred();
}

} // namespace

TEST_CASE("5-node P2P mesh: replication, 2-node dropout/recovery, split-brain CRDT merge, and handoff", "[integration][5nodes]") {
    const std::filesystem::path base_dir =
        std::filesystem::temp_directory_path() / ("agmesh_5node_it_" + std::to_string(core::now_us()));
    std::filesystem::create_directories(base_dir);

    struct Cleanup {
        std::filesystem::path p;
        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove_all(p, ec);
        }
    } cleanup{base_dir};

    const std::string psk = net::CryptoEngine::generate_random_psk_hex();
    std::vector<std::unique_ptr<ClusterNodeHarness>> nodes(5);

    // 1. Pre-generate 5 identities and bind ephemeral loopback ports sequentially
    for (int i = 0; i < 5; ++i) {
        auto h = std::make_unique<ClusterNodeHarness>();
        h->node_id = "node-" + std::to_string(i + 1);
        h->root_dir = base_dir / h->node_id;
        std::filesystem::create_directories(h->root_dir);
        h->config_path = h->root_dir / "config.toml";
        h->keys = net::CryptoEngine::generate_node_keypair(h->node_id);

        auto cfg = core::ConfigManager::create_default_config(h->node_id);
        cfg.data_dir = h->root_dir.string();
        cfg.db_path = (h->root_dir / "mesh.db").string();
        cfg.key_file = (h->root_dir / "node.key").string();
        cfg.network.listen_address = "127.0.0.1";
        cfg.network.listen_port = 0; // Ephemeral port assigned by OS
        cfg.network.enable_mdns_discovery = false;
        cfg.network.cluster_psk_hex = psk;
        cfg.network.heartbeat_interval_ms = 500;
        cfg.network.peer_timeout_ms = 3000;
        cfg.network.io_threads = 2;
        core::ConfigManager::save_to_file(cfg, h->config_path);

        h->cfg_mgr = std::make_unique<core::ConfigManager>(h->config_path);
        h->store = std::make_unique<storage::SqliteStore>(cfg.db_path);
        std::string db_err;
        REQUIRE(h->store->open_and_migrate(db_err));
        h->hlc = std::make_unique<core::HybridLogicalClock>(h->node_id);
        h->transport = std::make_unique<net::MeshTransport>(*h->cfg_mgr, *h->store, *h->hlc, h->keys);

        std::string net_err;
        REQUIRE(h->transport->start(net_err));
        REQUIRE(h->transport->bound_port() > 0);
        nodes[static_cast<std::size_t>(i)] = std::move(h);
    }

    // 2. Wire full 5-node peer whitelist and persist bound ports without stop/restart port churn
    std::vector<std::uint16_t> ports(5);
    for (int i = 0; i < 5; ++i) {
        ports[static_cast<std::size_t>(i)] = nodes[static_cast<std::size_t>(i)]->transport->bound_port();
    }
    for (int i = 0; i < 5; ++i) {
        for (int j = 0; j < 5; ++j) {
            if (i == j) continue;
            core::PeerConfig pc;
            pc.node_id = nodes[static_cast<std::size_t>(j)]->node_id;
            pc.address = "127.0.0.1";
            pc.port = ports[static_cast<std::size_t>(j)];
            pc.pubkey_fingerprint = nodes[static_cast<std::size_t>(j)]->keys.fingerprint_sha256;
            pc.ed25519_pubkey_hex = nodes[static_cast<std::size_t>(j)]->keys.public_key_hex;
            std::string perr;
            REQUIRE(nodes[static_cast<std::size_t>(i)]->cfg_mgr->add_or_update_peer(pc, perr));
        }
        // Persist assigned listen_port in config.toml so dropout recovery restarts on the same port
        auto persisted = nodes[static_cast<std::size_t>(i)]->cfg_mgr->snapshot();
        persisted.network.listen_port = ports[static_cast<std::size_t>(i)];
        core::ConfigManager::save_to_file(persisted, nodes[static_cast<std::size_t>(i)]->config_path);
    }

    for (auto& n : nodes) {
        n->transport->notify_config_reloaded();
    }

    // Wait until all 5 nodes establish authenticated mesh links (4 peers each)
    REQUIRE(wait_until([&]() {
        for (const auto& n : nodes) {
            if (n->transport->connected_peers().size() < 4) return false;
        }
        return true;
    }, 8000));

    // 3. Replicate a session + message from node-1 across all 5 hosts
    {
        core::SessionRecord s;
        s.session_id = "sess-cluster-1";
        s.node_id = "node-1";
        s.repo_name = "ag-mesh";
        s.git_branch = "main";
        s.title = "5-node replication verification";
        s.created_at_ms = core::now_ms();
        s.updated_at_ms = s.created_at_ms;
        s.hlc = nodes[0]->hlc->tick();
        REQUIRE(nodes[0]->store->upsert_session(s));

        core::MessageRecord m;
        m.message_id = "msg-1";
        m.session_id = s.session_id;
        m.node_id = "node-1";
        m.seq_index = 0;
        m.role = "assistant";
        m.content = "Replicated payload verified across five hosts";
        m.created_at_ms = s.created_at_ms;
        m.content_sha256 = core::CrdtResolver::compute_message_hash(m.session_id, m.role, m.seq_index, m.content);
        m.hlc = s.hlc;
        bool ins = false;
        REQUIRE(nodes[0]->store->append_message(m, ins));

        nodes[0]->transport->broadcast_delta_batch({s}, {m}, {}, {}, {}, {}, {});
    }

    REQUIRE(wait_until([&]() {
        for (const auto& n : nodes) {
            if (!n->store->get_session("sess-cluster-1").has_value()) return false;
            if (n->store->search_fts("Replicated payload").empty()) return false;
        }
        return true;
    }, 6000));

    // 4. Take 2 of 5 nodes offline (node-4 and node-5), write updates + handoff while offline
    nodes[3]->transport->stop();
    nodes[4]->transport->stop();
    REQUIRE(wait_until([&]() {
        return nodes[0]->transport->connected_peers().size() == 2 &&
               nodes[1]->transport->connected_peers().size() == 2 &&
               nodes[2]->transport->connected_peers().size() == 2;
    }, 5000));

    {
        core::HandoffPacket hp;
        hp.handoff_id = "ho-offline-test";
        hp.from_node_id = "node-2";
        hp.target_node_id = "node-5";
        hp.repo_name = "ag-mesh";
        hp.git_branch = "feat/sync";
        hp.git_commit = "abc1234";
        hp.task_description = "Finish offline outbox recovery test";
        hp.status = "pending";
        hp.created_at_ms = core::now_ms();
        hp.updated_at_ms = hp.created_at_ms;
        hp.hlc = nodes[1]->hlc->tick();
        REQUIRE(nodes[1]->store->upsert_handoff(hp));
        nodes[1]->transport->broadcast_delta_batch({}, {}, {}, {}, {hp}, {}, {});
    }

    // 5. Conflicting concurrent LWW session updates on node-1 and node-3 (split-brain style)
    {
        auto hlc_older = nodes[0]->hlc->tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        auto hlc_newer = nodes[2]->hlc->tick();

        core::SessionRecord s_old{"sess-conflict", "node-1", "", "repo", "b1", "Older Title", "active", false, "auto", {}, 100, 100, hlc_older};
        core::SessionRecord s_new{"sess-conflict", "node-3", "", "repo", "b1", "Winning Newer Title", "completed", false, "auto", {}, 100, 200, hlc_newer};

        nodes[0]->store->upsert_session(s_old);
        nodes[2]->store->upsert_session(s_new);
        nodes[0]->transport->broadcast_delta_batch({s_old}, {}, {}, {}, {}, {}, {});
        nodes[2]->transport->broadcast_delta_batch({s_new}, {}, {}, {}, {}, {}, {});
    }

    // 6. Restart node-4 and node-5 on their persisted ports: verify persistent outbox drain + convergence
    for (std::size_t idx : {std::size_t{3}, std::size_t{4}}) {
        nodes[idx]->cfg_mgr = std::make_unique<core::ConfigManager>(nodes[idx]->config_path);
        nodes[idx]->transport = std::make_unique<net::MeshTransport>(
            *nodes[idx]->cfg_mgr, *nodes[idx]->store, *nodes[idx]->hlc, nodes[idx]->keys);
        std::string rerr;
        REQUIRE(nodes[idx]->transport->start(rerr));
    }

    REQUIRE(wait_until([&]() {
        for (const auto& n : nodes) {
            const auto ho = n->store->get_handoff("ho-offline-test");
            if (!ho.has_value()) return false;
            const auto sc = n->store->get_session("sess-conflict");
            if (!sc.has_value() || sc->title != "Winning Newer Title") return false;
        }
        return true;
    }, 8000));

    // 7. Claim handoff on node-5 and confirm propagation back to node-1..4
    {
        auto ho = nodes[4]->store->get_handoff("ho-offline-test").value();
        ho.status = "claimed";
        ho.claimed_by_node = "node-5";
        ho.updated_at_ms = core::now_ms();
        ho.hlc = nodes[4]->hlc->tick();
        REQUIRE(nodes[4]->store->upsert_handoff(ho));
        nodes[4]->transport->broadcast_delta_batch({}, {}, {}, {}, {ho}, {}, {});
    }

    REQUIRE(wait_until([&]() {
        const auto ho = nodes[0]->store->get_handoff("ho-offline-test");
        return ho.has_value() && ho->status == "claimed" && ho->claimed_by_node == "node-5";
    }, 6000));

    for (auto& n : nodes) {
        n->transport->stop();
        n->store->close();
    }
}

TEST_CASE("5-node zero-touch auto-pairing via invite token and PEER_GOSSIP full mesh formation", "[integration][5nodes][pairing]") {
    const std::filesystem::path base_dir =
        std::filesystem::temp_directory_path() / ("agmesh_5node_zt_" + std::to_string(core::now_us()));
    std::filesystem::create_directories(base_dir);

    struct Cleanup {
        std::filesystem::path p;
        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove_all(p, ec);
        }
    } cleanup{base_dir};

    std::vector<std::unique_ptr<ClusterNodeHarness>> nodes(5);

    // 1. Initialize 5 isolated nodes WITHOUT any preconfigured peers or shared cluster PSK on nodes 2..5
    for (int i = 0; i < 5; ++i) {
        auto h = std::make_unique<ClusterNodeHarness>();
        h->node_id = "zt-node-" + std::to_string(i + 1);
        h->root_dir = base_dir / h->node_id;
        std::filesystem::create_directories(h->root_dir);
        h->config_path = h->root_dir / "config.toml";
        h->keys = net::CryptoEngine::generate_node_keypair(h->node_id);

        auto cfg = core::ConfigManager::create_default_config(h->node_id);
        cfg.data_dir = h->root_dir.string();
        cfg.db_path = (h->root_dir / "mesh.db").string();
        cfg.key_file = (h->root_dir / "node.key").string();
        cfg.network.listen_address = "127.0.0.1";
        cfg.network.listen_port = 0;
        cfg.network.enable_mdns_discovery = false;
        cfg.network.heartbeat_interval_ms = 500;
        cfg.network.peer_timeout_ms = 4000;
        cfg.network.io_threads = 2;
        if (i == 0) {
            cfg.network.cluster_psk_hex = net::CryptoEngine::generate_random_psk_hex();
        }
        core::ConfigManager::save_to_file(cfg, h->config_path);

        h->cfg_mgr = std::make_unique<core::ConfigManager>(h->config_path);
        h->store = std::make_unique<storage::SqliteStore>(cfg.db_path);
        std::string db_err;
        REQUIRE(h->store->open_and_migrate(db_err));
        h->hlc = std::make_unique<core::HybridLogicalClock>(h->node_id);
        h->transport = std::make_unique<net::MeshTransport>(*h->cfg_mgr, *h->store, *h->hlc, h->keys);

        std::string net_err;
        REQUIRE(h->transport->start(net_err));
        REQUIRE(h->transport->bound_port() > 0);
        nodes[static_cast<std::size_t>(i)] = std::move(h);
    }

    // 2. Host 1 opens a PairingWindow with max_uses=4 for Hosts 2..5
    const std::uint16_t port1 = nodes[0]->transport->bound_port();
    const auto pstatus = nodes[0]->transport->start_pairing_window(
        600, 4, port1, {net::EndpointCandidate{"127.0.0.1", port1, "lo0", "lan", 50}});
    REQUIRE(pstatus.active);
    REQUIRE(pstatus.max_uses == 4);
    REQUIRE(pstatus.invite_token.rfind("agmesh://join/v1/", 0) == 0);

    // 3. Hosts 2..5 join exclusively using the invite token
    for (std::size_t i = 1; i < 5; ++i) {
        const auto jr = nodes[i]->transport->join_cluster(pstatus.invite_token, "", 8000);
        REQUIRE(jr.ok);
        REQUIRE(jr.host_node_id == "zt-node-1");
        REQUIRE(jr.winning_route_ip == "127.0.0.1");
        REQUIRE(jr.winning_route_port == port1);
    }

    // 4. Verify PEER_GOSSIP introduced all nodes to one another into a complete 5-node mesh (4 peers each)
    REQUIRE(wait_until([&]() {
        for (const auto& n : nodes) {
            if (n->transport->connected_peers().size() < 4) return false;
        }
        return true;
    }, 10000));

    // 5. Publish a test chat on zt-node-4 and verify full mesh replication across all 5 nodes
    {
        core::SessionRecord s;
        s.session_id = "sess-zt-mesh";
        s.node_id = "zt-node-4";
        s.repo_name = "ag-mesh";
        s.git_branch = "feat/zero-touch";
        s.title = "Zero-touch auto-paired session";
        s.created_at_ms = core::now_ms();
        s.updated_at_ms = s.created_at_ms;
        s.hlc = nodes[3]->hlc->tick();
        REQUIRE(nodes[3]->store->upsert_session(s));

        core::MessageRecord m;
        m.message_id = "msg-zt-1";
        m.session_id = s.session_id;
        m.node_id = "zt-node-4";
        m.seq_index = 0;
        m.role = "assistant";
        m.content = "Zero-touch onboarding and PEER_GOSSIP converged on all 5 hosts";
        m.created_at_ms = s.created_at_ms;
        m.content_sha256 = core::CrdtResolver::compute_message_hash(m.session_id, m.role, m.seq_index, m.content);
        m.hlc = s.hlc;
        bool ins = false;
        REQUIRE(nodes[3]->store->append_message(m, ins));

        nodes[3]->transport->broadcast_delta_batch({s}, {m}, {}, {}, {}, {}, {});
    }

    REQUIRE(wait_until([&]() {
        for (const auto& n : nodes) {
            if (!n->store->get_session("sess-zt-mesh").has_value()) return false;
            if (n->store->search_fts("PEER_GOSSIP converged").empty()) return false;
        }
        return true;
    }, 8000));

    for (auto& n : nodes) {
        n->transport->stop();
        n->store->close();
    }
}
