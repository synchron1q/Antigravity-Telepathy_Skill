#include "agmesh/storage/sqlite_store.hpp"
#include "agmesh/core/hlc_crdt.hpp"
#include "agmesh/storage/schema_sql.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace agmesh::storage {

namespace {

class StmtGuard {
public:
    StmtGuard(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
            stmt_ = nullptr;
        }
    }
    ~StmtGuard() {
        if (stmt_) {
            sqlite3_finalize(stmt_);
        }
    }
    StmtGuard(const StmtGuard&) = delete;
    StmtGuard& operator=(const StmtGuard&) = delete;

    [[nodiscard]] sqlite3_stmt* get() const noexcept { return stmt_; }
    [[nodiscard]] bool valid() const noexcept { return stmt_ != nullptr; }

    void bind_text(int idx, const std::string& val) const {
        sqlite3_bind_text(stmt_, idx, val.c_str(), static_cast<int>(val.size()), SQLITE_TRANSIENT);
    }
    void bind_i64(int idx, std::int64_t val) const {
        sqlite3_bind_int64(stmt_, idx, static_cast<sqlite3_int64>(val));
    }
    void bind_u64(int idx, std::uint64_t val) const {
        sqlite3_bind_int64(stmt_, idx, static_cast<sqlite3_int64>(val));
    }
    void bind_int(int idx, int val) const {
        sqlite3_bind_int(stmt_, idx, val);
    }
    void bind_double(int idx, double val) const {
        sqlite3_bind_double(stmt_, idx, val);
    }

private:
    sqlite3_stmt* stmt_{nullptr};
};

std::string col_str(sqlite3_stmt* stmt, int col) {
    const unsigned char* txt = sqlite3_column_text(stmt, col);
    const int bytes = sqlite3_column_bytes(stmt, col);
    if (!txt || bytes <= 0) return {};
    return std::string(reinterpret_cast<const char*>(txt), static_cast<std::size_t>(bytes));
}

std::vector<std::string> parse_str_array(const std::string& json_str) {
    if (json_str.empty()) return {};
    try {
        auto j = nlohmann::json::parse(json_str);
        if (j.is_array()) {
            std::vector<std::string> out;
            out.reserve(j.size());
            for (const auto& el : j) {
                if (el.is_string()) out.push_back(el.get<std::string>());
            }
            return out;
        }
    } catch (...) {
    }
    return {};
}

std::string dump_str_array(const std::vector<std::string>& vec) {
    return nlohmann::json(vec).dump();
}

// Sanitizes arbitrary user search strings for safe FTS5 MATCH execution (supports Cyrillic & Latin).
std::string sanitize_fts5_query(const std::string& raw) {
    std::istringstream iss(raw);
    std::string token;
    std::vector<std::string> quoted_terms;
    while (iss >> token) {
        std::string cleaned;
        cleaned.reserve(token.size());
        for (char ch : token) {
            if (ch != '"' && ch != '*' && ch != '^' && ch != '(' && ch != ')') {
                cleaned.push_back(ch);
            }
        }
        if (!cleaned.empty()) {
            quoted_terms.push_back("\"" + cleaned + "\"");
        }
    }
    if (quoted_terms.empty()) return {};
    std::string out;
    for (std::size_t i = 0; i < quoted_terms.size(); ++i) {
        if (i > 0) out += " ";
        out += quoted_terms[i];
    }
    return out;
}

bool incoming_hlc_wins(sqlite3* db,
                       const char* select_sql,
                       const std::string& pk,
                       const core::HlcTimestamp& incoming) {
    StmtGuard check(db, select_sql);
    if (!check.valid()) return true;
    check.bind_text(1, pk);
    if (sqlite3_step(check.get()) == SQLITE_ROW) {
        core::HlcTimestamp existing;
        existing.physical_ms = static_cast<std::uint64_t>(sqlite3_column_int64(check.get(), 0));
        existing.logical = static_cast<std::uint32_t>(sqlite3_column_int64(check.get(), 1));
        existing.origin_node_id = col_str(check.get(), 2);
        return incoming >= existing;
    }
    return true;
}

} // namespace

SqliteStore::SqliteStore(std::filesystem::path db_path)
    : db_path_(std::move(db_path)) {}

SqliteStore::~SqliteStore() {
    close();
}

bool SqliteStore::exec_sql(const std::string& sql, std::string& error_out) {
    char* errmsg = nullptr;
    const int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        error_out = errmsg ? std::string(errmsg) : sqlite3_errmsg(db_);
        if (errmsg) sqlite3_free(errmsg);
        return false;
    }
    return true;
}

bool SqliteStore::open_and_migrate(std::string& error_out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_) return true;

    if (db_path_.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(db_path_.parent_path(), ec);
    }

    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(db_path_.string().c_str(), &db_, flags, nullptr) != SQLITE_OK) {
        error_out = db_ ? sqlite3_errmsg(db_) : "Failed to allocate sqlite3 handle";
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        return false;
    }

    sqlite3_busy_timeout(db_, 5000);

    if (!exec_sql(std::string(kSchemaSqlV1), error_out)) {
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }

    int current_ver = 0;
    {
        StmtGuard q(db_, "SELECT COALESCE(MAX(version), 0) FROM schema_migrations;");
        if (q.valid() && sqlite3_step(q.get()) == SQLITE_ROW) {
            current_ver = sqlite3_column_int(q.get(), 0);
        }
    }

    if (current_ver < kCurrentSchemaVersion) {
        StmtGuard ins(db_,
                      "INSERT OR REPLACE INTO schema_migrations(version, applied_at_ms, description) "
                      "VALUES (?, ?, ?);");
        if (ins.valid()) {
            ins.bind_int(1, kCurrentSchemaVersion);
            ins.bind_i64(2, core::now_ms());
            ins.bind_text(3, "Initial unified WAL + FTS5 schema v1");
            sqlite3_step(ins.get());
        }
    }

    return true;
}

void SqliteStore::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_) {
        sqlite3_wal_checkpoint_v2(db_, nullptr, SQLITE_CHECKPOINT_TRUNCATE, nullptr, nullptr);
        sqlite3_close_v2(db_);
        db_ = nullptr;
    }
}

bool SqliteStore::is_open() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return db_ != nullptr;
}

bool SqliteStore::check_integrity(std::string& details_out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) {
        details_out = "Database not opened";
        return false;
    }
    StmtGuard q(db_, "PRAGMA integrity_check;");
    if (!q.valid()) {
        details_out = sqlite3_errmsg(db_);
        return false;
    }
    if (sqlite3_step(q.get()) == SQLITE_ROW) {
        details_out = col_str(q.get(), 0);
        return details_out == "ok";
    }
    details_out = "integrity_check returned no rows";
    return false;
}

void SqliteStore::flush_wal() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_) {
        sqlite3_wal_checkpoint_v2(db_, nullptr, SQLITE_CHECKPOINT_PASSIVE, nullptr, nullptr);
    }
}

void SqliteStore::reindex_fts_entity(const std::string& entity_type,
                                     const std::string& entity_id,
                                     const std::string& session_id,
                                     const std::string& node_id,
                                     const std::string& repo_name,
                                     const std::string& title,
                                     const std::string& body) {
    StmtGuard del(db_, "DELETE FROM fts_context WHERE entity_type = ? AND entity_id = ?;");
    if (del.valid()) {
        del.bind_text(1, entity_type);
        del.bind_text(2, entity_id);
        sqlite3_step(del.get());
    }
    StmtGuard ins(db_,
                  "INSERT INTO fts_context(entity_type, entity_id, session_id, node_id, repo_name, title, body) "
                  "VALUES (?, ?, ?, ?, ?, ?, ?);");
    if (ins.valid()) {
        ins.bind_text(1, entity_type);
        ins.bind_text(2, entity_id);
        ins.bind_text(3, session_id);
        ins.bind_text(4, node_id);
        ins.bind_text(5, repo_name);
        ins.bind_text(6, title);
        ins.bind_text(7, body);
        sqlite3_step(ins.get());
    }
}

bool SqliteStore::upsert_node(const core::NodeInfo& node) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    if (!incoming_hlc_wins(db_,
                           "SELECT hlc_physical_ms, hlc_logical, origin_node_id FROM nodes WHERE node_id = ?;",
                           node.node_id,
                           node.hlc)) {
        return true;
    }
    StmtGuard st(db_,
                 "INSERT OR REPLACE INTO nodes("
                 "node_id, hostname, os_name, arch, tags_json, projects_json, pubkey_fp, endpoint, "
                 "online_status, rtt_ms, last_seen_ms, hlc_physical_ms, hlc_logical, origin_node_id"
                 ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);");
    if (!st.valid()) return false;
    st.bind_text(1, node.node_id);
    st.bind_text(2, node.hostname);
    st.bind_text(3, node.os_name);
    st.bind_text(4, node.arch);
    st.bind_text(5, dump_str_array(node.tags));
    st.bind_text(6, dump_str_array(node.projects));
    st.bind_text(7, node.pubkey_fingerprint);
    st.bind_text(8, node.endpoint);
    st.bind_text(9, core::to_string(node.online_status));
    st.bind_double(10, node.rtt_ms);
    st.bind_i64(11, node.last_seen_ms);
    st.bind_u64(12, node.hlc.physical_ms);
    st.bind_u64(13, node.hlc.logical);
    st.bind_text(14, node.hlc.origin_node_id);
    return sqlite3_step(st.get()) == SQLITE_DONE;
}

