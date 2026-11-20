#include "agmesh/adapter/antigravity_adapter.hpp"
#include "agmesh/core/hlc_crdt.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace agmesh::adapter {

namespace {

std::string env_var(const char* name) {
#ifdef _WIN32
    char* buf = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&buf, &len, name) == 0 && buf != nullptr) {
        std::string s(buf);
        std::free(buf);
        return s;
    }
    return {};
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string{};
#endif
}

std::string read_file_safe(const std::filesystem::path& p, std::size_t max_bytes = 2 * 1024 * 1024) {
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) return {};
    std::string out;
    ifs.seekg(0, std::ios::end);
    const auto sz = ifs.tellg();
    if (sz <= 0) return {};
    const auto to_read = static_cast<std::size_t>(std::min<std::streamoff>(sz, static_cast<std::streamoff>(max_bytes)));
    out.resize(to_read);
    ifs.seekg(0, std::ios::beg);
    ifs.read(out.data(), static_cast<std::streamsize>(to_read));
    out.resize(static_cast<std::size_t>(ifs.gcount()));
    return out;
}

std::int64_t file_mtime_ms(const std::filesystem::path& p) {
    std::error_code ec;
    const auto ftime = std::filesystem::last_write_time(p, ec);
    if (ec) return core::now_ms();
    const auto sys_tp = std::chrono::time_point_cast<std::chrono::milliseconds>(
        ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    return sys_tp.time_since_epoch().count();
}

std::string extract_first_heading(const std::string& md) {
    std::istringstream iss(md);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.size() > 2 && line[0] == '#') {
            std::size_t pos = 0;
            while (pos < line.size() && (line[pos] == '#' || line[pos] == ' ')) ++pos;
            return line.substr(pos);
        }
    }
    return {};
}

std::int64_t parse_iso8601_utc_ms(const std::string& ts) {
    if (ts.size() < 19) return core::now_ms();
    std::tm tm_val{};
    std::istringstream iss(ts.substr(0, 19));
    iss >> std::get_time(&tm_val, "%Y-%m-%d %H:%M:%S");
    if (iss.fail()) {
        std::istringstream iss2(ts.substr(0, 19));
        iss2 >> std::get_time(&tm_val, "%Y-%m-%dT%H:%M:%S");
        if (iss2.fail()) return core::now_ms();
    }
#ifdef _WIN32
    const std::time_t secs = _mkgmtime(&tm_val);
#else
    const std::time_t secs = timegm(&tm_val);
#endif
    if (secs <= 0) return core::now_ms();
    return static_cast<std::int64_t>(secs) * 1000LL;
}

std::string uri_to_local_path(const std::string& raw_uris) {
    if (raw_uris.empty()) return {};
    // workspace_uris may hold multiple URIs separated by newline, comma, or whitespace
    std::string first = raw_uris;
    const auto sep = first.find_first_of("\r\n,;");
    if (sep != std::string::npos) first = first.substr(0, sep);
    while (!first.empty() && (first.front() == ' ' || first.front() == '[' || first.front() == '"')) {
        first.erase(first.begin());
    }
    while (!first.empty() && (first.back() == ' ' || first.back() == ']' || first.back() == '"')) {
        first.pop_back();
    }
    const std::string file_prefix = "file:///";
    if (first.rfind(file_prefix, 0) == 0) {
        std::string rest = first.substr(file_prefix.size());
#ifndef _WIN32
        if (!rest.empty() && rest[0] != '/') rest = "/" + rest;
#endif
        return rest;
    }
    return first;
}

std::unordered_map<std::string, std::pair<std::string, std::int64_t>> load_annotations_map(
    const std::filesystem::path& app_data_dir) {
    std::unordered_map<std::string, std::pair<std::string, std::int64_t>> out;
    std::error_code ec;
    const auto dir = app_data_dir / "annotations";
    if (!std::filesystem::exists(dir, ec)) return out;

    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".pbtxt") continue;
        const std::string conv_id = entry.path().stem().string();
        const std::string txt = read_file_safe(entry.path(), 16384);
        std::string title;
        std::int64_t view_ms = 0;

        const auto tpos = txt.find("title:\"");
        if (tpos != std::string::npos) {
            const auto q1 = tpos + 7;
            const auto q2 = txt.find('"', q1);
            if (q2 != std::string::npos) title = txt.substr(q1, q2 - q1);
        }
        const auto spos = txt.find("seconds:");
        if (spos != std::string::npos) {
            try {
                view_ms = std::stoll(txt.substr(spos + 8)) * 1000LL;
            } catch (...) {
            }
        }
        out[conv_id] = {std::move(title), view_ms};
    }
    return out;
}

std::set<std::string> load_presence_active_set(const std::filesystem::path& app_data_dir) {
    std::set<std::string> active;
    std::error_code ec;
    const auto dir = app_data_dir / "presence";
    if (!std::filesystem::exists(dir, ec)) return active;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.path().extension() == ".lock") {
            active.insert(entry.path().stem().string());
        }
    }
    return active;
}

struct SnapshotDbHandle {
    sqlite3* db{nullptr};
    std::filesystem::path temp_dir;

