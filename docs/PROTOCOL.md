# `ag-mesh` Wire Protocol Specification (`agmesh/1.0`)

**Protocol Version:** `0x0100` (Major `1`, Minor `0`)  
**Magic Bytes:** `0x41 0x47 0x4D 0x53` (`"AGMS"`, big-endian `0x41474D53`)  
**Byte Order:** Network Byte Order (Big-Endian) for all multi-byte header and length fields.

---

## 1. Binary Frame Format (24-byte Fixed Header)

Every packet transmitted over TCP (or encapsulated inside a zero-knowledge relay envelope) consists of a fixed 24-byte header followed by a variable-length payload (`0 .. 16 MiB`).

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                        Magic (0x41474D53)                     |  Bytes 0..3
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|     Version (uint16)          |  Flags (u8)   | FrameType(u8) |  Bytes 4..7
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                       Stream ID (uint32)                      |  Bytes 8..11
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    Sequence Number (uint64)                   |  Bytes 12..19
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                     Payload Length (uint32)                   |  Bytes 20..23
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                     Header CRC32C (uint32)                    |  Bytes 24..27 (see Note)
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

> **Canonical 24-byte Header Profile (`agmesh::net::FrameHeader`):**
> To maintain an exact 24-byte aligned header on the wire (`6 x uint32`), `seq_no` is 32-bit per-stream sequence counter (`uint32_t`, wraps with epoch rekeying) and `header_crc32c` occupies bytes `20..23`:
> - Bytes `0..3`: `magic` (`uint32_t` = `0x41474D53`)
> - Bytes `4..5`: `version` (`uint16_t` = `0x0100`)
> - Byte  `6`:    `flags` (`uint8_t` bitmask)
> - Byte  `7`:    `frame_type` (`uint8_t`)
> - Bytes `8..11`: `stream_id` (`uint32_t`, `0` = Control/Heartbeat, `>=1` = Logical Data/Artifact Streams)
> - Bytes `12..15`: `seq_no` (`uint32_t`)
> - Bytes `16..19`: `payload_len` (`uint32_t`, max `16 * 1024 * 1024` bytes)
> - Bytes `20..23`: `header_crc32c` (`uint32_t`, Castagnoli CRC32C over bytes `0..19`)

### 1.1 Header Flags (`uint8_t`)
| Bit | Mask | Name | Description |
|-----|------|------|-------------|
| 0 | `0x01` | `FLAG_COMPRESSED_ZSTD` | Payload is compressed with Zstandard (`zstd`) before AEAD encryption. |
| 1 | `0x02` | `FLAG_ENCRYPTED_AEAD` | Payload is AEAD-encrypted (`ChaCha20-Poly1305` with 12-byte nonce + 16-byte tag). |
| 2 | `0x04` | `FLAG_RELAY_ENVELOPE` | Frame contains an outer zero-knowledge relay routing envelope. |
| 3 | `0x08` | `FLAG_END_OF_STREAM` | Final chunk of a logical stream transfer. |
| 4 | `0x10` | `FLAG_ACK_REQUIRED` | Receiver must acknowledge `seq_no` with `FRAME_ACK`. |
| 5..7 | `0xE0` | `RESERVED` | Must be `0` in `agmesh/1.0`. |

---

## 2. Logical Stream Multiplexing

- **`stream_id = 0` (Control Plane):** Reserved exclusively for handshake frames, `HEARTBEAT_PING`, `HEARTBEAT_PONG`, `ERROR`, `ACK`, and vector-clock anti-entropy exchange (`SYNC_CLOCK_REQUEST` / `SYNC_CLOCK_RESPONSE`). Stream `0` frames bypass artifact bandwidth queues so RTT measurement and failure detection never experience head-of-line blocking from large artifact transfers.
- **`stream_id` odd (`1, 3, 5, ...`):** Initiated by the TCP/Noise initiator (client side).
- **`stream_id` even (`2, 4, 6, ...`):** Initiated by the TCP/Noise responder (server side).
- Maximum concurrent active streams per peer session: `256` (configurable).

---

## 3. Cryptographic Handshake State Machine (Noise_XX + Cluster PSK + Ed25519 Whitelist)