bool SqliteStore::upsert_workspace(const core::WorkspaceInfo& ws) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    if (!incoming_hlc_wins(db_,
                           "SELECT hlc_physical_ms, hlc_logical, origin_node_id FROM workspaces WHERE workspace_id = ?;",
                           ws.workspace_id,
                           ws.hlc)) {
        return true;
    }
    StmtGuard st(db_,
                 "INSERT OR REPLACE INTO workspaces("
                 "workspace_id, node_id, root_path, repo_name, git_branch, git_commit, is_open, "
                 "active_task, last_active_ms, hlc_physical_ms, hlc_logical, origin_node_id"
                 ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);");
    if (!st.valid()) return false;
    st.bind_text(1, ws.workspace_id);
    st.bind_text(2, ws.node_id);
    st.bind_text(3, ws.root_path);
    st.bind_text(4, ws.repo_name);
    st.bind_text(5, ws.git_branch);
    st.bind_text(6, ws.git_commit);
    st.bind_int(7, ws.is_open ? 1 : 0);
    st.bind_text(8, ws.active_task);
    st.bind_i64(9, ws.last_active_ms);
    st.bind_u64(10, ws.hlc.physical_ms);
    st.bind_u64(11, ws.hlc.logical);
    st.bind_text(12, ws.hlc.origin_node_id);
    return sqlite3_step(st.get()) == SQLITE_DONE;
}

bool SqliteStore::is_tombstoned(const std::string& target_type, const std::string& target_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    StmtGuard st(db_, "SELECT 1 FROM tombstones WHERE target_type = ? AND target_id = ?;");
    if (!st.valid()) return false;
    st.bind_text(1, target_type);
    st.bind_text(2, target_id);
    return sqlite3_step(st.get()) == SQLITE_ROW;
}

bool SqliteStore::upsert_session(const core::SessionRecord& session) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    {
        StmtGuard ts(db_, "SELECT 1 FROM tombstones WHERE target_type = 'session' AND target_id = ?;");
        if (ts.valid()) {
            ts.bind_text(1, session.session_id);
            if (sqlite3_step(ts.get()) == SQLITE_ROW) return true;
        }
    }
    if (!incoming_hlc_wins(db_,
                           "SELECT hlc_physical_ms, hlc_logical, origin_node_id FROM sessions WHERE session_id = ?;",
                           session.session_id,
                           session.hlc)) {
        return true;
    }
    StmtGuard st(db_,
                 "INSERT OR REPLACE INTO sessions("
                 "session_id, node_id, workspace_path, repo_name, git_branch, title, status, "
                 "is_private, source_adapter, tags_json, created_at_ms, updated_at_ms, "
                 "hlc_physical_ms, hlc_logical, origin_node_id"
                 ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);");
    if (!st.valid()) return false;
    st.bind_text(1, session.session_id);
    st.bind_text(2, session.node_id);
    st.bind_text(3, session.workspace_path);
    st.bind_text(4, session.repo_name);
    st.bind_text(5, session.git_branch);
    st.bind_text(6, session.title);
    st.bind_text(7, session.status);
    st.bind_int(8, session.is_private ? 1 : 0);
    st.bind_text(9, session.source_adapter);
    st.bind_text(10, dump_str_array(session.tags));
    st.bind_i64(11, session.created_at_ms);
    st.bind_i64(12, session.updated_at_ms);
    st.bind_u64(13, session.hlc.physical_ms);
    st.bind_u64(14, session.hlc.logical);
    st.bind_text(15, session.hlc.origin_node_id);
    const bool ok = sqlite3_step(st.get()) == SQLITE_DONE;
    if (ok && !session.is_private) {
        reindex_fts_entity("session",
                           session.session_id,
                           session.session_id,
                           session.node_id,
                           session.repo_name,
                           session.title,
                           session.workspace_path + " " + session.git_branch);
    }
    return ok;
}

bool SqliteStore::append_message(const core::MessageRecord& msg, bool& inserted_new) {
    std::lock_guard<std::mutex> lock(mutex_);
    inserted_new = false;
    if (!db_) return false;
    {
        StmtGuard ts(db_, "SELECT 1 FROM tombstones WHERE target_type = 'session' AND target_id = ?;");
        if (ts.valid()) {
            ts.bind_text(1, msg.session_id);
            if (sqlite3_step(ts.get()) == SQLITE_ROW) return true;
        }
    }
    {
        StmtGuard dup(db_, "SELECT 1 FROM messages WHERE content_sha256 = ? OR message_id = ?;");
        if (dup.valid()) {
            dup.bind_text(1, msg.content_sha256);
            dup.bind_text(2, msg.message_id);
            if (sqlite3_step(dup.get()) == SQLITE_ROW) {
                return true;
            }
        }
    }

    StmtGuard st(db_,
                 "INSERT OR IGNORE INTO messages("
                 "message_id, session_id, node_id, seq_index, role, content, touched_files_json, "
                 "content_sha256, created_at_ms, hlc_physical_ms, hlc_logical, origin_node_id"
                 ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);");
    if (!st.valid()) return false;
    st.bind_text(1, msg.message_id);
    st.bind_text(2, msg.session_id);
    st.bind_text(3, msg.node_id);
    st.bind_i64(4, msg.seq_index);
    st.bind_text(5, msg.role);
    st.bind_text(6, msg.content);
    st.bind_text(7, dump_str_array(msg.touched_files));
    st.bind_text(8, msg.content_sha256);
    st.bind_i64(9, msg.created_at_ms);
    st.bind_u64(10, msg.hlc.physical_ms);
    st.bind_u64(11, msg.hlc.logical);
    st.bind_text(12, msg.hlc.origin_node_id);
    if (sqlite3_step(st.get()) != SQLITE_DONE) return false;

    inserted_new = sqlite3_changes(db_) > 0;
    if (inserted_new) {
        std::string repo;
        std::string title;
        StmtGuard s(db_, "SELECT repo_name, title FROM sessions WHERE session_id = ?;");
        if (s.valid()) {
            s.bind_text(1, msg.session_id);
            if (sqlite3_step(s.get()) == SQLITE_ROW) {
                repo = col_str(s.get(), 0);
                title = col_str(s.get(), 1);
            }
        }
        reindex_fts_entity("message", msg.message_id, msg.session_id, msg.node_id, repo, title, msg.content);
    }
    return true;
}

bool SqliteStore::upsert_summary(const core::SessionSummary& summary) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    if (!incoming_hlc_wins(db_,
                           "SELECT hlc_physical_ms, hlc_logical, origin_node_id FROM session_summaries WHERE session_id = ?;",
                           summary.session_id,
                           summary.hlc)) {
        return true;
    }
    StmtGuard st(db_,
                 "INSERT OR REPLACE INTO session_summaries("
                 "session_id, node_id, current_goal, decisions_json, modified_files_json, "
                 "bugs_found_json, open_questions_json, summary_text, updated_at_ms, "
                 "hlc_physical_ms, hlc_logical, origin_node_id"
                 ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);");
    if (!st.valid()) return false;
    st.bind_text(1, summary.session_id);
    st.bind_text(2, summary.node_id);
    st.bind_text(3, summary.current_goal);
    st.bind_text(4, dump_str_array(summary.decisions));
    st.bind_text(5, dump_str_array(summary.modified_files));
    st.bind_text(6, dump_str_array(summary.bugs_found));
    st.bind_text(7, dump_str_array(summary.open_questions));
    st.bind_text(8, summary.summary_text);
    st.bind_i64(9, summary.updated_at_ms);
    st.bind_u64(10, summary.hlc.physical_ms);
    st.bind_u64(11, summary.hlc.logical);
    st.bind_text(12, summary.hlc.origin_node_id);
    const bool ok = sqlite3_step(st.get()) == SQLITE_DONE;
    if (ok) {
        std::string combined = summary.current_goal + "\n" + summary.summary_text;
        for (const auto& d : summary.decisions) combined += "\n" + d;
        for (const auto& b : summary.bugs_found) combined += "\n" + b;
        reindex_fts_entity("summary", summary.session_id, summary.session_id, summary.node_id, "", summary.current_goal, combined);
    }
    return ok;
}

bool SqliteStore::upsert_handoff(const core::HandoffPacket& handoff) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    if (!incoming_hlc_wins(db_,
                           "SELECT hlc_physical_ms, hlc_logical, origin_node_id FROM handoffs WHERE handoff_id = ?;",
                           handoff.handoff_id,
                           handoff.hlc)) {
        return true;
    }
    StmtGuard st(db_,
                 "INSERT OR REPLACE INTO handoffs("
                 "handoff_id, from_node_id, target_node_id, session_id, claimed_by_node, claimed_session_id, "
                 "repo_name, workspace_path, git_branch, git_commit, task_description, uncommitted_diff, "
                 "blockers_json, next_steps_json, artifact_refs_json, status, created_at_ms, updated_at_ms, "
                 "hlc_physical_ms, hlc_logical, origin_node_id"
                 ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);");
    if (!st.valid()) return false;
    st.bind_text(1, handoff.handoff_id);
    st.bind_text(2, handoff.from_node_id);
    st.bind_text(3, handoff.target_node_id);
    st.bind_text(4, handoff.session_id);
    st.bind_text(5, handoff.claimed_by_node);
    st.bind_text(6, handoff.claimed_session_id);
    st.bind_text(7, handoff.repo_name);
    st.bind_text(8, handoff.workspace_path);
    st.bind_text(9, handoff.git_branch);
    st.bind_text(10, handoff.git_commit);
    st.bind_text(11, handoff.task_description);
    st.bind_text(12, handoff.uncommitted_diff);
    st.bind_text(13, dump_str_array(handoff.blockers));
    st.bind_text(14, dump_str_array(handoff.next_steps));
    st.bind_text(15, dump_str_array(handoff.artifact_refs));
    st.bind_text(16, handoff.status);
    st.bind_i64(17, handoff.created_at_ms);
    st.bind_i64(18, handoff.updated_at_ms);
    st.bind_u64(19, handoff.hlc.physical_ms);
    st.bind_u64(20, handoff.hlc.logical);
    st.bind_text(21, handoff.hlc.origin_node_id);
    return sqlite3_step(st.get()) == SQLITE_DONE;
}