    ~SnapshotDbHandle() {
        if (db) sqlite3_close(db);
        if (!temp_dir.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(temp_dir, ec);
        }
    }
};

bool verify_snapshot_healthy(sqlite3* db) {
    if (!db) return false;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "PRAGMA quick_check(1);", -1, &st, nullptr) != SQLITE_OK) {
        return false;
    }
    bool ok = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char* txt = sqlite3_column_text(st, 0);
        if (txt && std::string(reinterpret_cast<const char*>(txt)) == "ok") {
            ok = true;
        }
    }
    sqlite3_finalize(st);
    return ok;
}

bool open_wal_safe_readonly(const std::filesystem::path& db_file, SnapshotDbHandle& out) {
    // Retry once if an active IDE WAL checkpoint caused a torn file copy (SQLITE_CORRUPT).
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::error_code ec;
        const auto snap_dir = std::filesystem::temp_directory_path(ec) /
                              ("agmesh_ag_ro_" + std::to_string(core::now_us()));
        if (!ec && std::filesystem::create_directories(snap_dir, ec)) {
            const auto dst_db = snap_dir / db_file.filename();
            if (std::filesystem::copy_file(db_file, dst_db, std::filesystem::copy_options::overwrite_existing, ec)) {
                for (const char* suffix : {"-wal", "-shm"}) {
                    const auto side_src = std::filesystem::path(db_file.string() + suffix);
                    if (std::filesystem::exists(side_src, ec)) {
                        std::filesystem::copy_file(
                            side_src,
                            std::filesystem::path(dst_db.string() + suffix),
                            std::filesystem::copy_options::overwrite_existing,
                            ec);
                    }
                }
                if (sqlite3_open_v2(dst_db.string().c_str(), &out.db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK &&
                    verify_snapshot_healthy(out.db)) {
                    out.temp_dir = snap_dir;
                    return true;
                }
                if (out.db) {
                    sqlite3_close(out.db);
                    out.db = nullptr;
                }
            }
            std::filesystem::remove_all(snap_dir, ec);
        }
        if (attempt == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
    }

    // Fallback to immutable read-only URI (also validated via quick_check before accepting)
    const std::string uri = "file:" + db_file.generic_string() + "?mode=ro&immutable=1";
    if (sqlite3_open_v2(uri.c_str(), &out.db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr) == SQLITE_OK &&
        verify_snapshot_healthy(out.db)) {
        return true;
    }
    if (out.db) {
        sqlite3_close(out.db);
        out.db = nullptr;
    }
    return false;
}

bool table_exists(sqlite3* db, const char* name) {
    sqlite3_stmt* st = nullptr;
    bool found = false;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?;", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
        found = (sqlite3_step(st) == SQLITE_ROW);
        sqlite3_finalize(st);
    }
    return found;
}

std::string safe_col_text(sqlite3_stmt* st, int col) {
    const unsigned char* p = sqlite3_column_text(st, col);
    const int n = sqlite3_column_bytes(st, col);
    if (!p || n <= 0) return {};
    return std::string(reinterpret_cast<const char*>(p), static_cast<std::size_t>(n));
}

// 1. WAL-Safe Read-Only SQLite Parser (supports live Antigravity conversation_summaries + conversations_fts)
class SqliteReadOnlyParser final : public IAntigravityParser {
public:
    std::string name() const override { return "sqlite-readonly"; }

    bool can_parse(const std::filesystem::path& app_data_dir) const override {
        std::error_code ec;
        if (!std::filesystem::exists(app_data_dir, ec)) return false;
        if (std::filesystem::exists(app_data_dir / "conversation_summaries.db", ec)) return true;
        for (const auto& entry : std::filesystem::directory_iterator(app_data_dir, ec)) {
            if (entry.path().extension() == ".db" || entry.path().extension() == ".sqlite") {
                return true;
            }
        }
        return false;
    }

