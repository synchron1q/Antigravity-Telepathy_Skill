-- ag-mesh Unified Local & Replicated SQLite Schema (WAL + FTS5)
-- Schema Version: 1

PRAGMA journal_mode = WAL;
PRAGMA synchronous = NORMAL;
PRAGMA foreign_keys = ON;
PRAGMA temp_store = MEMORY;

CREATE TABLE IF NOT EXISTS schema_migrations (
    version         INTEGER PRIMARY KEY,
    applied_at_ms   INTEGER NOT NULL,
    description     TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS nodes (
    node_id         TEXT PRIMARY KEY,
    hostname        TEXT NOT NULL,
    os_name         TEXT NOT NULL,
    arch            TEXT NOT NULL,
    tags_json       TEXT NOT NULL DEFAULT '[]',
    projects_json   TEXT NOT NULL DEFAULT '[]',
    pubkey_fp       TEXT NOT NULL,
    endpoint        TEXT NOT NULL DEFAULT '',
    online_status   TEXT NOT NULL DEFAULT 'offline', -- 'online' | 'degraded' | 'offline'
    rtt_ms          REAL NOT NULL DEFAULT -1.0,
    last_seen_ms    INTEGER NOT NULL DEFAULT 0,
    hlc_physical_ms INTEGER NOT NULL DEFAULT 0,
    hlc_logical     INTEGER NOT NULL DEFAULT 0,
    origin_node_id  TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS workspaces (
    workspace_id    TEXT PRIMARY KEY,
    node_id         TEXT NOT NULL,
    root_path       TEXT NOT NULL,
    repo_name       TEXT NOT NULL,
    git_branch      TEXT NOT NULL DEFAULT '',
    git_commit      TEXT NOT NULL DEFAULT '',
    is_open         INTEGER NOT NULL DEFAULT 0,
    active_task     TEXT NOT NULL DEFAULT '',
    last_active_ms  INTEGER NOT NULL DEFAULT 0,
    hlc_physical_ms INTEGER NOT NULL DEFAULT 0,
    hlc_logical     INTEGER NOT NULL DEFAULT 0,
    origin_node_id  TEXT NOT NULL,
    FOREIGN KEY(node_id) REFERENCES nodes(node_id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_workspaces_node ON workspaces(node_id, is_open);
CREATE INDEX IF NOT EXISTS idx_workspaces_repo ON workspaces(repo_name);

CREATE TABLE IF NOT EXISTS sessions (
    session_id      TEXT PRIMARY KEY,
    node_id         TEXT NOT NULL,
    workspace_path  TEXT NOT NULL DEFAULT '',
    repo_name       TEXT NOT NULL DEFAULT '',
    git_branch      TEXT NOT NULL DEFAULT '',
    title           TEXT NOT NULL DEFAULT '',
    status          TEXT NOT NULL DEFAULT 'active', -- 'active' | 'idle' | 'completed' | 'archived'
    is_private      INTEGER NOT NULL DEFAULT 0,
    source_adapter  TEXT NOT NULL DEFAULT 'auto',
    tags_json       TEXT NOT NULL DEFAULT '[]',
    created_at_ms   INTEGER NOT NULL,
    updated_at_ms   INTEGER NOT NULL,
    hlc_physical_ms INTEGER NOT NULL DEFAULT 0,
    hlc_logical     INTEGER NOT NULL DEFAULT 0,
    origin_node_id  TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_sessions_node_updated ON sessions(node_id, updated_at_ms DESC);
CREATE INDEX IF NOT EXISTS idx_sessions_repo ON sessions(repo_name, status);

CREATE TABLE IF NOT EXISTS messages (
    message_id      TEXT PRIMARY KEY,
    session_id      TEXT NOT NULL,
    node_id         TEXT NOT NULL,
    seq_index       INTEGER NOT NULL,
    role            TEXT NOT NULL, -- 'user' | 'assistant' | 'system' | 'tool'
    content         TEXT NOT NULL,
    touched_files_json TEXT NOT NULL DEFAULT '[]',
    content_sha256  TEXT NOT NULL UNIQUE,
    created_at_ms   INTEGER NOT NULL,
    hlc_physical_ms INTEGER NOT NULL DEFAULT 0,
    hlc_logical     INTEGER NOT NULL DEFAULT 0,
    origin_node_id  TEXT NOT NULL,
    FOREIGN KEY(session_id) REFERENCES sessions(session_id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_messages_session_seq ON messages(session_id, seq_index ASC);
CREATE INDEX IF NOT EXISTS idx_messages_created ON messages(created_at_ms DESC);

CREATE TABLE IF NOT EXISTS session_summaries (
    session_id          TEXT PRIMARY KEY,
    node_id             TEXT NOT NULL,
    current_goal        TEXT NOT NULL DEFAULT '',
    decisions_json      TEXT NOT NULL DEFAULT '[]',
    modified_files_json TEXT NOT NULL DEFAULT '[]',
    bugs_found_json     TEXT NOT NULL DEFAULT '[]',
    open_questions_json TEXT NOT NULL DEFAULT '[]',
    summary_text        TEXT NOT NULL DEFAULT '',
    updated_at_ms       INTEGER NOT NULL,
    hlc_physical_ms     INTEGER NOT NULL DEFAULT 0,
    hlc_logical         INTEGER NOT NULL DEFAULT 0,
    origin_node_id      TEXT NOT NULL,
    FOREIGN KEY(session_id) REFERENCES sessions(session_id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS handoffs (
    handoff_id          TEXT PRIMARY KEY,
    from_node_id        TEXT NOT NULL,
    target_node_id      TEXT NOT NULL DEFAULT '',
    session_id          TEXT NOT NULL DEFAULT '',
    claimed_by_node     TEXT NOT NULL DEFAULT '',
    claimed_session_id  TEXT NOT NULL DEFAULT '',
    repo_name           TEXT NOT NULL DEFAULT '',
    workspace_path      TEXT NOT NULL DEFAULT '',
    git_branch          TEXT NOT NULL DEFAULT '',
    git_commit          TEXT NOT NULL DEFAULT '',
    task_description    TEXT NOT NULL,
    uncommitted_diff    TEXT NOT NULL DEFAULT '',
    blockers_json       TEXT NOT NULL DEFAULT '[]',
    next_steps_json     TEXT NOT NULL DEFAULT '[]',
    artifact_refs_json  TEXT NOT NULL DEFAULT '[]',
    status              TEXT NOT NULL DEFAULT 'pending', -- 'pending' | 'claimed' | 'completed' | 'cancelled'
    created_at_ms       INTEGER NOT NULL,
    updated_at_ms       INTEGER NOT NULL,
    hlc_physical_ms     INTEGER NOT NULL DEFAULT 0,
    hlc_logical         INTEGER NOT NULL DEFAULT 0,
    origin_node_id      TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_handoffs_status ON handoffs(status, updated_at_ms DESC);

CREATE TABLE IF NOT EXISTS artifacts (
    artifact_id     TEXT PRIMARY KEY,
    session_id      TEXT NOT NULL,
    node_id         TEXT NOT NULL,
    relative_path   TEXT NOT NULL,
    mime_type       TEXT NOT NULL DEFAULT 'text/markdown',
    content_text    TEXT NOT NULL DEFAULT '',
    size_bytes      INTEGER NOT NULL DEFAULT 0,
    content_sha256  TEXT NOT NULL,
    updated_at_ms   INTEGER NOT NULL,
    hlc_physical_ms INTEGER NOT NULL DEFAULT 0,
    hlc_logical     INTEGER NOT NULL DEFAULT 0,
    origin_node_id  TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_artifacts_session ON artifacts(session_id);

CREATE TABLE IF NOT EXISTS locks (
    lock_key        TEXT PRIMARY KEY, -- e.g. 'repo:ag-mesh/src/net'
    owner_node_id   TEXT NOT NULL,
    owner_session_id TEXT NOT NULL DEFAULT '',
    reason          TEXT NOT NULL DEFAULT '',
    acquired_at_ms  INTEGER NOT NULL,
    expires_at_ms   INTEGER NOT NULL,
    released        INTEGER NOT NULL DEFAULT 0,
    hlc_physical_ms INTEGER NOT NULL DEFAULT 0,
    hlc_logical     INTEGER NOT NULL DEFAULT 0,
    origin_node_id  TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS sync_cursors (
    peer_node_id        TEXT PRIMARY KEY,
    last_hlc_physical   INTEGER NOT NULL DEFAULT 0,
    last_hlc_logical    INTEGER NOT NULL DEFAULT 0,
    vector_clock_json   TEXT NOT NULL DEFAULT '{}',
    last_sync_ms        INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS outbox_queue (
    event_id        TEXT PRIMARY KEY,
    target_node_id  TEXT NOT NULL, -- '*' for broadcast to all authorized peers
    frame_type      INTEGER NOT NULL,
    payload_json    TEXT NOT NULL,
    created_at_ms   INTEGER NOT NULL,
    attempts        INTEGER NOT NULL DEFAULT 0,
    next_retry_ms   INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX IF NOT EXISTS idx_outbox_target_retry ON outbox_queue(target_node_id, next_retry_ms);

CREATE TABLE IF NOT EXISTS tombstones (
    target_type     TEXT NOT NULL, -- 'session' | 'artifact' | 'handoff'
    target_id       TEXT NOT NULL,
    purged_at_ms    INTEGER NOT NULL,
    hlc_physical_ms INTEGER NOT NULL DEFAULT 0,
    hlc_logical     INTEGER NOT NULL DEFAULT 0,
    origin_node_id  TEXT NOT NULL,
    PRIMARY KEY(target_type, target_id)
);

CREATE TABLE IF NOT EXISTS redaction_audit_log (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id      TEXT NOT NULL,
    secret_type     TEXT NOT NULL,
    redacted_marker TEXT NOT NULL,
    source_path     TEXT NOT NULL DEFAULT '',
    detected_at_ms  INTEGER NOT NULL
);

-- FTS5 Full-Text Index across sessions, messages, summaries, and artifacts (supports Cyrillic + Latin via unicode61)
CREATE VIRTUAL TABLE IF NOT EXISTS fts_context USING fts5(
    entity_type UNINDEXED,
    entity_id UNINDEXED,
    session_id UNINDEXED,
    node_id UNINDEXED,
    repo_name,
    title,
    body,
    tokenize = 'unicode61 remove_diacritics 1'
);