bool SqliteStore::upsert_artifact(const core::ArtifactRecord& artifact) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    if (!incoming_hlc_wins(db_,
                           "SELECT hlc_physical_ms, hlc_logical, origin_node_id FROM artifacts WHERE artifact_id = ?;",
                           artifact.artifact_id,
                           artifact.hlc)) {
        return true;
    }
    StmtGuard st(db_,
                 "INSERT OR REPLACE INTO artifacts("
                 "artifact_id, session_id, node_id, relative_path, mime_type, content_text, "
                 "size_bytes, content_sha256, updated_at_ms, hlc_physical_ms, hlc_logical, origin_node_id"
                 ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);");
    if (!st.valid()) return false;
    st.bind_text(1, artifact.artifact_id);
    st.bind_text(2, artifact.session_id);
    st.bind_text(3, artifact.node_id);
    st.bind_text(4, artifact.relative_path);
    st.bind_text(5, artifact.mime_type);
    st.bind_text(6, artifact.content_text);
    st.bind_i64(7, artifact.size_bytes);
    st.bind_text(8, artifact.content_sha256);
    st.bind_i64(9, artifact.updated_at_ms);
    st.bind_u64(10, artifact.hlc.physical_ms);
    st.bind_u64(11, artifact.hlc.logical);
    st.bind_text(12, artifact.hlc.origin_node_id);
    const bool ok = sqlite3_step(st.get()) == SQLITE_DONE;
    if (ok && !artifact.content_text.empty()) {
        reindex_fts_entity("artifact",
                           artifact.artifact_id,
                           artifact.session_id,
                           artifact.node_id,
                           "",
                           artifact.relative_path,
                           artifact.content_text);
    }
    return ok;
}

bool SqliteStore::upsert_lock(const core::DistributedLock& lock_obj) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    if (!incoming_hlc_wins(db_,
                           "SELECT hlc_physical_ms, hlc_logical, origin_node_id FROM locks WHERE lock_key = ?;",
                           lock_obj.lock_key,
                           lock_obj.hlc)) {
        return true;
    }
    StmtGuard st(db_,
                 "INSERT OR REPLACE INTO locks("
                 "lock_key, owner_node_id, owner_session_id, reason, acquired_at_ms, expires_at_ms, "
                 "released, hlc_physical_ms, hlc_logical, origin_node_id"
                 ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);");
    if (!st.valid()) return false;
    st.bind_text(1, lock_obj.lock_key);
    st.bind_text(2, lock_obj.owner_node_id);
    st.bind_text(3, lock_obj.owner_session_id);
    st.bind_text(4, lock_obj.reason);
    st.bind_i64(5, lock_obj.acquired_at_ms);
    st.bind_i64(6, lock_obj.expires_at_ms);
    st.bind_int(7, lock_obj.released ? 1 : 0);
    st.bind_u64(8, lock_obj.hlc.physical_ms);
    st.bind_u64(9, lock_obj.hlc.logical);
    st.bind_text(10, lock_obj.hlc.origin_node_id);
    return sqlite3_step(st.get()) == SQLITE_DONE;
}

bool SqliteStore::purge_entity(const core::TombstoneRecord& tombstone) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    {
        StmtGuard ts(db_,
                     "INSERT OR REPLACE INTO tombstones("
                     "target_type, target_id, purged_at_ms, hlc_physical_ms, hlc_logical, origin_node_id"
                     ") VALUES (?, ?, ?, ?, ?, ?);");
        if (!ts.valid()) return false;
        ts.bind_text(1, tombstone.target_type);
        ts.bind_text(2, tombstone.target_id);
        ts.bind_i64(3, tombstone.purged_at_ms);
        ts.bind_u64(4, tombstone.hlc.physical_ms);
        ts.bind_u64(5, tombstone.hlc.logical);
        ts.bind_text(6, tombstone.hlc.origin_node_id);
        if (sqlite3_step(ts.get()) != SQLITE_DONE) return false;
    }

    if (tombstone.target_type == "session") {
        const char* queries[] = {
            "DELETE FROM fts_context WHERE session_id = ?;",
            "DELETE FROM messages WHERE session_id = ?;",
            "DELETE FROM session_summaries WHERE session_id = ?;",
            "DELETE FROM artifacts WHERE session_id = ?;",
            "DELETE FROM sessions WHERE session_id = ?;"
        };
        for (const char* q : queries) {
            StmtGuard d(db_, q);
            if (d.valid()) {
                d.bind_text(1, tombstone.target_id);
                sqlite3_step(d.get());
            }
        }
    } else if (tombstone.target_type == "handoff") {
        StmtGuard d(db_, "DELETE FROM handoffs WHERE handoff_id = ?;");
        if (d.valid()) {
            d.bind_text(1, tombstone.target_id);
            sqlite3_step(d.get());
        }
    }
    return true;
}

bool SqliteStore::update_sync_cursor(const std::string& peer_node_id,
                                     const core::HlcTimestamp& hlc,
                                     const core::VectorClock& vc,
                                     std::int64_t sync_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    StmtGuard st(db_,
                 "INSERT OR REPLACE INTO sync_cursors("
                 "peer_node_id, last_hlc_physical, last_hlc_logical, vector_clock_json, last_sync_ms"
                 ") VALUES (?, ?, ?, ?, ?);");
    if (!st.valid()) return false;
    st.bind_text(1, peer_node_id);
    st.bind_u64(2, hlc.physical_ms);
    st.bind_u64(3, hlc.logical);
    st.bind_text(4, core::VectorClockOps::to_json(vc));
    st.bind_i64(5, sync_ms);
    return sqlite3_step(st.get()) == SQLITE_DONE;
}

core::VectorClock SqliteStore::get_peer_vector_clock(const std::string& peer_node_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return {};
    StmtGuard st(db_, "SELECT vector_clock_json FROM sync_cursors WHERE peer_node_id = ?;");
    if (!st.valid()) return {};
    st.bind_text(1, peer_node_id);
    if (sqlite3_step(st.get()) == SQLITE_ROW) {
        return core::VectorClockOps::from_json(col_str(st.get(), 0));
    }
    return {};
}

bool SqliteStore::enqueue_outbox(const core::OutboxEvent& ev) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    StmtGuard st(db_,
                 "INSERT OR IGNORE INTO outbox_queue("
                 "event_id, target_node_id, frame_type, payload_json, created_at_ms, attempts, next_retry_ms"
                 ") VALUES (?, ?, ?, ?, ?, ?, ?);");
    if (!st.valid()) return false;
    st.bind_text(1, ev.event_id);
    st.bind_text(2, ev.target_node_id);
    st.bind_int(3, static_cast<int>(ev.frame_type));
    st.bind_text(4, ev.payload_json);
    st.bind_i64(5, ev.created_at_ms);
    st.bind_int(6, ev.attempts);
    st.bind_i64(7, ev.next_retry_ms);
    return sqlite3_step(st.get()) == SQLITE_DONE;
}

std::vector<core::OutboxEvent> SqliteStore::fetch_due_outbox(const std::string& target_node_id,
                                                             std::int64_t now_ms_val,
                                                             int limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<core::OutboxEvent> out;
    if (!db_) return out;
    StmtGuard st(db_,
                 "SELECT event_id, target_node_id, frame_type, payload_json, created_at_ms, attempts, next_retry_ms "
                 "FROM outbox_queue WHERE (target_node_id = ? OR target_node_id = '*') AND next_retry_ms <= ? "
                 "ORDER BY created_at_ms ASC LIMIT ?;");
    if (!st.valid()) return out;
    st.bind_text(1, target_node_id);
    st.bind_i64(2, now_ms_val);
    st.bind_int(3, limit);
    while (sqlite3_step(st.get()) == SQLITE_ROW) {
        core::OutboxEvent ev;
        ev.event_id = col_str(st.get(), 0);
        ev.target_node_id = col_str(st.get(), 1);
        ev.frame_type = static_cast<std::uint8_t>(sqlite3_column_int(st.get(), 2));
        ev.payload_json = col_str(st.get(), 3);
        ev.created_at_ms = sqlite3_column_int64(st.get(), 4);
        ev.attempts = sqlite3_column_int(st.get(), 5);
        ev.next_retry_ms = sqlite3_column_int64(st.get(), 6);
        out.push_back(std::move(ev));
    }
    return out;
}

bool SqliteStore::mark_outbox_delivered(const std::string& event_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    StmtGuard st(db_, "DELETE FROM outbox_queue WHERE event_id = ?;");
    if (!st.valid()) return false;
    st.bind_text(1, event_id);
    return sqlite3_step(st.get()) == SQLITE_DONE;
}