    ParserProbeInfo probe(const std::filesystem::path& app_data_dir) const override {
        ParserProbeInfo info;
        info.parser_name = name();
        info.detected_format = "sqlite3 (WAL-snapshot + uri=mode=ro&immutable=1)";
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(app_data_dir, ec)) {
            if (entry.path().extension() != ".db" && entry.path().extension() != ".sqlite") continue;
            SnapshotDbHandle snap;
            if (open_wal_safe_readonly(entry.path(), snap)) {
                sqlite3_stmt* st = nullptr;
                if (table_exists(snap.db, "conversation_summaries")) {
                    if (sqlite3_prepare_v2(snap.db, "SELECT COUNT(*) FROM conversation_summaries;", -1, &st, nullptr) == SQLITE_OK) {
                        if (sqlite3_step(st) == SQLITE_ROW) {
                            info.compatible = true;
                            info.discovered_items += static_cast<std::size_t>(sqlite3_column_int(st, 0));
                        }
                        sqlite3_finalize(st);
                    }
                } else if (sqlite3_prepare_v2(snap.db, "SELECT COUNT(*) FROM sqlite_master WHERE type='table';", -1, &st, nullptr) == SQLITE_OK) {
                    if (sqlite3_step(st) == SQLITE_ROW) {
                        info.compatible = true;
                        info.discovered_items += static_cast<std::size_t>(sqlite3_column_int(st, 0));
                    }
                    sqlite3_finalize(st);
                }
            }
        }
        const auto conv_dir = app_data_dir / "conversations";
        std::size_t conv_dbs = 0;
        if (std::filesystem::exists(conv_dir, ec)) {
            for (const auto& f : std::filesystem::directory_iterator(conv_dir, ec)) {
                if (f.path().extension() == ".db") ++conv_dbs;
            }
        }
        info.diagnostic = info.compatible
                              ? ("WAL-safe read-only snapshot verified (summaries/tables=" +
                                 std::to_string(info.discovered_items) + ", trajectory_dbs=" +
                                 std::to_string(conv_dbs) + ")")
                              : "No compatible SQLite databases found or tables unreadable";
        return info;
    }

    std::vector<ParsedConversationBundle> scan(
        const std::filesystem::path& app_data_dir,
        const std::string& local_node_id,
        const core::PrivacyConfig& privacy) const override {
        std::vector<ParsedConversationBundle> results;
        std::error_code ec;
        const auto db_file = app_data_dir / "conversation_summaries.db";
        if (!std::filesystem::exists(db_file, ec)) return results;

        SnapshotDbHandle snap;
        if (!open_wal_safe_readonly(db_file, snap)) {
            return results;
        }

        const auto annotations = load_annotations_map(app_data_dir);
        const auto active_locks = load_presence_active_set(app_data_dir);

        // Primary path: exact schema of Antigravity conversation_summaries + conversations_fts
        if (table_exists(snap.db, "conversation_summaries")) {
            std::unordered_map<std::string, std::size_t> by_id;
            const char* sum_sql =
                "SELECT conversation_id, title, preview, step_count, last_modified_time, "
                "workspace_uris, status, project_id, not_fully_idle, killed "
                "FROM conversation_summaries ORDER BY last_modified_time DESC LIMIT 300;";
            sqlite3_stmt* st = nullptr;
            int step_rc = SQLITE_OK;
            if (sqlite3_prepare_v2(snap.db, sum_sql, -1, &st, nullptr) == SQLITE_OK) {
                while ((step_rc = sqlite3_step(st)) == SQLITE_ROW) {
                    const std::string cid = safe_col_text(st, 0);
                    if (cid.empty()) continue;

                    std::string title = safe_col_text(st, 1);
                    const std::string preview = safe_col_text(st, 2);
                    const int step_count = sqlite3_column_int(st, 3);
                    const std::string last_mod = safe_col_text(st, 4);
                    const std::string ws_uris = safe_col_text(st, 5);
                    const std::string raw_status = safe_col_text(st, 6);
                    const std::string project_id = safe_col_text(st, 7);
                    const int not_idle = sqlite3_column_int(st, 8);
                    const int killed = sqlite3_column_int(st, 9);

                    const std::string ws_path = uri_to_local_path(ws_uris);
                    if (!ws_path.empty() && !core::ConfigManager::is_path_allowed(privacy, ws_path)) {
                        continue;
                    }

                    ParsedConversationBundle bundle;
                    bundle.session.session_id = cid;
                    bundle.session.node_id = local_node_id;
                    bundle.session.workspace_path = ws_path;
                    bundle.session.source_adapter = name();
                    bundle.session.updated_at_ms = parse_iso8601_utc_ms(last_mod);
                    bundle.session.created_at_ms = bundle.session.updated_at_ms;

                    if (killed != 0) {
                        bundle.session.status = "archived";
                    } else if (raw_status.find("RUNNING") != std::string::npos ||
                               not_idle != 0 ||
                               active_locks.count(cid) > 0) {
                        bundle.session.status = "active";
                    } else {
                        bundle.session.status = "idle";
                    }

                    auto ann_it = annotations.find(cid);
                    if (ann_it != annotations.end()) {
                        if (title.empty() && !ann_it->second.first.empty()) title = ann_it->second.first;
                        if (ann_it->second.second > bundle.session.updated_at_ms) {
                            bundle.session.updated_at_ms = ann_it->second.second;
                        }
                    }
                    if (title.empty()) title = preview;
                    if (title.empty()) title = "Antigravity Session " + cid.substr(0, std::min<std::size_t>(8, cid.size()));
                    bundle.session.title = title;

                    if (!ws_path.empty()) {
                        std::string repo, branch, commit;
                        if (AntigravityAdapter::inspect_git_repo(ws_path, repo, branch, commit)) {
                            bundle.session.repo_name = repo;
                            bundle.session.git_branch = branch;
                        }
                    }
                    if (bundle.session.repo_name.empty() && !project_id.empty() && project_id != "outside-of-project") {
                        bundle.session.repo_name = project_id;
                    }

                    if (!preview.empty() || step_count > 0) {
                        core::SessionSummary sm;
                        sm.session_id = cid;
                        sm.node_id = local_node_id;
                        sm.current_goal = title;
                        sm.summary_text = preview.empty()
                                              ? (title + " (" + std::to_string(step_count) + " steps)")
                                              : preview;
                        sm.updated_at_ms = bundle.session.updated_at_ms;
                        bundle.summary = std::move(sm);
                    }

                    by_id[cid] = results.size();
                    results.push_back(std::move(bundle));
                }
                sqlite3_finalize(st);
                if (step_rc != SQLITE_DONE) {
                    // Snapshot caught a torn checkpoint mid-query; silently skip this pass.
                    return {};
                }
            }

            // Enrich with searchable user_body / agent_body chunks from conversations_fts
            if (table_exists(snap.db, "conversations_fts")) {
                const char* fts_sql =
                    "SELECT conversation_id, chunk_index, user_body, agent_body "
                    "FROM conversations_fts WHERE chunk_index >= 0 ORDER BY conversation_id, chunk_index ASC LIMIT 1000;";
                sqlite3_stmt* fst = nullptr;
                int fts_rc = SQLITE_OK;
                if (sqlite3_prepare_v2(snap.db, fts_sql, -1, &fst, nullptr) == SQLITE_OK) {
                    while ((fts_rc = sqlite3_step(fst)) == SQLITE_ROW) {
                        const std::string cid = safe_col_text(fst, 0);
                        auto it = by_id.find(cid);
                        if (it == by_id.end()) continue;
                        auto& bundle = results[it->second];
                        const int chunk_idx = sqlite3_column_int(fst, 1);
                        const std::string user_txt = safe_col_text(fst, 2);
                        const std::string agent_txt = safe_col_text(fst, 3);

                        auto add_chunk_msg = [&](const std::string& role, const std::string& body, int sub) {
                            if (body.empty()) return;
                            core::MessageRecord m;
                            m.session_id = cid;
                            m.node_id = local_node_id;
                            m.seq_index = static_cast<std::int64_t>(chunk_idx) * 2 + sub;
                            m.role = role;
                            m.content = privacy.metadata_only ? "[metadata-only mode]" : body;
                            m.created_at_ms = bundle.session.updated_at_ms;
                            m.content_sha256 = core::CrdtResolver::compute_message_hash(
                                m.session_id, m.role, m.seq_index, m.content);
                            m.message_id = cid + ":fts:" + m.content_sha256.substr(0, 16);
                            bundle.messages.push_back(std::move(m));
                        };
                        add_chunk_msg("user", user_txt, 0);
                        add_chunk_msg("assistant", agent_txt, 1);
                    }
                    sqlite3_finalize(fst);
                    if (fts_rc != SQLITE_DONE) {
                        return {};
                    }
                }
            }
            return results;
        }

        // Generic fallback introspection if a future IDE version changes table names
        sqlite3_stmt* tbl_st = nullptr;
        if (sqlite3_prepare_v2(snap.db, "SELECT name FROM sqlite_master WHERE type='table';", -1, &tbl_st, nullptr) == SQLITE_OK) {
            while (sqlite3_step(tbl_st) == SQLITE_ROW) {
                const char* tname = reinterpret_cast<const char*>(sqlite3_column_text(tbl_st, 0));
                if (!tname) continue;
                const std::string table_name(tname);
                if (table_name.find("conversation") == std::string::npos &&
                    table_name.find("summary") == std::string::npos &&
                    table_name.find("session") == std::string::npos) {
                    continue;
                }
                const std::string query = "SELECT * FROM \"" + table_name + "\" LIMIT 200;";
                sqlite3_stmt* row_st = nullptr;
                if (sqlite3_prepare_v2(snap.db, query.c_str(), -1, &row_st, nullptr) == SQLITE_OK) {
                    const int ncols = sqlite3_column_count(row_st);
                    while (sqlite3_step(row_st) == SQLITE_ROW) {
                        ParsedConversationBundle bundle;
                        bundle.session.node_id = local_node_id;
                        bundle.session.source_adapter = name();
                        bundle.session.status = "active";
                        bundle.session.created_at_ms = core::now_ms();
                        bundle.session.updated_at_ms = bundle.session.created_at_ms;

                        for (int c = 0; c < ncols; ++c) {
                            const char* cname_c = sqlite3_column_name(row_st, c);
                            const std::string val = safe_col_text(row_st, c);
                            if (!cname_c || val.empty()) continue;
                            std::string cname(cname_c);
                            if (cname.find("id") != std::string::npos && bundle.session.session_id.empty()) {
                                bundle.session.session_id = val;
                            } else if (cname.find("title") != std::string::npos) {
                                bundle.session.title = val;
                            } else if (cname.find("workspace") != std::string::npos || cname.find("path") != std::string::npos) {
                                bundle.session.workspace_path = val;
                            } else if (cname.find("summary") != std::string::npos) {
                                core::SessionSummary sm;
                                sm.node_id = local_node_id;
                                sm.summary_text = val;
                                sm.updated_at_ms = bundle.session.updated_at_ms;
                                bundle.summary = std::move(sm);
                            }
                        }
                        if (!bundle.session.session_id.empty()) {
                            if (!bundle.session.workspace_path.empty() &&
                                !core::ConfigManager::is_path_allowed(privacy, bundle.session.workspace_path)) {
                                continue;
                            }
                            if (bundle.summary) bundle.summary->session_id = bundle.session.session_id;
                            results.push_back(std::move(bundle));
                        }
                    }
                    sqlite3_finalize(row_st);
                }
            }
            sqlite3_finalize(tbl_st);
        }

        return results;
    }
};