All peers authenticate using an X25519 ephemeral/static Noise_XX-inspired 3-message authenticated key exchange bound to:
1. A cluster-wide Pre-Shared Key (`cluster_psk`, 32-byte BLAKE2b/SHA-256 derived secret).
2. Each node's long-term **Ed25519 identity keypair** (`ag-mesh keygen`).
3. An explicit per-node **authorized peer fingerprint whitelist** (`SHA-256(ed25519_pubkey)`).

### 3.1 Handshake Finite State Machine (FSM)

```
Initiator (Client)                                    Responder (Server)
------------------                                    ------------------
State: INIT                                           State: LISTEN
  |-- Generate e_i (X25519 ephemeral)                   |
  |-- Compute mac = HMAC(psk, e_i || node_id_i)         |
  |-- [1] HANDSHAKE_INIT ------------------------------>| State: WAIT_INIT -> Verify PSK MAC
  |   { protocol_ver, node_id_i, e_i_pub,               |   Generate e_r (X25519 ephemeral)
  |     ed25519_pub_i, nonce_i, psk_hmac }              |   Check ed25519_pub_i fingerprint in whitelist
  |                                                     |   Compute DH1 = X25519(e_r, e_i)
  |                                                     |   Sign transcript_1 with Ed25519_priv_r
  |                                                     | State: WAIT_FINISH
  |<-- [2] HANDSHAKE_RESP ------------------------------|
  |   { node_id_r, e_r_pub, ed25519_pub_r,              |
  |     sig_r(transcript_1), psk_hmac_r }               |
State: VERIFY_RESP                                      |
  |-- Verify ed25519_pub_r in whitelist                 |
  |-- Verify sig_r over transcript_1                    |
  |-- Compute DH1 = X25519(e_i, e_r)                    |
  |-- Derive tx_key, rx_key = HKDF-SHA256(DH1 || psk)   |
  |-- Sign transcript_2 with Ed25519_priv_i             |
  |-- [3] HANDSHAKE_FINISH (AEAD encrypted) ----------->| Verify sig_i over transcript_2
  |   { sig_i(transcript_2), capabilities,              | Derive rx_key, tx_key = HKDF-SHA256(DH1 || psk)
  |     hlc_now, node_metadata }                        | State: ESTABLISHED
State: ESTABLISHED                                      |
```

If **any** check fails (invalid PSK HMAC, unknown Ed25519 fingerprint, invalid signature, protocol major mismatch), the responder immediately emits `FRAME_ERROR` (`ERR_AUTH_FAILED` or `ERR_UNAUTHORIZED_PEER`) and closes the socket.

### 3.2 AEAD Payload Framing (`FLAG_ENCRYPTED_AEAD`)
When `FLAG_ENCRYPTED_AEAD` is set:
`wire_payload = nonce_12bytes || ChaCha20_Poly1305_Ciphertext || tag_16bytes`  
AAD (Additional Authenticated Data) = exact 24-byte `FrameHeader` (with `header_crc32c` included).

---

## 4. Message Type Catalog (`frame_type`)