bool SqliteStore::mark_outbox_retry(const std::string& event_id, std::int64_t next_retry_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    StmtGuard st(db_, "UPDATE outbox_queue SET attempts = attempts + 1, next_retry_ms = ? WHERE event_id = ?;");
    if (!st.valid()) return false;
    st.bind_i64(1, next_retry_ms);
    st.bind_text(2, event_id);
    return sqlite3_step(st.get()) == SQLITE_DONE;
}

bool SqliteStore::record_redaction(const std::string& session_id,
                                   const std::string& secret_type,
                                   const std::string& marker,
                                   const std::string& source_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    StmtGuard st(db_,
                 "INSERT INTO redaction_audit_log(session_id, secret_type, redacted_marker, source_path, detected_at_ms) "
                 "VALUES (?, ?, ?, ?, ?);");
    if (!st.valid()) return false;
    st.bind_text(1, session_id);
    st.bind_text(2, secret_type);
    st.bind_text(3, marker);
    st.bind_text(4, source_path);
    st.bind_i64(5, core::now_ms());
    return sqlite3_step(st.get()) == SQLITE_DONE;
}

std::vector<RedactionLogEntry> SqliteStore::list_redactions(int limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<RedactionLogEntry> out;
    if (!db_) return out;
    StmtGuard st(db_,
                 "SELECT id, session_id, secret_type, redacted_marker, source_path, detected_at_ms "
                 "FROM redaction_audit_log ORDER BY id DESC LIMIT ?;");
    if (!st.valid()) return out;
    st.bind_int(1, limit);
    while (sqlite3_step(st.get()) == SQLITE_ROW) {
        RedactionLogEntry entry;
        entry.id = sqlite3_column_int64(st.get(), 0);
        entry.session_id = col_str(st.get(), 1);
        entry.secret_type = col_str(st.get(), 2);
        entry.redacted_marker = col_str(st.get(), 3);
        entry.source_path = col_str(st.get(), 4);
        entry.detected_at_ms = sqlite3_column_int64(st.get(), 5);
        out.push_back(std::move(entry));
    }
    return out;
}

core::ClusterOverview SqliteStore::get_cluster_overview(const std::string& local_node_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    core::ClusterOverview ov;
    ov.local_node_id = local_node_id;
    ov.generated_at_ms = core::now_ms();
    if (!db_) return ov;

    {
        StmtGuard st(db_,
                     "SELECT node_id, hostname, os_name, arch, tags_json, projects_json, pubkey_fp, endpoint, "
                     "online_status, rtt_ms, last_seen_ms, hlc_physical_ms, hlc_logical, origin_node_id "
                     "FROM nodes ORDER BY node_id ASC;");
        if (st.valid()) {
            while (sqlite3_step(st.get()) == SQLITE_ROW) {
                core::NodeInfo n;
                n.node_id = col_str(st.get(), 0);
                n.hostname = col_str(st.get(), 1);
                n.os_name = col_str(st.get(), 2);
                n.arch = col_str(st.get(), 3);
                n.tags = parse_str_array(col_str(st.get(), 4));
                n.projects = parse_str_array(col_str(st.get(), 5));
                n.pubkey_fingerprint = col_str(st.get(), 6);
                n.endpoint = col_str(st.get(), 7);
                n.online_status = core::online_status_from_string(col_str(st.get(), 8));
                n.rtt_ms = sqlite3_column_double(st.get(), 9);
                n.last_seen_ms = sqlite3_column_int64(st.get(), 10);
                n.hlc.physical_ms = static_cast<std::uint64_t>(sqlite3_column_int64(st.get(), 11));
                n.hlc.logical = static_cast<std::uint32_t>(sqlite3_column_int64(st.get(), 12));
                n.hlc.origin_node_id = col_str(st.get(), 13);
                ov.nodes.push_back(std::move(n));
            }
        }
    }

    {
        StmtGuard st(db_,
                     "SELECT workspace_id, node_id, root_path, repo_name, git_branch, git_commit, is_open, "
                     "active_task, last_active_ms, hlc_physical_ms, hlc_logical, origin_node_id "
                     "FROM workspaces WHERE is_open = 1 ORDER BY last_active_ms DESC LIMIT 32;");
        if (st.valid()) {
            while (sqlite3_step(st.get()) == SQLITE_ROW) {
                core::WorkspaceInfo w;
                w.workspace_id = col_str(st.get(), 0);
                w.node_id = col_str(st.get(), 1);
                w.root_path = col_str(st.get(), 2);
                w.repo_name = col_str(st.get(), 3);
                w.git_branch = col_str(st.get(), 4);
                w.git_commit = col_str(st.get(), 5);
                w.is_open = sqlite3_column_int(st.get(), 6) != 0;
                w.active_task = col_str(st.get(), 7);
                w.last_active_ms = sqlite3_column_int64(st.get(), 8);
                w.hlc.physical_ms = static_cast<std::uint64_t>(sqlite3_column_int64(st.get(), 9));
                w.hlc.logical = static_cast<std::uint32_t>(sqlite3_column_int64(st.get(), 10));
                w.hlc.origin_node_id = col_str(st.get(), 11);
                ov.active_workspaces.push_back(std::move(w));
            }
        }
    }

    {
        StmtGuard st(db_,
                     "SELECT session_id, node_id, workspace_path, repo_name, git_branch, title, status, "
                     "is_private, source_adapter, tags_json, created_at_ms, updated_at_ms, "
                     "hlc_physical_ms, hlc_logical, origin_node_id "
                     "FROM sessions WHERE is_private = 0 ORDER BY updated_at_ms DESC LIMIT 20;");
        if (st.valid()) {
            while (sqlite3_step(st.get()) == SQLITE_ROW) {
                core::SessionRecord s;
                s.session_id = col_str(st.get(), 0);
                s.node_id = col_str(st.get(), 1);
                s.workspace_path = col_str(st.get(), 2);
                s.repo_name = col_str(st.get(), 3);
                s.git_branch = col_str(st.get(), 4);
                s.title = col_str(st.get(), 5);
                s.status = col_str(st.get(), 6);
                s.is_private = sqlite3_column_int(st.get(), 7) != 0;
                s.source_adapter = col_str(st.get(), 8);
                s.tags = parse_str_array(col_str(st.get(), 9));
                s.created_at_ms = sqlite3_column_int64(st.get(), 10);
                s.updated_at_ms = sqlite3_column_int64(st.get(), 11);
                s.hlc.physical_ms = static_cast<std::uint64_t>(sqlite3_column_int64(st.get(), 12));
                s.hlc.logical = static_cast<std::uint32_t>(sqlite3_column_int64(st.get(), 13));
                s.hlc.origin_node_id = col_str(st.get(), 14);
                ov.recent_sessions.push_back(std::move(s));
            }
        }
    }

    {
        StmtGuard st(db_,
                     "SELECT session_id, node_id, current_goal, decisions_json, modified_files_json, "
                     "bugs_found_json, open_questions_json, summary_text, updated_at_ms, "
                     "hlc_physical_ms, hlc_logical, origin_node_id "
                     "FROM session_summaries ORDER BY updated_at_ms DESC LIMIT 20;");
        if (st.valid()) {
            while (sqlite3_step(st.get()) == SQLITE_ROW) {
                core::SessionSummary sm;
                sm.session_id = col_str(st.get(), 0);
                sm.node_id = col_str(st.get(), 1);
                sm.current_goal = col_str(st.get(), 2);
                sm.decisions = parse_str_array(col_str(st.get(), 3));
                sm.modified_files = parse_str_array(col_str(st.get(), 4));
                sm.bugs_found = parse_str_array(col_str(st.get(), 5));
                sm.open_questions = parse_str_array(col_str(st.get(), 6));
                sm.summary_text = col_str(st.get(), 7);
                sm.updated_at_ms = sqlite3_column_int64(st.get(), 8);
                sm.hlc.physical_ms = static_cast<std::uint64_t>(sqlite3_column_int64(st.get(), 9));
                sm.hlc.logical = static_cast<std::uint32_t>(sqlite3_column_int64(st.get(), 10));
                sm.hlc.origin_node_id = col_str(st.get(), 11);
                ov.recent_summaries.push_back(std::move(sm));
            }
        }
    }

    {
        StmtGuard st(db_,
                     "SELECT handoff_id, from_node_id, target_node_id, session_id, claimed_by_node, claimed_session_id, "
                     "repo_name, workspace_path, git_branch, git_commit, task_description, uncommitted_diff, "
                     "blockers_json, next_steps_json, artifact_refs_json, status, created_at_ms, updated_at_ms, "
                     "hlc_physical_ms, hlc_logical, origin_node_id "
                     "FROM handoffs WHERE status = 'pending' ORDER BY updated_at_ms DESC LIMIT 20;");
        if (st.valid()) {
            while (sqlite3_step(st.get()) == SQLITE_ROW) {
                core::HandoffPacket h;
                h.handoff_id = col_str(st.get(), 0);
                h.from_node_id = col_str(st.get(), 1);
                h.target_node_id = col_str(st.get(), 2);
                h.session_id = col_str(st.get(), 3);
                h.claimed_by_node = col_str(st.get(), 4);
                h.claimed_session_id = col_str(st.get(), 5);
                h.repo_name = col_str(st.get(), 6);
                h.workspace_path = col_str(st.get(), 7);
                h.git_branch = col_str(st.get(), 8);
                h.git_commit = col_str(st.get(), 9);
                h.task_description = col_str(st.get(), 10);
                h.uncommitted_diff = col_str(st.get(), 11);
                h.blockers = parse_str_array(col_str(st.get(), 12));
                h.next_steps = parse_str_array(col_str(st.get(), 13));
                h.artifact_refs = parse_str_array(col_str(st.get(), 14));
                h.status = col_str(st.get(), 15);
                h.created_at_ms = sqlite3_column_int64(st.get(), 16);
                h.updated_at_ms = sqlite3_column_int64(st.get(), 17);
                h.hlc.physical_ms = static_cast<std::uint64_t>(sqlite3_column_int64(st.get(), 18));
                h.hlc.logical = static_cast<std::uint32_t>(sqlite3_column_int64(st.get(), 19));
                h.hlc.origin_node_id = col_str(st.get(), 20);
                ov.pending_handoffs.push_back(std::move(h));
            }
        }
    }

    {
        StmtGuard st(db_,
                     "SELECT lock_key, owner_node_id, owner_session_id, reason, acquired_at_ms, expires_at_ms, "
                     "released, hlc_physical_ms, hlc_logical, origin_node_id "
                     "FROM locks WHERE released = 0 AND expires_at_ms > ?;");
        if (st.valid()) {
            st.bind_i64(1, ov.generated_at_ms);
            while (sqlite3_step(st.get()) == SQLITE_ROW) {
                core::DistributedLock l;
                l.lock_key = col_str(st.get(), 0);
                l.owner_node_id = col_str(st.get(), 1);
                l.owner_session_id = col_str(st.get(), 2);
                l.reason = col_str(st.get(), 3);
                l.acquired_at_ms = sqlite3_column_int64(st.get(), 4);
                l.expires_at_ms = sqlite3_column_int64(st.get(), 5);
                l.released = sqlite3_column_int(st.get(), 6) != 0;
                l.hlc.physical_ms = static_cast<std::uint64_t>(sqlite3_column_int64(st.get(), 7));
                l.hlc.logical = static_cast<std::uint32_t>(sqlite3_column_int64(st.get(), 8));
                l.hlc.origin_node_id = col_str(st.get(), 9);
                ov.active_locks.push_back(std::move(l));
            }
        }
    }

    return ov;
}