// 2. JSONL Transcript Parser (parses brain/<conv-id>/.system_generated/logs/transcript.jsonl resiliently)
class JsonlTranscriptParser final : public IAntigravityParser {
public:
    std::string name() const override { return "jsonl-transcript"; }

    bool can_parse(const std::filesystem::path& app_data_dir) const override {
        std::error_code ec;
        return std::filesystem::exists(app_data_dir / "brain", ec);
    }

    ParserProbeInfo probe(const std::filesystem::path& app_data_dir) const override {
        ParserProbeInfo info;
        info.parser_name = name();
        info.detected_format = "jsonl (brain/<id>/.system_generated/logs/transcript.jsonl)";
        std::error_code ec;
        const auto brain_dir = app_data_dir / "brain";
        if (std::filesystem::exists(brain_dir, ec)) {
            for (const auto& conv : std::filesystem::directory_iterator(brain_dir, ec)) {
                if (!conv.is_directory(ec)) continue;
                const auto tr = conv.path() / ".system_generated" / "logs" / "transcript.jsonl";
                if (std::filesystem::exists(tr, ec)) {
                    ++info.discovered_items;
                }
            }
        }
        info.compatible = info.discovered_items > 0;
        info.diagnostic = info.compatible
                              ? "Found " + std::to_string(info.discovered_items) + " JSONL transcript logs"
                              : "No JSONL transcripts discovered under brain/";
        return info;
    }