| Code | Name | Stream | Direction | Description |
|------|------|--------|-----------|-------------|
| `0x01` | `HANDSHAKE_INIT` | `0` | I -> R | Phase 1 Noise/Ed25519 identity + ephemeral key + PSK tag. |
| `0x02` | `HANDSHAKE_RESP` | `0` | R -> I | Phase 2 responder ephemeral key + Ed25519 signature. |
| `0x03` | `HANDSHAKE_FINISH` | `0` | I -> R | Phase 3 initiator signature + node metadata + capabilities. |
| `0x04` | `HEARTBEAT_PING` | `0` | Both | Keepalive probe carrying `sender_ts_us` and `hlc`. |
| `0x05` | `HEARTBEAT_PONG` | `0` | Both | Echoes `sender_ts_us` for exact RTT computation. |
| `0x06` | `FRAME_ACK` | `0` | Both | Explicit acknowledgment of `(ack_stream_id, ack_seq_no)`. |
| `0x07` | `FRAME_ERROR` | `0` | Both | Structured error code (`uint16_t`) + UTF-8 diagnostic message. |
| `0x08` | `PAIR_REQUEST` | `0` | Joiner -> Host | Zero-touch pairing handshake carrying X25519 ephemeral key, Ed25519 identity, and `HMAC-SHA256(pairing_secret / pin, transcript)`. |
| `0x09` | `PAIR_WELCOME` | `0` | Host -> Joiner | Ed25519-signed & ChaCha20-Poly1305 encrypted onboarding bundle (`cluster_psk`, host profile, peer list). |
| `0x0A` | `PAIR_REJECT` | `0` | Host -> Joiner | Pairing rejection notice (invalid secret/PIN, expired TTL, max uses reached, or 5-attempt lockout). |
| `0x10` | `NODE_HELLO_META` | `>=1` | Both | Full `NodeInfo` announcement (OS, hostname, tags, projects). |
| `0x11` | `SYNC_CLOCK_REQ` | `0` | Both | Vector Clock + per-node HLC cursor exchange to compute delta. |
| `0x12` | `SYNC_CLOCK_RESP` | `0` | Both | Response listing missing `(origin_node, hlc_range)` deltas. |
| `0x20` | `DELTA_EVENT_BATCH` | `>=1` | Both | Batch of CRDT delta records (sessions, messages, summaries, workspaces). |
| `0x21` | `ARTIFACT_CHUNK` | `>=1` | Both | Chunked artifact payload (max chunk 256 KiB, SHA-256 verified). |
| `0x22` | `HANDOFF_PACKET` | `>=1` | Both | Task handoff transfer object (`LWW-Element-Set` state). |
| `0x23` | `LOCK_OP` | `>=1` | Both | Distributed soft lock acquire / renew / release (`LWW-Element-Set`). |
| `0x24` | `PEER_GOSSIP` | `>=1` | Both | Signed peer introduction propagating newly paired nodes across existing mesh peers. |
| `0x25` | `TOMBSTONE_PURGE` | `>=1` | Both | Cluster-wide purge tombstone (`session_id` + cryptographic timestamp). |
| `0x30` | `RELAY_ENVELOPE` | `>=1` | Spoke <-> Hub | Zero-knowledge encrypted envelope `{ target_node_id, sender_node_id, sealed_inner_frame }`. |

---

## 5. Error Codes (`FRAME_ERROR`)

| Code | Constant | Description |
|------|----------|-------------|
| `0x0001` | `ERR_INVALID_MAGIC` | Frame magic mismatch or corrupted header CRC32C. |
| `0x0002` | `ERR_UNSUPPORTED_VERSION` | Incompatible major protocol version. |
| `0x0003` | `ERR_AUTH_FAILED` | Cluster PSK HMAC or Ed25519 signature verification failed. |
| `0x0004` | `ERR_UNAUTHORIZED_PEER` | Peer Ed25519 fingerprint is not in `authorized_peers`. |
| `0x0005` | `ERR_DECRYPT_FAILED` | ChaCha20-Poly1305 AEAD tag verification failed. |
| `0x0006` | `ERR_DECOMPRESS_FAILED` | Zstandard decompression failed or exceeded `max_uncompressed_bytes`. |
| `0x0007` | `ERR_QUOTA_EXCEEDED` | Attachment or frame payload exceeds configured node quota. |
| `0x0008` | `ERR_RELAY_TARGET_OFFLINE` | Hub relay could not deliver envelope because target spoke is offline. |
| `0x0009` | `ERR_RATE_LIMITED` | Peer exceeded token-bucket bandwidth/frame limits. |

---

## 6. Hybrid Logical Clocks (HLC), Vector Clocks & CRDT Semantics

### 6.1 HLC Encoding
An `HlcTimestamp` is a 96-bit tuple `(physical_ms: uint64, logical: uint32, node_id_hash: uint32)`:
- Lexicographic total order: `physical_ms` -> `logical` -> `node_id` string tie-breaker.
- On local write: `pt = max(old_pt, wall_clock_ms)`; if `pt == old_pt`, `logical++`, else `logical = 0`.
- On receive `(msg_pt, msg_l)`: `pt = max(old_pt, msg_pt, wall_clock_ms)`; `logical` advances strictly past both clocks if physical timestamps match.

### 6.2 CRDT Conflict Resolution
1. **Nodes, Workspaces, Session Metadata, Summaries, Handoffs, Locks (`LWW-Element-Set`):**
   - Each record carries `hlc_ts` (`physical_ms`, `logical`, `origin_node_id`).
   - When an incoming delta conflicts with a local record on primary key, the record with the strictly higher `(physical_ms, logical, origin_node_id)` wins deterministically across all nodes regardless of packet arrival order.