std::vector<core::SessionRecord> SqliteStore::list_sessions(const SessionFilter& filter) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<core::SessionRecord> out;
    if (!db_) return out;

    std::string sql =
        "SELECT session_id, node_id, workspace_path, repo_name, git_branch, title, status, "
        "is_private, source_adapter, tags_json, created_at_ms, updated_at_ms, "
        "hlc_physical_ms, hlc_logical, origin_node_id FROM sessions WHERE 1=1 ";
    if (!filter.include_private) sql += "AND is_private = 0 ";
    if (!filter.node_id.empty()) sql += "AND node_id = ? ";
    if (!filter.repo_name.empty()) sql += "AND repo_name = ? ";
    if (!filter.status.empty()) sql += "AND status = ? ";
    if (filter.since_ms > 0) sql += "AND updated_at_ms >= ? ";
    if (!filter.tag.empty()) sql += "AND tags_json LIKE ? ";
    sql += "ORDER BY updated_at_ms DESC LIMIT ? OFFSET ?;";

    StmtGuard st(db_, sql.c_str());
    if (!st.valid()) return out;
    int idx = 1;
    if (!filter.node_id.empty()) st.bind_text(idx++, filter.node_id);
    if (!filter.repo_name.empty()) st.bind_text(idx++, filter.repo_name);
    if (!filter.status.empty()) st.bind_text(idx++, filter.status);
    if (filter.since_ms > 0) st.bind_i64(idx++, filter.since_ms);
    if (!filter.tag.empty()) st.bind_text(idx++, "%\"" + filter.tag + "\"%");
    st.bind_int(idx++, filter.limit);
    st.bind_int(idx++, filter.offset);

    while (sqlite3_step(st.get()) == SQLITE_ROW) {
        core::SessionRecord s;
        s.session_id = col_str(st.get(), 0);
        s.node_id = col_str(st.get(), 1);
        s.workspace_path = col_str(st.get(), 2);
        s.repo_name = col_str(st.get(), 3);
        s.git_branch = col_str(st.get(), 4);
        s.title = col_str(st.get(), 5);
        s.status = col_str(st.get(), 6);
        s.is_private = sqlite3_column_int(st.get(), 7) != 0;
        s.source_adapter = col_str(st.get(), 8);
        s.tags = parse_str_array(col_str(st.get(), 9));
        s.created_at_ms = sqlite3_column_int64(st.get(), 10);
        s.updated_at_ms = sqlite3_column_int64(st.get(), 11);
        s.hlc.physical_ms = static_cast<std::uint64_t>(sqlite3_column_int64(st.get(), 12));
        s.hlc.logical = static_cast<std::uint32_t>(sqlite3_column_int64(st.get(), 13));
        s.hlc.origin_node_id = col_str(st.get(), 14);
        out.push_back(std::move(s));
    }
    return out;
}

std::optional<core::SessionRecord> SqliteStore::get_session(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return std::nullopt;
    StmtGuard st(db_,
                 "SELECT session_id, node_id, workspace_path, repo_name, git_branch, title, status, "
                 "is_private, source_adapter, tags_json, created_at_ms, updated_at_ms, "
                 "hlc_physical_ms, hlc_logical, origin_node_id FROM sessions WHERE session_id = ?;");
    if (!st.valid()) return std::nullopt;
    st.bind_text(1, session_id);
    if (sqlite3_step(st.get()) != SQLITE_ROW) return std::nullopt;
    core::SessionRecord s;
    s.session_id = col_str(st.get(), 0);
    s.node_id = col_str(st.get(), 1);
    s.workspace_path = col_str(st.get(), 2);
    s.repo_name = col_str(st.get(), 3);
    s.git_branch = col_str(st.get(), 4);
    s.title = col_str(st.get(), 5);
    s.status = col_str(st.get(), 6);
    s.is_private = sqlite3_column_int(st.get(), 7) != 0;
    s.source_adapter = col_str(st.get(), 8);
    s.tags = parse_str_array(col_str(st.get(), 9));
    s.created_at_ms = sqlite3_column_int64(st.get(), 10);
    s.updated_at_ms = sqlite3_column_int64(st.get(), 11);
    s.hlc.physical_ms = static_cast<std::uint64_t>(sqlite3_column_int64(st.get(), 12));
    s.hlc.logical = static_cast<std::uint32_t>(sqlite3_column_int64(st.get(), 13));
    s.hlc.origin_node_id = col_str(st.get(), 14);
    return s;
}

std::optional<core::SessionSummary> SqliteStore::get_summary(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return std::nullopt;
    StmtGuard st(db_,
                 "SELECT session_id, node_id, current_goal, decisions_json, modified_files_json, "
                 "bugs_found_json, open_questions_json, summary_text, updated_at_ms, "
                 "hlc_physical_ms, hlc_logical, origin_node_id FROM session_summaries WHERE session_id = ?;");
    if (!st.valid()) return std::nullopt;
    st.bind_text(1, session_id);
    if (sqlite3_step(st.get()) != SQLITE_ROW) return std::nullopt;
    core::SessionSummary sm;
    sm.session_id = col_str(st.get(), 0);
    sm.node_id = col_str(st.get(), 1);
    sm.current_goal = col_str(st.get(), 2);
    sm.decisions = parse_str_array(col_str(st.get(), 3));
    sm.modified_files = parse_str_array(col_str(st.get(), 4));
    sm.bugs_found = parse_str_array(col_str(st.get(), 5));
    sm.open_questions = parse_str_array(col_str(st.get(), 6));
    sm.summary_text = col_str(st.get(), 7);
    sm.updated_at_ms = sqlite3_column_int64(st.get(), 8);
    sm.hlc.physical_ms = static_cast<std::uint64_t>(sqlite3_column_int64(st.get(), 9));
    sm.hlc.logical = static_cast<std::uint32_t>(sqlite3_column_int64(st.get(), 10));
    sm.hlc.origin_node_id = col_str(st.get(), 11);
    return sm;
}

std::vector<core::MessageRecord> SqliteStore::read_messages(const std::string& session_id,
                                                            int offset,
                                                            int limit,
                                                            std::size_t max_chars) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<core::MessageRecord> out;
    if (!db_) return out;
    StmtGuard st(db_,
                 "SELECT message_id, session_id, node_id, seq_index, role, content, touched_files_json, "
                 "content_sha256, created_at_ms, hlc_physical_ms, hlc_logical, origin_node_id "
                 "FROM messages WHERE session_id = ? ORDER BY seq_index ASC LIMIT ? OFFSET ?;");
    if (!st.valid()) return out;
    st.bind_text(1, session_id);
    st.bind_int(2, limit);
    st.bind_int(3, offset);
    std::size_t accumulated = 0;
    while (sqlite3_step(st.get()) == SQLITE_ROW) {
        core::MessageRecord m;
        m.message_id = col_str(st.get(), 0);
        m.session_id = col_str(st.get(), 1);
        m.node_id = col_str(st.get(), 2);
        m.seq_index = sqlite3_column_int64(st.get(), 3);
        m.role = col_str(st.get(), 4);
        m.content = col_str(st.get(), 5);
        m.touched_files = parse_str_array(col_str(st.get(), 6));
        m.content_sha256 = col_str(st.get(), 7);
        m.created_at_ms = sqlite3_column_int64(st.get(), 8);
        m.hlc.physical_ms = static_cast<std::uint64_t>(sqlite3_column_int64(st.get(), 9));
        m.hlc.logical = static_cast<std::uint32_t>(sqlite3_column_int64(st.get(), 10));
        m.hlc.origin_node_id = col_str(st.get(), 11);

        if (max_chars > 0 && accumulated + m.content.size() > max_chars && !out.empty()) {
            break;
        }
        if (max_chars > 0 && m.content.size() > max_chars) {
            m.content = m.content.substr(0, max_chars) + "...[truncated]";
        }
        accumulated += m.content.size();
        out.push_back(std::move(m));
    }
    return out;
}