    std::vector<ParsedConversationBundle> scan(
        const std::filesystem::path& app_data_dir,
        const std::string& local_node_id,
        const core::PrivacyConfig& privacy) const override {
        std::vector<ParsedConversationBundle> bundles;
        std::error_code ec;
        const auto brain_dir = app_data_dir / "brain";
        if (!std::filesystem::exists(brain_dir, ec)) return bundles;

        for (const auto& conv : std::filesystem::directory_iterator(brain_dir, ec)) {
            if (!conv.is_directory(ec)) continue;
            const std::string conv_id = conv.path().filename().string();
            const auto tr_path = conv.path() / ".system_generated" / "logs" / "transcript.jsonl";
            if (!std::filesystem::exists(tr_path, ec)) continue;

            std::ifstream ifs(tr_path, std::ios::binary);
            if (!ifs) continue;

            ParsedConversationBundle bundle;
            bundle.session.session_id = conv_id;
            bundle.session.node_id = local_node_id;
            bundle.session.source_adapter = name();
            bundle.session.updated_at_ms = file_mtime_ms(tr_path);
            bundle.session.created_at_ms = bundle.session.updated_at_ms;
            bundle.session.status = (core::now_ms() - bundle.session.updated_at_ms < 300000) ? "active" : "idle";

            std::string line;
            std::int64_t seq = 0;
            while (std::getline(ifs, line)) {
                if (line.empty()) continue;
                try {
                    // Resilient against half-written trailing lines at append time.
                    auto j = nlohmann::json::parse(line);
                    if (!j.is_object()) continue;

                    const std::string step_type = j.value("type", "");
                    const std::string content = j.value("content", "");
                    if (content.empty()) continue;

                    core::MessageRecord msg;
                    msg.session_id = conv_id;
                    msg.node_id = local_node_id;
                    msg.seq_index = seq++;
                    msg.created_at_ms = bundle.session.updated_at_ms;
                    if (step_type == "USER_INPUT" || j.value("source", "") == "USER_EXPLICIT") {
                        msg.role = "user";
                        if (bundle.session.title.empty()) {
                            bundle.session.title = content.substr(0, std::min<std::size_t>(96, content.size()));
                        }
                    } else if (step_type == "PLANNER_RESPONSE") {
                        msg.role = "assistant";
                    } else {
                        msg.role = "system";
                    }
                    msg.content = privacy.metadata_only ? "[metadata-only mode]" : content;
                    msg.content_sha256 = core::CrdtResolver::compute_message_hash(
                        msg.session_id, msg.role, msg.seq_index, msg.content);
                    msg.message_id = conv_id + ":" + msg.content_sha256.substr(0, 16);
                    bundle.messages.push_back(std::move(msg));
                } catch (...) {
                    // Skip incomplete or malformed JSON lines safely.
                    continue;
                }
            }

            if (bundle.session.title.empty()) {
                bundle.session.title = "Antigravity Session " + conv_id.substr(0, std::min<std::size_t>(8, conv_id.size()));
            }
            if (!bundle.messages.empty()) {
                bundles.push_back(std::move(bundle));
            }
        }
        return bundles;
    }
};

// 3. Markdown Artifacts Parser (reads brain/<conv-id>/*.md plans, walkthroughs, summaries)
class MarkdownArtifactParser final : public IAntigravityParser {
public:
    std::string name() const override { return "markdown-artifacts"; }

    bool can_parse(const std::filesystem::path& app_data_dir) const override {
        std::error_code ec;
        return std::filesystem::exists(app_data_dir / "brain", ec);
    }

