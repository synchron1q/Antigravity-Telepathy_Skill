# Changelog

## [0.1.0] - 2026-11-20
### Added
- Initial public release of `ag-mesh` C++20 multi-host connector, E2EE mesh transport, and Antigravity MCP integration.
- `IAntigravityAdapter` plugin chain (read-only immutable SQLite, JSONL transcripts, Markdown artifacts, protobuf state, MCP hook fallback) and `ag-mesh probe`.
- Cross-platform `FileWatcher` (`inotify`, `ReadDirectoryChangesW`, `FSEvents`) with debouncing.
- Noise-style X25519 + Ed25519 + Cluster PSK authenticated transport on Asio with 24-byte CRC32C framing, Zstd compression, ChaCha20-Poly1305 AEAD, token-bucket rate limiting, and Hub-and-Spoke zero-knowledge relay support.
- Hybrid Logical Clocks (HLC), Vector Clocks, LWW-Element-Set, Append-Only Log deduplication, and persistent disk `outbox_queue`.
- SQLite WAL + FTS5 unified store with Cyrillic & Latin search, TTL message rotation, tombstones, and atomic JSON/Markdown export.
- Regex + Shannon entropy `SecretSanitizer` with deterministic redaction markers and audit logging.
- Embedded MCP JSON-RPC 2.0 server (`stdio` and HTTP/SSE `/mcp`, `/sse`, `/healthz`, `/metrics`).
- Complete CLI (`CLI11`), FTXUI dashboard (`ag-mesh tui`), OS service installers (`systemd`, `Windows Service`, `launchd`), and `skill/antigravity-mesh/` package.
- Catch2 unit tests, 5-node integration suite, Docker Compose relay setup, and GitHub Actions CI/Release workflows.