std::vector<core::ArtifactRecord> SqliteStore::list_artifacts(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<core::ArtifactRecord> out;
    if (!db_) return out;
    StmtGuard st(db_,
                 "SELECT artifact_id, session_id, node_id, relative_path, mime_type, content_text, "
                 "size_bytes, content_sha256, updated_at_ms, hlc_physical_ms, hlc_logical, origin_node_id "
                 "FROM artifacts WHERE session_id = ? ORDER BY updated_at_ms DESC;");
    if (!st.valid()) return out;
    st.bind_text(1, session_id);
    while (sqlite3_step(st.get()) == SQLITE_ROW) {
        core::ArtifactRecord a;
        a.artifact_id = col_str(st.get(), 0);
        a.session_id = col_str(st.get(), 1);
        a.node_id = col_str(st.get(), 2);
        a.relative_path = col_str(st.get(), 3);
        a.mime_type = col_str(st.get(), 4);
        a.content_text = col_str(st.get(), 5);
        a.size_bytes = sqlite3_column_int64(st.get(), 6);
        a.content_sha256 = col_str(st.get(), 7);
        a.updated_at_ms = sqlite3_column_int64(st.get(), 8);
        a.hlc.physical_ms = static_cast<std::uint64_t>(sqlite3_column_int64(st.get(), 9));
        a.hlc.logical = static_cast<std::uint32_t>(sqlite3_column_int64(st.get(), 10));
        a.hlc.origin_node_id = col_str(st.get(), 11);
        out.push_back(std::move(a));
    }
    return out;
}

std::optional<core::HandoffPacket> SqliteStore::get_handoff(const std::string& handoff_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return std::nullopt;
    StmtGuard st(db_,
                 "SELECT handoff_id, from_node_id, target_node_id, session_id, claimed_by_node, claimed_session_id, "
                 "repo_name, workspace_path, git_branch, git_commit, task_description, uncommitted_diff, "
                 "blockers_json, next_steps_json, artifact_refs_json, status, created_at_ms, updated_at_ms, "
                 "hlc_physical_ms, hlc_logical, origin_node_id FROM handoffs WHERE handoff_id = ?;");
    if (!st.valid()) return std::nullopt;
    st.bind_text(1, handoff_id);
    if (sqlite3_step(st.get()) != SQLITE_ROW) return std::nullopt;
    core::HandoffPacket h;
    h.handoff_id = col_str(st.get(), 0);
    h.from_node_id = col_str(st.get(), 1);
    h.target_node_id = col_str(st.get(), 2);
    h.session_id = col_str(st.get(), 3);
    h.claimed_by_node = col_str(st.get(), 4);
    h.claimed_session_id = col_str(st.get(), 5);
    h.repo_name = col_str(st.get(), 6);
    h.workspace_path = col_str(st.get(), 7);
    h.git_branch = col_str(st.get(), 8);
    h.git_commit = col_str(st.get(), 9);
    h.task_description = col_str(st.get(), 10);
    h.uncommitted_diff = col_str(st.get(), 11);
    h.blockers = parse_str_array(col_str(st.get(), 12));
    h.next_steps = parse_str_array(col_str(st.get(), 13));
    h.artifact_refs = parse_str_array(col_str(st.get(), 14));
    h.status = col_str(st.get(), 15);
    h.created_at_ms = sqlite3_column_int64(st.get(), 16);
    h.updated_at_ms = sqlite3_column_int64(st.get(), 17);
    h.hlc.physical_ms = static_cast<std::uint64_t>(sqlite3_column_int64(st.get(), 18));
    h.hlc.logical = static_cast<std::uint32_t>(sqlite3_column_int64(st.get(), 19));
    h.hlc.origin_node_id = col_str(st.get(), 20);
    return h;
}

std::vector<core::HandoffPacket> SqliteStore::list_handoffs(const std::string& status_filter) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<core::HandoffPacket> out;
    if (!db_) return out;
    std::string sql =
        "SELECT handoff_id, from_node_id, target_node_id, session_id, claimed_by_node, claimed_session_id, "
        "repo_name, workspace_path, git_branch, git_commit, task_description, uncommitted_diff, "
        "blockers_json, next_steps_json, artifact_refs_json, status, created_at_ms, updated_at_ms, "
        "hlc_physical_ms, hlc_logical, origin_node_id FROM handoffs ";
    if (!status_filter.empty()) sql += "WHERE status = ? ";
    sql += "ORDER BY updated_at_ms DESC LIMIT 50;";

    StmtGuard st(db_, sql.c_str());
    if (!st.valid()) return out;
    if (!status_filter.empty()) st.bind_text(1, status_filter);
    while (sqlite3_step(st.get()) == SQLITE_ROW) {
        core::HandoffPacket h;
        h.handoff_id = col_str(st.get(), 0);
        h.from_node_id = col_str(st.get(), 1);
        h.target_node_id = col_str(st.get(), 2);
        h.session_id = col_str(st.get(), 3);
        h.claimed_by_node = col_str(st.get(), 4);
        h.claimed_session_id = col_str(st.get(), 5);
        h.repo_name = col_str(st.get(), 6);
        h.workspace_path = col_str(st.get(), 7);
        h.git_branch = col_str(st.get(), 8);
        h.git_commit = col_str(st.get(), 9);
        h.task_description = col_str(st.get(), 10);
        h.uncommitted_diff = col_str(st.get(), 11);
        h.blockers = parse_str_array(col_str(st.get(), 12));
        h.next_steps = parse_str_array(col_str(st.get(), 13));
        h.artifact_refs = parse_str_array(col_str(st.get(), 14));
        h.status = col_str(st.get(), 15);
        h.created_at_ms = sqlite3_column_int64(st.get(), 16);
        h.updated_at_ms = sqlite3_column_int64(st.get(), 17);
        h.hlc.physical_ms = static_cast<std::uint64_t>(sqlite3_column_int64(st.get(), 18));
        h.hlc.logical = static_cast<std::uint32_t>(sqlite3_column_int64(st.get(), 19));
        h.hlc.origin_node_id = col_str(st.get(), 20);
        out.push_back(std::move(h));
    }
    return out;
}

std::vector<core::DistributedLock> SqliteStore::list_active_locks(std::int64_t now_ms_val) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<core::DistributedLock> out;
    if (!db_) return out;
    StmtGuard st(db_,
                 "SELECT lock_key, owner_node_id, owner_session_id, reason, acquired_at_ms, expires_at_ms, "
                 "released, hlc_physical_ms, hlc_logical, origin_node_id "
                 "FROM locks WHERE released = 0 AND expires_at_ms > ?;");
    if (!st.valid()) return out;
    st.bind_i64(1, now_ms_val);
    while (sqlite3_step(st.get()) == SQLITE_ROW) {
        core::DistributedLock l;
        l.lock_key = col_str(st.get(), 0);
        l.owner_node_id = col_str(st.get(), 1);
        l.owner_session_id = col_str(st.get(), 2);
        l.reason = col_str(st.get(), 3);
        l.acquired_at_ms = sqlite3_column_int64(st.get(), 4);
        l.expires_at_ms = sqlite3_column_int64(st.get(), 5);
        l.released = sqlite3_column_int(st.get(), 6) != 0;
        l.hlc.physical_ms = static_cast<std::uint64_t>(sqlite3_column_int64(st.get(), 7));
        l.hlc.logical = static_cast<std::uint32_t>(sqlite3_column_int64(st.get(), 8));
        l.hlc.origin_node_id = col_str(st.get(), 9);
        out.push_back(std::move(l));
    }
    return out;
}

std::vector<core::SearchHit> SqliteStore::search_fts(const std::string& query,
                                                     const std::string& node_filter,
                                                     const std::string& repo_filter,
                                                     int limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<core::SearchHit> hits;
    if (!db_) return hits;

    const std::string fts_q = sanitize_fts5_query(query);
    if (fts_q.empty()) return hits;

    std::string sql =
        "SELECT entity_type, entity_id, session_id, node_id, repo_name, title, "
        "snippet(fts_context, 6, '[', ']', '...', 18), bm25(fts_context) "
        "FROM fts_context WHERE fts_context MATCH ? ";
    if (!node_filter.empty()) sql += "AND node_id = ? ";
    if (!repo_filter.empty()) sql += "AND repo_name = ? ";
    sql += "ORDER BY bm25(fts_context) ASC LIMIT ?;";

    StmtGuard st(db_, sql.c_str());
    if (!st.valid()) return hits;
    int idx = 1;
    st.bind_text(idx++, fts_q);
    if (!node_filter.empty()) st.bind_text(idx++, node_filter);
    if (!repo_filter.empty()) st.bind_text(idx++, repo_filter);
    st.bind_int(idx++, limit);

    while (sqlite3_step(st.get()) == SQLITE_ROW) {
        core::SearchHit h;
        h.entity_type = col_str(st.get(), 0);
        h.entity_id = col_str(st.get(), 1);
        h.session_id = col_str(st.get(), 2);
        h.node_id = col_str(st.get(), 3);
        h.repo_name = col_str(st.get(), 4);
        h.title = col_str(st.get(), 5);
        h.snippet = col_str(st.get(), 6);
        h.score = -sqlite3_column_double(st.get(), 7);
        hits.push_back(std::move(h));
    }
    return hits;
}