    ParserProbeInfo probe(const std::filesystem::path& app_data_dir) const override {
        ParserProbeInfo info;
        info.parser_name = name();
        info.detected_format = "markdown (brain/<id>/*.md)";
        std::error_code ec;
        const auto brain_dir = app_data_dir / "brain";
        if (std::filesystem::exists(brain_dir, ec)) {
            for (const auto& conv : std::filesystem::directory_iterator(brain_dir, ec)) {
                if (!conv.is_directory(ec)) continue;
                for (const auto& f : std::filesystem::directory_iterator(conv.path(), ec)) {
                    if (f.path().extension() == ".md") ++info.discovered_items;
                }
            }
        }
        info.compatible = info.discovered_items > 0;
        info.diagnostic = "Detected " + std::to_string(info.discovered_items) + " markdown artifacts";
        return info;
    }

    std::vector<ParsedConversationBundle> scan(
        const std::filesystem::path& app_data_dir,
        const std::string& local_node_id,
        const core::PrivacyConfig& privacy) const override {
        std::vector<ParsedConversationBundle> bundles;
        std::error_code ec;
        const auto brain_dir = app_data_dir / "brain";
        if (!std::filesystem::exists(brain_dir, ec)) return bundles;

        for (const auto& conv : std::filesystem::directory_iterator(brain_dir, ec)) {
            if (!conv.is_directory(ec)) continue;
            const std::string conv_id = conv.path().filename().string();
            ParsedConversationBundle bundle;
            bundle.session.session_id = conv_id;
            bundle.session.node_id = local_node_id;
            bundle.session.source_adapter = name();

            for (const auto& f : std::filesystem::directory_iterator(conv.path(), ec)) {
                if (!f.is_regular_file(ec) || f.path().extension() != ".md") continue;
                const std::string rel = f.path().filename().string();
                if (!core::ConfigManager::is_path_allowed(privacy, f.path().string())) continue;

                const std::string content = read_file_safe(f.path());
                if (content.empty()) continue;

                core::ArtifactRecord art;
                art.session_id = conv_id;
                art.node_id = local_node_id;
                art.relative_path = rel;
                art.mime_type = "text/markdown";
                art.content_text = privacy.metadata_only ? "" : content;
                art.size_bytes = static_cast<std::int64_t>(content.size());
                art.content_sha256 = core::CrdtResolver::sha256_hex(content);
                art.artifact_id = conv_id + ":" + rel;
                art.updated_at_ms = file_mtime_ms(f.path());

                if (bundle.session.title.empty()) {
                    bundle.session.title = extract_first_heading(content);
                }
                bundle.session.updated_at_ms = std::max(bundle.session.updated_at_ms, art.updated_at_ms);
                bundle.artifacts.push_back(std::move(art));
            }
            if (!bundle.artifacts.empty()) {
                if (bundle.session.created_at_ms == 0) {
                    bundle.session.created_at_ms = bundle.session.updated_at_ms;
                }
                if (bundle.session.title.empty()) {
                    bundle.session.title = "Artifacts " + conv_id.substr(0, std::min<std::size_t>(8, conv_id.size()));
                }
                bundles.push_back(std::move(bundle));
            }
        }
        return bundles;
    }
};

// 4. Protobuf / pbtxt State Parser (parses antigravity_state.pbtxt & binary .pb fallback)
class ProtobufStateParser final : public IAntigravityParser {
public:
    std::string name() const override { return "protobuf-state"; }

    bool can_parse(const std::filesystem::path& app_data_dir) const override {
        std::error_code ec;
        return std::filesystem::exists(app_data_dir / "antigravity_state.pbtxt", ec) ||
               std::filesystem::exists(app_data_dir / "agyhub_summaries_proto.pb", ec);
    }

    ParserProbeInfo probe(const std::filesystem::path& app_data_dir) const override {
        ParserProbeInfo info;
        info.parser_name = name();
        info.detected_format = "pbtxt / binary protobuf strings";
        std::error_code ec;
        if (std::filesystem::exists(app_data_dir / "antigravity_state.pbtxt", ec)) ++info.discovered_items;
        if (std::filesystem::exists(app_data_dir / "agyhub_summaries_proto.pb", ec)) ++info.discovered_items;
        info.compatible = info.discovered_items > 0;
        info.diagnostic = info.compatible ? "Detected Antigravity state protobuf files" : "No pb/pbtxt files found";
        return info;
    }