2. **Chat Messages & Events (`Append-Only Log` with SHA-256 Deduplication):**
   - Each message has a deterministic `content_sha256` (`SHA-256(session_id || role || timestamp_ms || normalized_content)`).
   - `INSERT OR IGNORE` semantics guarantee idempotent delivery and automatic deduplication during split-brain healing.
3. **Purge (`Tombstone`):**
   - A `TOMBSTONE_PURGE` record carries `(target_type, target_id, hlc_ts)` and permanently suppresses re-ingestion of messages older than `hlc_ts`.

---

## 7. Zero-Knowledge Hub-and-Spoke Relay Envelope

In Hub-and-Spoke topology, a Relay Coordinator routes frames between NATted spokes **without access to inner plaintext keys**:
- Outer TLS/Noise session authenticates `Spoke <-> Hub` using the Hub's Ed25519 key.
- Payload of `RELAY_ENVELOPE` (`0x30`):
  - `target_node_id` (UTF-8 length-prefixed string)
  - `sender_node_id` (UTF-8 length-prefixed string)
  - `inner_e2ee_payload` = End-to-end AEAD ciphertext encrypted under `HKDF(X25519(SpokeA_static, SpokeB_static) || cluster_psk)`
- The Hub reads only `(sender_node_id, target_node_id)` and forwards `inner_e2ee_payload` to `target_node_id` (or queues it if offline). The Hub cannot decrypt `inner_e2ee_payload`.

---

## 8. Backward Compatibility Rules

1. **Major version bump (`0x0200`):** Required only for breaking header or cryptographic primitive changes.
2. **Minor version bump (`0x0101`):** Additive `frame_type` codes or optional trailing JSON/CBOR fields inside payloads. Receivers MUST ignore unknown optional payload keys and reject unknown critical frame types with `ERR_UNSUPPORTED_VERSION` only when major version differs.
3. **SQLite schema compatibility:** Handled independently via `schema_migrations` version table; older nodes ignore unknown JSON fields inside synced event envelopes while preserving raw delta hashes.

---

## 9. Zero-Touch Auto-Pairing & Transitive Peer Gossip

### 9.1 Invite Token Format
Compact URI: `agmesh://join/v1/<base64url(json)>` (RFC 4648 URL-safe base64 without `=` padding) encoding:
```json
{
  "v": 1,
  "node_id": "host-1",
  "fingerprint": "<sha256(ed25519_pubkey)>",
  "endpoints": [
    {"ip": "100.64.0.1", "port": 47890, "if": "tailscale0", "kind": "vpn", "prio": 100},
    {"ip": "192.168.1.10", "port": 47890, "if": "eth0", "kind": "lan", "prio": 50}
  ],
  "secret": "<128-bit hex from RAND_bytes>"
}
```

### 9.2 Pairing Window Security Properties
- **TTL & Bounded Uses:** `ttl_seconds` (default `600`) and `max_uses` (default `1`, up to `10`).
- **Rate Limiting:** Maximum `5` failed HMAC/PIN attempts; on the 5th failure, `PairingWindow` locks out and closes immediately.
- **MITM Resistance:** Joiner derives `auth_key = SHA-256("agmesh-pair-v1:" || secret_or_pin)`, proves possession via `HMAC-SHA256(auth_key, transcript)` + `Ed25519_Sign`, verifies `host_fingerprint` against the invite token, and decrypts `PAIR_WELCOME` via `ChaCha20-Poly1305` keyed with `HKDF-SHA256(X25519(eph_j, eph_h) || auth_key)`.
- **Happy Eyeballs Dialing:** Joiner probes sorted VPN -> LAN -> Public candidates in parallel staggered at `150 ms` intervals, selecting the fastest reachable route.

### 9.3 Transitive `PEER_GOSSIP` (`0x24`)
Upon accepting a new peer, the host signs `(introducer_node_id || peer.node_id || peer.pubkey_fingerprint || peer.address || peer.port || ts_ms)` with its Ed25519 identity key and broadcasts `PEER_GOSSIP` to all established peers. Existing peers verify the introducer's signature against their local trust whitelist, atomically save the new peer to `config.toml`, and dial it directly to form a full P2P mesh.