std::vector<core::SessionRecord> SqliteStore::get_sessions_since(std::uint64_t min_hlc_ms, int limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<core::SessionRecord> out;
    if (!db_) return out;
    StmtGuard st(db_,
                 "SELECT session_id, node_id, workspace_path, repo_name, git_branch, title, status, "
                 "is_private, source_adapter, tags_json, created_at_ms, updated_at_ms, "
                 "hlc_physical_ms, hlc_logical, origin_node_id FROM sessions "
                 "WHERE is_private = 0 AND hlc_physical_ms >= ? ORDER BY hlc_physical_ms ASC LIMIT ?;");
    if (!st.valid()) return out;
    st.bind_u64(1, min_hlc_ms);
    st.bind_int(2, limit);
    while (sqlite3_step(st.get()) == SQLITE_ROW) {
        core::SessionRecord s;
        s.session_id = col_str(st.get(), 0);
        s.node_id = col_str(st.get(), 1);
        s.workspace_path = col_str(st.get(), 2);
        s.repo_name = col_str(st.get(), 3);
        s.git_branch = col_str(st.get(), 4);
        s.title = col_str(st.get(), 5);
        s.status = col_str(st.get(), 6);
        s.is_private = sqlite3_column_int(st.get(), 7) != 0;
        s.source_adapter = col_str(st.get(), 8);
        s.tags = parse_str_array(col_str(st.get(), 9));
        s.created_at_ms = sqlite3_column_int64(st.get(), 10);
        s.updated_at_ms = sqlite3_column_int64(st.get(), 11);
        s.hlc.physical_ms = static_cast<std::uint64_t>(sqlite3_column_int64(st.get(), 12));
        s.hlc.logical = static_cast<std::uint32_t>(sqlite3_column_int64(st.get(), 13));
        s.hlc.origin_node_id = col_str(st.get(), 14);
        out.push_back(std::move(s));
    }
    return out;
}

std::vector<core::MessageRecord> SqliteStore::get_messages_since(std::uint64_t min_hlc_ms, int limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<core::MessageRecord> out;
    if (!db_) return out;
    StmtGuard st(db_,
                 "SELECT m.message_id, m.session_id, m.node_id, m.seq_index, m.role, m.content, "
                 "m.touched_files_json, m.content_sha256, m.created_at_ms, m.hlc_physical_ms, "
                 "m.hlc_logical, m.origin_node_id FROM messages m "
                 "JOIN sessions s ON s.session_id = m.session_id "
                 "WHERE s.is_private = 0 AND m.hlc_physical_ms >= ? ORDER BY m.hlc_physical_ms ASC LIMIT ?;");
    if (!st.valid()) return out;
    st.bind_u64(1, min_hlc_ms);
    st.bind_int(2, limit);
    while (sqlite3_step(st.get()) == SQLITE_ROW) {
        core::MessageRecord m;
        m.message_id = col_str(st.get(), 0);
        m.session_id = col_str(st.get(), 1);
        m.node_id = col_str(st.get(), 2);
        m.seq_index = sqlite3_column_int64(st.get(), 3);
        m.role = col_str(st.get(), 4);
        m.content = col_str(st.get(), 5);
        m.touched_files = parse_str_array(col_str(st.get(), 6));
        m.content_sha256 = col_str(st.get(), 7);
        m.created_at_ms = sqlite3_column_int64(st.get(), 8);
        m.hlc.physical_ms = static_cast<std::uint64_t>(sqlite3_column_int64(st.get(), 9));
        m.hlc.logical = static_cast<std::uint32_t>(sqlite3_column_int64(st.get(), 10));
        m.hlc.origin_node_id = col_str(st.get(), 11);
        out.push_back(std::move(m));
    }
    return out;
}

std::vector<core::SessionSummary> SqliteStore::get_summaries_since(std::uint64_t min_hlc_ms, int limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<core::SessionSummary> out;
    if (!db_) return out;
    StmtGuard st(db_,
                 "SELECT session_id, node_id, current_goal, decisions_json, modified_files_json, "
                 "bugs_found_json, open_questions_json, summary_text, updated_at_ms, "
                 "hlc_physical_ms, hlc_logical, origin_node_id FROM session_summaries "
                 "WHERE hlc_physical_ms >= ? ORDER BY hlc_physical_ms ASC LIMIT ?;");
    if (!st.valid()) return out;
    st.bind_u64(1, min_hlc_ms);
    st.bind_int(2, limit);
    while (sqlite3_step(st.get()) == SQLITE_ROW) {
        core::SessionSummary sm;
        sm.session_id = col_str(st.get(), 0);
        sm.node_id = col_str(st.get(), 1);
        sm.current_goal = col_str(st.get(), 2);
        sm.decisions = parse_str_array(col_str(st.get(), 3));
        sm.modified_files = parse_str_array(col_str(st.get(), 4));
        sm.bugs_found = parse_str_array(col_str(st.get(), 5));
        sm.open_questions = parse_str_array(col_str(st.get(), 6));
        sm.summary_text = col_str(st.get(), 7);
        sm.updated_at_ms = sqlite3_column_int64(st.get(), 8);
        sm.hlc.physical_ms = static_cast<std::uint64_t>(sqlite3_column_int64(st.get(), 9));
        sm.hlc.logical = static_cast<std::uint32_t>(sqlite3_column_int64(st.get(), 10));
        sm.hlc.origin_node_id = col_str(st.get(), 11);
        out.push_back(std::move(sm));
    }
    return out;
}

std::vector<core::HandoffPacket> SqliteStore::get_handoffs_since(std::uint64_t min_hlc_ms, int limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<core::HandoffPacket> out;
    if (!db_) return out;
    StmtGuard st(db_,
                 "SELECT handoff_id, from_node_id, target_node_id, session_id, claimed_by_node, claimed_session_id, "
                 "repo_name, workspace_path, git_branch, git_commit, task_description, uncommitted_diff, "
                 "blockers_json, next_steps_json, artifact_refs_json, status, created_at_ms, updated_at_ms, "
                 "hlc_physical_ms, hlc_logical, origin_node_id FROM handoffs "
                 "WHERE hlc_physical_ms >= ? ORDER BY hlc_physical_ms ASC LIMIT ?;");
    if (!st.valid()) return out;
    st.bind_u64(1, min_hlc_ms);
    st.bind_int(2, limit);
    while (sqlite3_step(st.get()) == SQLITE_ROW) {
        core::HandoffPacket h;
        h.handoff_id = col_str(st.get(), 0);
        h.from_node_id = col_str(st.get(), 1);
        h.target_node_id = col_str(st.get(), 2);
        h.session_id = col_str(st.get(), 3);
        h.claimed_by_node = col_str(st.get(), 4);
        h.claimed_session_id = col_str(st.get(), 5);
        h.repo_name = col_str(st.get(), 6);
        h.workspace_path = col_str(st.get(), 7);
        h.git_branch = col_str(st.get(), 8);
        h.git_commit = col_str(st.get(), 9);
        h.task_description = col_str(st.get(), 10);
        h.uncommitted_diff = col_str(st.get(), 11);
        h.blockers = parse_str_array(col_str(st.get(), 12));
        h.next_steps = parse_str_array(col_str(st.get(), 13));
        h.artifact_refs = parse_str_array(col_str(st.get(), 14));
        h.status = col_str(st.get(), 15);
        h.created_at_ms = sqlite3_column_int64(st.get(), 16);
        h.updated_at_ms = sqlite3_column_int64(st.get(), 17);
        h.hlc.physical_ms = static_cast<std::uint64_t>(sqlite3_column_int64(st.get(), 18));
        h.hlc.logical = static_cast<std::uint32_t>(sqlite3_column_int64(st.get(), 19));
        h.hlc.origin_node_id = col_str(st.get(), 20);
        out.push_back(std::move(h));
    }
    return out;
}

std::vector<core::DistributedLock> SqliteStore::get_locks_since(std::uint64_t min_hlc_ms, int limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<core::DistributedLock> out;
    if (!db_) return out;
    StmtGuard st(db_,
                 "SELECT lock_key, owner_node_id, owner_session_id, reason, acquired_at_ms, expires_at_ms, "
                 "released, hlc_physical_ms, hlc_logical, origin_node_id FROM locks "
                 "WHERE hlc_physical_ms >= ? ORDER BY hlc_physical_ms ASC LIMIT ?;");
    if (!st.valid()) return out;
    st.bind_u64(1, min_hlc_ms);
    st.bind_int(2, limit);
    while (sqlite3_step(st.get()) == SQLITE_ROW) {
        core::DistributedLock l;
        l.lock_key = col_str(st.get(), 0);
        l.owner_node_id = col_str(st.get(), 1);
        l.owner_session_id = col_str(st.get(), 2);
        l.reason = col_str(st.get(), 3);
        l.acquired_at_ms = sqlite3_column_int64(st.get(), 4);
        l.expires_at_ms = sqlite3_column_int64(st.get(), 5);
        l.released = sqlite3_column_int(st.get(), 6) != 0;
        l.hlc.physical_ms = static_cast<std::uint64_t>(sqlite3_column_int64(st.get(), 7));
        l.hlc.logical = static_cast<std::uint32_t>(sqlite3_column_int64(st.get(), 8));
        l.hlc.origin_node_id = col_str(st.get(), 9);
        out.push_back(std::move(l));
    }
    return out;
}