    std::vector<ParsedConversationBundle> scan(
        const std::filesystem::path& app_data_dir,
        const std::string& local_node_id,
        const core::PrivacyConfig& privacy) const override {
        std::vector<ParsedConversationBundle> bundles;
        const auto pbtxt = app_data_dir / "antigravity_state.pbtxt";
        std::error_code ec;
        if (!std::filesystem::exists(pbtxt, ec)) return bundles;

        const std::string text = read_file_safe(pbtxt);
        if (text.empty()) return bundles;

        std::istringstream iss(text);
        std::string line;
        while (std::getline(iss, line)) {
            const auto pos = line.find("workspace");
            if (pos != std::string::npos) {
                const auto q1 = line.find('"');
                const auto q2 = line.rfind('"');
                if (q1 != std::string::npos && q2 != std::string::npos && q2 > q1 + 1) {
                    const std::string ws_path = line.substr(q1 + 1, q2 - q1 - 1);
                    if (!core::ConfigManager::is_path_allowed(privacy, ws_path)) continue;
                    ParsedConversationBundle b;
                    b.session.session_id = "state-" + core::CrdtResolver::sha256_hex(ws_path).substr(0, 12);
                    b.session.node_id = local_node_id;
                    b.session.workspace_path = ws_path;
                    b.session.source_adapter = name();
                    b.session.updated_at_ms = file_mtime_ms(pbtxt);
                    b.session.created_at_ms = b.session.updated_at_ms;
                    std::string repo, branch, commit;
                    if (AntigravityAdapter::inspect_git_repo(ws_path, repo, branch, commit)) {
                        b.session.repo_name = repo;
                        b.session.git_branch = branch;
                    }
                    b.session.title = "Workspace " + (repo.empty() ? ws_path : repo);
                    bundles.push_back(std::move(b));
                }
            }
        }
        return bundles;
    }
};

} // namespace

AntigravityAdapter::AntigravityAdapter(core::AntigravityPathsConfig paths_config)
    : paths_config_(std::move(paths_config)) {
    parsers_.push_back(std::make_shared<JsonlTranscriptParser>());
    parsers_.push_back(std::make_shared<MarkdownArtifactParser>());
    parsers_.push_back(std::make_shared<SqliteReadOnlyParser>());
    parsers_.push_back(std::make_shared<ProtobufStateParser>());
}

void AntigravityAdapter::register_parser(std::shared_ptr<IAntigravityParser> parser) {
    if (parser) {
        parsers_.push_back(std::move(parser));
    }
}

std::filesystem::path AntigravityAdapter::detect_app_data_dir() const {
    if (!paths_config_.app_data_dir.empty()) {
        return std::filesystem::path(paths_config_.app_data_dir);
    }

#ifdef _WIN32
    const auto userprofile = env_var("USERPROFILE");
    if (!userprofile.empty()) {
        const auto p = std::filesystem::path(userprofile) / ".gemini" / "antigravity";
        if (std::filesystem::exists(p)) return p;
    }
    const auto appdata = env_var("APPDATA");
    if (!appdata.empty()) {
        return std::filesystem::path(appdata) / "Antigravity";
    }
    return std::filesystem::path(".") / ".gemini" / "antigravity";
#else
    const auto home = env_var("HOME");
    const std::filesystem::path primary = std::filesystem::path(home.empty() ? "." : home) / ".gemini" / "antigravity";
    if (std::filesystem::exists(primary)) return primary;
#ifdef __APPLE__
    const auto mac_alt = std::filesystem::path(home) / "Library" / "Application Support" / "Antigravity";
    if (std::filesystem::exists(mac_alt)) return mac_alt;
#else
    const auto xdg = env_var("XDG_CONFIG_HOME");
    if (!xdg.empty()) {
        const auto lin_alt = std::filesystem::path(xdg) / "Antigravity";
        if (std::filesystem::exists(lin_alt)) return lin_alt;
    }
#endif
    return primary;
#endif
}

bool AntigravityAdapter::inspect_git_repo(const std::filesystem::path& workspace_path,
                                          std::string& repo_name_out,
                                          std::string& branch_out,
                                          std::string& commit_out) {
    std::error_code ec;
    if (workspace_path.empty() || !std::filesystem::exists(workspace_path, ec)) return false;

    std::filesystem::path cur = workspace_path;
    std::filesystem::path git_dir;
    for (int depth = 0; depth < 8; ++depth) {
        const auto cand = cur / ".git";
        if (std::filesystem::exists(cand, ec)) {
            git_dir = cand;
            repo_name_out = cur.filename().string();
            break;
        }
        if (!cur.has_parent_path() || cur == cur.parent_path()) break;
        cur = cur.parent_path();
    }
    if (git_dir.empty()) {
        repo_name_out = workspace_path.filename().string();
        return false;
    }

    // Handle git worktree files (".git" file pointing to "gitdir: ...")
    if (std::filesystem::is_regular_file(git_dir, ec)) {
        const std::string content = read_file_safe(git_dir, 4096);
        const std::string prefix = "gitdir:";
        if (content.rfind(prefix, 0) == 0) {
            std::string target = content.substr(prefix.size());
            while (!target.empty() && (target.front() == ' ' || target.front() == '\t')) target.erase(target.begin());
            while (!target.empty() && (target.back() == '\r' || target.back() == '\n')) target.pop_back();
            git_dir = std::filesystem::path(target);
        }
    }

    const std::string head = read_file_safe(git_dir / "HEAD", 1024);
    if (head.rfind("ref:", 0) == 0) {
        std::string ref = head.substr(4);
        while (!ref.empty() && (ref.front() == ' ' || ref.front() == '\t')) ref.erase(ref.begin());
        while (!ref.empty() && (ref.back() == '\r' || ref.back() == '\n')) ref.pop_back();
        const std::string heads_prefix = "refs/heads/";
        if (ref.rfind(heads_prefix, 0) == 0) {
            branch_out = ref.substr(heads_prefix.size());
        } else {
            branch_out = ref;
        }
        const std::string ref_hash = read_file_safe(git_dir / ref, 128);
        if (!ref_hash.empty()) {
            commit_out = ref_hash.substr(0, std::min<std::size_t>(12, ref_hash.find_first_of("\r\n")));
        }
    } else if (!head.empty()) {
        branch_out = "DETACHED";
        commit_out = head.substr(0, std::min<std::size_t>(12, head.find_first_of("\r\n")));
    }
    return true;
}

