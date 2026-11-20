# Security Model & Policy

## 1. Cryptographic Guarantees
- **Peer Authentication:** Every node generates a dedicated **Ed25519** identity keypair (`ag-mesh keygen`). Connections are rejected (`ERR_UNAUTHORIZED_PEER` / `ERR_AUTH_FAILED`) unless:
  1. The peer proves knowledge of the 256-bit `cluster_psk_hex` via HMAC-SHA256 over ephemeral X25519 keys.
  2. The peer's Ed25519 public key SHA-256 fingerprint exists in the node's explicit `[[peers]]` whitelist.
  3. The Ed25519 handshake transcript signature verifies cleanly.
- **Forward Secrecy & AEAD:** Session keys are derived per connection from ephemeral **X25519** ECDH + `cluster_psk` via `HKDF-SHA256`, encrypting all post-handshake frames with **ChaCha20-Poly1305** bound to the 24-byte frame header as AAD.
- **Zero-Knowledge Relaying:** In Hub-and-Spoke mode, the Relay Coordinator routes `RelayEnvelope` frames containing inner ChaCha20-Poly1305 ciphertexts. The relay cannot read message contents or summaries.

## 2. Secret Leakage Prevention
- `SecretSanitizer` runs synchronously on every local session bundle, handoff diff, and MCP `cluster_publish_update` call before storing into replicated tables or transmitting over the network.
- Configure directory allow/deny globs (`privacy.allow_globs`, `privacy.deny_globs`), `privacy.metadata_only = true`, or per-session `is_private = true` to prevent sensitive workspaces from ever leaving the local host.

## 3. Local Attack Surface
- **IPC Permissions:** Unix domain sockets are created with mode `0600` (owner read/write only); Windows Named Pipes restrict access to local user sessions.
- **MCP HTTP Bind:** Binds strictly to `127.0.0.1` by default.
- **Private Keys:** `ag-mesh export` never exports `node.key` or `cluster_psk_hex`.

## 4. Prompt-Injection Mitigation
- Remote chat logs and artifacts synced from other hosts are explicitly tagged as untrusted reference data in MCP tool responses and forbidden from being executed as commands in `skill/antigravity-mesh/SKILL.md`.

## Reporting Vulnerabilities
Please open a private security advisory on GitHub or contact maintainers directly before public disclosure.