std::vector<core::WorkspaceInfo> SqliteStore::get_workspaces_since(std::uint64_t min_hlc_ms, int limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<core::WorkspaceInfo> out;
    if (!db_) return out;
    StmtGuard st(db_,
                 "SELECT workspace_id, node_id, root_path, repo_name, git_branch, git_commit, is_open, "
                 "active_task, last_active_ms, hlc_physical_ms, hlc_logical, origin_node_id FROM workspaces "
                 "WHERE hlc_physical_ms >= ? ORDER BY hlc_physical_ms ASC LIMIT ?;");
    if (!st.valid()) return out;
    st.bind_u64(1, min_hlc_ms);
    st.bind_int(2, limit);
    while (sqlite3_step(st.get()) == SQLITE_ROW) {
        core::WorkspaceInfo w;
        w.workspace_id = col_str(st.get(), 0);
        w.node_id = col_str(st.get(), 1);
        w.root_path = col_str(st.get(), 2);
        w.repo_name = col_str(st.get(), 3);
        w.git_branch = col_str(st.get(), 4);
        w.git_commit = col_str(st.get(), 5);
        w.is_open = sqlite3_column_int(st.get(), 6) != 0;
        w.active_task = col_str(st.get(), 7);
        w.last_active_ms = sqlite3_column_int64(st.get(), 8);
        w.hlc.physical_ms = static_cast<std::uint64_t>(sqlite3_column_int64(st.get(), 9));
        w.hlc.logical = static_cast<std::uint32_t>(sqlite3_column_int64(st.get(), 10));
        w.hlc.origin_node_id = col_str(st.get(), 11);
        out.push_back(std::move(w));
    }
    return out;
}

std::vector<core::TombstoneRecord> SqliteStore::get_tombstones_since(std::uint64_t min_hlc_ms, int limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<core::TombstoneRecord> out;
    if (!db_) return out;
    StmtGuard st(db_,
                 "SELECT target_type, target_id, purged_at_ms, hlc_physical_ms, hlc_logical, origin_node_id "
                 "FROM tombstones WHERE hlc_physical_ms >= ? ORDER BY hlc_physical_ms ASC LIMIT ?;");
    if (!st.valid()) return out;
    st.bind_u64(1, min_hlc_ms);
    st.bind_int(2, limit);
    while (sqlite3_step(st.get()) == SQLITE_ROW) {
        core::TombstoneRecord t;
        t.target_type = col_str(st.get(), 0);
        t.target_id = col_str(st.get(), 1);
        t.purged_at_ms = sqlite3_column_int64(st.get(), 2);
        t.hlc.physical_ms = static_cast<std::uint64_t>(sqlite3_column_int64(st.get(), 3));
        t.hlc.logical = static_cast<std::uint32_t>(sqlite3_column_int64(st.get(), 4));
        t.hlc.origin_node_id = col_str(st.get(), 5);
        out.push_back(std::move(t));
    }
    return out;
}

std::size_t SqliteStore::rotate_expired_messages(int ttl_days, std::int64_t now_ms_val) {
    if (ttl_days <= 0) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return 0;
    const std::int64_t cutoff_ms = now_ms_val - static_cast<std::int64_t>(ttl_days) * 86400LL * 1000LL;
    {
        StmtGuard del_fts(db_,
                          "DELETE FROM fts_context WHERE entity_type = 'message' AND entity_id IN "
                          "(SELECT message_id FROM messages WHERE created_at_ms < ?);");
        if (del_fts.valid()) {
            del_fts.bind_i64(1, cutoff_ms);
            sqlite3_step(del_fts.get());
        }
    }
    StmtGuard del_msgs(db_, "DELETE FROM messages WHERE created_at_ms < ?;");
    if (!del_msgs.valid()) return 0;
    del_msgs.bind_i64(1, cutoff_ms);
    if (sqlite3_step(del_msgs.get()) != SQLITE_DONE) return 0;
    return static_cast<std::size_t>(sqlite3_changes(db_));
}

bool SqliteStore::export_snapshot_json(const std::filesystem::path& dest_path, const std::string& local_node_id) {
    const auto ov = get_cluster_overview(local_node_id);
    nlohmann::json root;
    root["local_node_id"] = ov.local_node_id;
    root["generated_at_ms"] = ov.generated_at_ms;

    nlohmann::json nodes_arr = nlohmann::json::array();
    for (const auto& n : ov.nodes) {
        nodes_arr.push_back({
            {"node_id", n.node_id},
            {"hostname", n.hostname},
            {"os", n.os_name},
            {"status", core::to_string(n.online_status)},
            {"rtt_ms", n.rtt_ms},
            {"last_seen_ms", n.last_seen_ms},
            {"tags", n.tags}
        });
    }
    root["nodes"] = std::move(nodes_arr);

    nlohmann::json sess_arr = nlohmann::json::array();
    for (const auto& s : ov.recent_sessions) {
        sess_arr.push_back({
            {"session_id", s.session_id},
            {"node_id", s.node_id},
            {"repo_name", s.repo_name},
            {"git_branch", s.git_branch},
            {"title", s.title},
            {"status", s.status},
            {"updated_at_ms", s.updated_at_ms}
        });
    }
    root["recent_sessions"] = std::move(sess_arr);

    nlohmann::json sum_arr = nlohmann::json::array();
    for (const auto& sm : ov.recent_summaries) {
        sum_arr.push_back({
            {"session_id", sm.session_id},
            {"node_id", sm.node_id},
            {"current_goal", sm.current_goal},
            {"decisions", sm.decisions},
            {"modified_files", sm.modified_files},
            {"summary_text", sm.summary_text}
        });
    }
    root["session_summaries"] = std::move(sum_arr);

    const auto tmp = dest_path.string() + ".tmp";
    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs) return false;
        ofs << root.dump(2) << "\n";
        ofs.flush();
        if (!ofs) return false;
    }
    std::filesystem::rename(tmp, dest_path);
    return true;
}

bool SqliteStore::export_snapshot_markdown(const std::filesystem::path& dest_path, const std::string& local_node_id) {
    const auto ov = get_cluster_overview(local_node_id);
    std::ostringstream oss;
    oss << "# ag-mesh Cluster Context Snapshot\n\n";
    oss << "- **Local Node:** `" << ov.local_node_id << "`\n";
    oss << "- **Generated At (ms):** `" << ov.generated_at_ms << "`\n\n";
    oss << "## Nodes\n\n| Node ID | Hostname | OS | Status | RTT (ms) |\n|---|---|---|---|---|\n";
    for (const auto& n : ov.nodes) {
        oss << "| `" << n.node_id << "` | " << n.hostname << " | " << n.os_name << " | "
            << core::to_string(n.online_status) << " | " << n.rtt_ms << " |\n";
    }
    oss << "\n## Recent Sessions\n\n";
    for (const auto& s : ov.recent_sessions) {
        oss << "- **[" << s.node_id << "] " << s.title << "** (`" << s.session_id << "`, repo `"
            << s.repo_name << "`, branch `" << s.git_branch << "`, status `" << s.status << "`)\n";
    }
    oss << "\n## Pending Handoffs\n\n";
    for (const auto& h : ov.pending_handoffs) {
        oss << "- `" << h.handoff_id << "` from `" << h.from_node_id << "` -> `"
            << (h.target_node_id.empty() ? "any" : h.target_node_id) << "`: " << h.task_description << "\n";
    }

    const auto tmp = dest_path.string() + ".tmp";
    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs) return false;
        ofs << oss.str();
        ofs.flush();
        if (!ofs) return false;
    }
    std::filesystem::rename(tmp, dest_path);
    return true;
}

StoreStats SqliteStore::get_stats() {
    std::lock_guard<std::mutex> lock(mutex_);
    StoreStats stats;
    if (!db_) return stats;
    auto count_table = [&](const char* sql) -> std::int64_t {
        StmtGuard q(db_, sql);
        if (q.valid() && sqlite3_step(q.get()) == SQLITE_ROW) {
            return sqlite3_column_int64(q.get(), 0);
        }
        return 0;
    };
    stats.nodes_count = count_table("SELECT COUNT(*) FROM nodes;");
    stats.workspaces_count = count_table("SELECT COUNT(*) FROM workspaces;");
    stats.sessions_count = count_table("SELECT COUNT(*) FROM sessions;");
    stats.messages_count = count_table("SELECT COUNT(*) FROM messages;");
    stats.artifacts_count = count_table("SELECT COUNT(*) FROM artifacts;");
    stats.handoffs_count = count_table("SELECT COUNT(*) FROM handoffs;");
    stats.outbox_pending_count = count_table("SELECT COUNT(*) FROM outbox_queue;");
    stats.redactions_count = count_table("SELECT COUNT(*) FROM redaction_audit_log;");
    stats.schema_version = static_cast<int>(count_table("SELECT COALESCE(MAX(version), 0) FROM schema_migrations;"));

    std::error_code ec;
    if (std::filesystem::exists(db_path_, ec)) {
        stats.db_file_size_bytes = static_cast<std::int64_t>(std::filesystem::file_size(db_path_, ec));
    }
    return stats;
}

} // namespace agmesh::storage