std::vector<core::WorkspaceInfo> AntigravityAdapter::detect_active_workspaces(
    const std::string& local_node_id) const {
    std::vector<core::WorkspaceInfo> out;
    std::error_code ec;

    auto inspect_root = [&](const std::filesystem::path& p, bool is_open_flag) {
        if (!std::filesystem::exists(p, ec)) return;
        core::WorkspaceInfo ws;
        ws.node_id = local_node_id;
        ws.root_path = p.string();
        ws.workspace_id = local_node_id + ":" + core::CrdtResolver::sha256_hex(ws.root_path).substr(0, 12);
        ws.is_open = is_open_flag;
        ws.last_active_ms = file_mtime_ms(p);
        inspect_git_repo(p, ws.repo_name, ws.git_branch, ws.git_commit);
        if (ws.repo_name.empty()) ws.repo_name = p.filename().string();
        out.push_back(std::move(ws));
    };

    for (const auto& extra : paths_config_.extra_workspace_roots) {
        inspect_root(std::filesystem::path(extra), true);
    }

    const auto app_dir = detect_app_data_dir();
    const auto scratch_dir = app_dir / "scratch";
    if (std::filesystem::exists(scratch_dir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(scratch_dir, ec)) {
            if (entry.is_directory(ec)) {
                inspect_root(entry.path(), false);
            }
        }
    }
    return out;
}

ProbeReport AntigravityAdapter::run_probe() const {
    ProbeReport rep;
    rep.os_name = core::ConfigManager::detect_os_name();
    rep.app_data_dir = detect_app_data_dir();

    std::error_code ec;
    rep.app_data_exists = std::filesystem::exists(rep.app_data_dir, ec);
    if (!rep.app_data_exists) {
        rep.warnings.push_back("Antigravity data directory does not exist at: " + rep.app_data_dir.string());
        return rep;
    }

    // Verify readability
    auto it = std::filesystem::directory_iterator(rep.app_data_dir, ec);
    rep.app_data_readable = !ec;
    if (ec) {
        rep.warnings.push_back("Cannot read Antigravity data directory: " + ec.message());
        return rep;
    }

    for (const auto& entry : it) {
        const auto ext = entry.path().extension();
        if (ext == ".db" || ext == ".sqlite") {
            rep.found_databases.push_back(entry.path());
        } else if (ext == ".pb" || ext == ".pbtxt") {
            rep.found_state_files.push_back(entry.path());
        }
    }

    const auto brain_dir = rep.app_data_dir / "brain";
    if (std::filesystem::exists(brain_dir, ec)) {
        for (const auto& conv : std::filesystem::directory_iterator(brain_dir, ec)) {
            if (!conv.is_directory(ec)) continue;
            const auto tr = conv.path() / ".system_generated" / "logs" / "transcript.jsonl";
            if (std::filesystem::exists(tr, ec)) rep.found_transcripts.push_back(tr);
            for (const auto& f : std::filesystem::directory_iterator(conv.path(), ec)) {
                if (f.path().extension() == ".md") rep.found_artifacts.push_back(f.path());
            }
        }
    }

    for (const auto& parser : parsers_) {
        rep.parsers.push_back(parser->probe(rep.app_data_dir));
    }
    rep.active_workspaces = detect_active_workspaces("local-probe");
    return rep;
}

void AntigravityAdapter::ingest_mcp_hook_event(const ParsedConversationBundle& bundle) {
    std::lock_guard<std::mutex> lock(hook_mutex_);
    mcp_hook_buffer_.push_back(bundle);
}

std::vector<ParsedConversationBundle> AntigravityAdapter::collect_updates(
    const std::string& local_node_id,
    const core::PrivacyConfig& privacy) const {
    std::vector<ParsedConversationBundle> collected;
    const auto app_dir = detect_app_data_dir();
    std::error_code ec;
    if (std::filesystem::exists(app_dir, ec)) {
        for (const auto& parser : parsers_) {
            if (parser->can_parse(app_dir)) {
                auto part = parser->scan(app_dir, local_node_id, privacy);
                collected.insert(collected.end(),
                                 std::make_move_iterator(part.begin()),
                                 std::make_move_iterator(part.end()));
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(hook_mutex_);
        if (!mcp_hook_buffer_.empty()) {
            collected.insert(collected.end(),
                             std::make_move_iterator(mcp_hook_buffer_.begin()),
                             std::make_move_iterator(mcp_hook_buffer_.end()));
            mcp_hook_buffer_.clear();
        }
    }

    return collected;
}

} // namespace agmesh::adapter
