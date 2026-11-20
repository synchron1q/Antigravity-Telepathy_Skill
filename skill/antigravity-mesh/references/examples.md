# `ag-mesh` 5-Host Usage Examples

This reference illustrates real-world multi-host workflows across a 5-machine cluster:
1. `desktop-win` (Windows 11 workstation)
2. `laptop-linux` (Ubuntu developer laptop)
3. `macbook-arm64` (macOS M3 Pro)
4. `home-server` (Debian home lab / WireGuard node)
5. `vps-relay` (Cloud VPS relay / build node)

---

## Scenario 1: Parallel Microservice Development Across 5 Hosts

**Context:** You are building an API on `desktop-win` while another Antigravity session on `laptop-linux` is modifying the shared protobuf contract, and `home-server` is running integration benchmarks.

**Agent Flow on `desktop-win`:**
1. User opens a new chat: *"Add rate limiting to the gRPC gateway."*
2. Agent triggers passive awareness -> calls `cluster_get_overview()`.
3. Output shows:
   - `laptop-linux` is active in `platform-monorepo` (`feat/proto-v2`), holding soft lock `repo:platform-monorepo/proto/`.
   - `home-server` is running `perf-suite` (`main`).
4. Agent acquires non-conflicting lock:
   ```json
   cluster_manage_lock({
     "action": "acquire",
     "lock_key": "repo:platform-monorepo/services/gateway",
     "reason": "Implementing token-bucket rate limiter"
   })
   ```
5. Agent warns user: *"Note: `laptop-linux` currently holds a soft lock on `proto/`. I locked `services/gateway/` and will avoid editing `.proto` files directly."*
6. After completing implementation, agent calls `cluster_publish_update(...)` and `cluster_manage_lock(action="release", ...)`.

---

## Scenario 2: Seamless Task Handoff from Laptop to Desktop

**Context:** You start refactoring a tricky memory allocator bug on `laptop-linux` on a train, hit a test failure, and switch to `desktop-win` at home.

**On `laptop-linux` before closing lid:**
User: *"Package this debugging session so I can finish it on my Windows desktop."*
Agent:
1. Inspects `git branch` (`fix/arena-oom`), `git rev-parse --short HEAD` (`9a41c0d`), and `git diff`.
2. Calls:
   ```json
   cluster_create_handoff({
     "session_id": "sess-train-8821",
     "target_node_id": "desktop-win",
     "repo_name": "ag-mesh",
     "git_branch": "fix/arena-oom",
     "git_commit": "9a41c0d",
     "task_description": "Fix ASan heap-use-after-free in PeerSession strand teardown",
     "blockers": ["Need MSVC + Windows NamedPipe verification"],
     "next_steps": [
       "Re-run test_cluster_5nodes under ASan",
       "Verify PeerSession::close_internal() strand lifetime"
     ]
   })
   ```
3. Returns `handoff_id: "ho-7d2a91bc4f01"`.

**On `desktop-win`:**
User: *"Pick up my handoff from the laptop."*
Agent:
1. Calls `cluster_get_overview()` -> finds pending handoff `ho-7d2a91bc4f01`.
2. Calls `cluster_claim_handoff({"handoff_id": "ho-7d2a91bc4f01", "new_session_id": "sess-win-104"})`.
3. Checks local Git commit via `git rev-parse --short HEAD`:
   - Sees `desktop-win` is on `main` (`4410e2b`) instead of `fix/arena-oom` (`9a41c0d`).
   - Tells user: *"Claimed handoff `ho-7d2a91bc4f01`. Local repo is currently on `main` (`4410e2b`), while the handoff expects `fix/arena-oom` (`9a41c0d`). Shall I run `git fetch && git checkout fix/arena-oom`?"*

---

## Scenario 3: Finding Yesterday's Fix From the Build Server

**Context:** While configuring TLS on `macbook-arm64`, you hit `EVP_CTRL_AEAD_SET_TAG failed` during OpenSSL 3.x ChaCha20-Poly1305 decryption. You remember solving a similar issue yesterday on `home-server`.

**Agent Flow on `macbook-arm64`:**
1. Agent calls:
   ```json
   cluster_search_context({
     "query": "EVP_CTRL_AEAD_SET_TAG ChaCha20",
     "limit": 5
   })
   ```
2. FTS5 returns a high-BM25 hit from `home-server` session `sess-srv-319`.
3. Agent calls L2 summary first:
   ```json
   cluster_read_session({
     "session_id": "sess-srv-319",
     "mode": "summary"
   })
   ```
4. Summary reveals the exact root cause (passing `const` tag buffer to OpenSSL `EVP_CIPHER_CTX_ctrl` and mismatching AAD byte order). Agent applies the fix on `macbook-arm64` in seconds without pulling megabytes of raw logs.

---

## Scenario 4: Zero-Touch Pairing of 5 Hosts via Invite Token / IP + PIN

**On Host 1 (`desktop-win`):**
User: *"Узнай мой адрес и жди коннект для 4 остальных машин."*
Agent:
1. Calls `cluster_start_pairing({"ttl_seconds": 600, "max_uses": 4})`.
2. Presents detected routes (`[vpn] 10.8.0.1:47890`, `[lan] 192.168.1.10:47890`), PIN `482-915`, and invite token `agmesh://join/v1/...`.

**On Host 2 (`laptop-linux`):**
User pastes: *"Подключись: `agmesh://join/v1/eyJ2IjoxLC...`"* (or *"Подключись к 10.8.0.1 код 482-915"*).
Agent:
1. Immediately calls `cluster_join_with_token(...)` — Happy Eyeballs probes VPN and LAN in parallel and connects over `10.8.0.1:47890` (`vpn`).
2. Host 1 pushes `PAIR_WELCOME` with `cluster_psk` and broadcasts `PEER_GOSSIP` to all already-paired hosts (`node-2..node-5`), automatically forming a full 5-node mesh.
3. Agent calls `cluster_get_overview()` and displays the live 5-host table.
