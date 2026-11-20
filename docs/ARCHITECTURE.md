# `ag-mesh` Architecture

## 1. Subsystem Overview

`ag-mesh` is structured around clean dependency boundaries under `include/agmesh/` and `src/`:

1. **Core & CRDT Layer (`core/`):**
   - `ConfigManager`: Parses and validates `config.toml`, supports glob allow/deny evaluation, and hot-reloads peers and privacy rules via `SIGHUP` or local IPC.
   - `HybridLogicalClock`, `VectorClockOps`, `CrdtResolver`: Provides deterministic total ordering (`(physical_ms, logical, origin_node_id)`) for `LWW-Element-Set` entities and SHA-256 deduplicated `Append-Only Log` message streams.

2. **Antigravity Inspection & Real-Time Capture (`adapter/`):**
   - `IAntigravityAdapter` isolates the sync engine from Antigravity internal storage changes.
   - Plugin parser chain (`JsonlTranscriptParser`, `MarkdownArtifactParser`, `SqliteReadOnlyParser`, `ProtobufStateParser`) safely reads IDE data using `mode=ro&immutable=1` URIs and partial-line-resilient JSONL scanning.
   - `FileWatcher` monitors local directories with `inotify` (Linux), `ReadDirectoryChangesW` (Windows), or `FSEvents` (macOS) using a debounce coalescer.
   - MCP hook fallback (`cluster_publish_update`) guarantees operation even if local IDE formats become encrypted or opaque.

3. **Secret Sanitizer (`sanitizer/`):**
   - Scans every outbound message, summary, diff, and artifact prior to network serialization or FTS5 indexing.
   - Combines regex pattern matching (AWS, GitHub, OpenAI, Anthropic, Google API, Telegram, JWT, Slack, PEM private keys, DB URIs) with Shannon entropy analysis (`>= 4.5` bits/char).
   - Replaces matches deterministically with `[REDACTED:TYPE:HASH4]` and logs metadata locally in `redaction_audit_log`.

4. **Unified Replicated Storage (`storage/`):**
   - Single SQLite database in `WAL` mode with `FTS5` (`unicode61 remove_diacritics 1`) supporting Cyrillic and Latin full-text search.
   - Manages `nodes`, `workspaces`, `sessions`, `messages`, `session_summaries`, `handoffs`, `artifacts`, `locks`, `sync_cursors`, `outbox_queue`, and `tombstones`.

5. **Encrypted Networking (`net/`):**
   - Multi-threaded Asio TCP transport with 24-byte CRC32C binary framing, Zstandard payload compression, and ChaCha20-Poly1305 AEAD encryption.
   - Stream `0` prioritizes heartbeats, error control, and sync clock negotiation to prevent head-of-line blocking during artifact transfers.
   - Supports both direct **P2P-Mesh** and **Hub-and-Spoke** topologies (with zero-knowledge `RelayEnvelope` forwarding).

6. **Local IPC, Embedded MCP Server, CLI & TUI (`ipc/`, `mcp/`, `cli/`, `tui/`, `service/`):**
   - Local IPC over `0600` Unix Domain Sockets or Windows Named Pipes.
   - Embedded MCP server supporting both `stdio` and local HTTP/SSE (`127.0.0.1`), exposing `/healthz` and Prometheus `/metrics`.

---

## 2. L1 / L2 / L3 Progressive Context Model

```
+-------------------------------------------------------------------+
| L1: cluster_get_overview (Always tiny, <2 KB)                     |
|   -> Online nodes, RTT, open repos, recent session titles, locks  |
+-------------------------------------------------------------------+
                                 |
                                 v
+-------------------------------------------------------------------+
| L2: cluster_read_session(mode="summary") & cluster_search_context |
|   -> Key architectural decisions, modified files, bugs, snippets  |
+-------------------------------------------------------------------+
                                 |
                                 v
+-------------------------------------------------------------------+
| L3: cluster_read_session(mode="messages", limit, max_chars)       |
|   -> Strict paginated slices loaded ONLY on explicit demand       |
+-------------------------------------------------------------------+
```
