#include "agmesh/tui/dashboard_tui.hpp"

#include <nlohmann/json.hpp>

#if AGMESH_HAS_FTXUI
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#endif

#include <atomic>
#include <chrono>
#include <iostream>
#include <sstream>
#include <thread>

namespace agmesh::tui {

DashboardTui::DashboardTui(core::ConfigManager& config_mgr,
                           storage::SqliteStore& store,
                           ipc::LocalIpcClient* ipc_client_or_null)
    : config_mgr_(config_mgr),
      store_(store),
      ipc_client_(ipc_client_or_null) {}

int DashboardTui::run() {
#if AGMESH_HAS_FTXUI
    using namespace ftxui;

    const auto cfg = config_mgr_.snapshot();
    std::string search_query;
    auto input_search = Input(&search_query, "Search FTS5 context (Ctrl+C or 'q' / Esc to quit)...");

    auto screen = ScreenInteractive::Fullscreen();
    std::atomic<bool> running{true};

    std::thread refresher([&]() {
        while (running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            screen.PostEvent(Event::Custom);
        }
    });

    auto renderer = Renderer(input_search, [&]() {
        const auto ov = store_.get_cluster_overview(cfg.node_id);

        Elements node_rows;
        node_rows.push_back(hbox({
            text("NODE ID") | bold | size(WIDTH, EQUAL, 18),
            text("OS") | bold | size(WIDTH, EQUAL, 10),
            text("STATUS") | bold | size(WIDTH, EQUAL, 10),
            text("RTT(ms)") | bold | size(WIDTH, EQUAL, 10),
            text("ENDPOINT") | bold | flex
        }));
        for (const auto& n : ov.nodes) {
            const auto st_str = core::to_string(n.online_status);
            auto st_color = (n.online_status == core::OnlineStatus::Online) ? Color::Green : Color::Red;
            std::ostringstream rtt_ss;
            if (n.rtt_ms >= 0.0) rtt_ss << n.rtt_ms;
            else rtt_ss << "-";

            node_rows.push_back(hbox({
                text(n.node_id) | size(WIDTH, EQUAL, 18),
                text(n.os_name) | size(WIDTH, EQUAL, 10),
                text(st_str) | color(st_color) | size(WIDTH, EQUAL, 10),
                text(rtt_ss.str()) | size(WIDTH, EQUAL, 10),
                text(n.endpoint) | flex
            }));
        }

        Elements session_rows;
        for (const auto& s : ov.recent_sessions) {
            session_rows.push_back(hbox({
                text("[" + s.node_id + "] ") | color(Color::Cyan),
                text(s.repo_name + ":" + s.git_branch + " ") | color(Color::Yellow),
                text(s.title) | flex
            }));
        }
        if (session_rows.empty()) {
            session_rows.push_back(text("No synchronized sessions yet.") | dim);
        }

        Elements right_rows;
        if (!search_query.empty()) {
            const auto hits = store_.search_fts(search_query, "", "", 12);
            for (const auto& h : hits) {
                right_rows.push_back(vbox({
                    hbox({text("[" + h.node_id + "] ") | color(Color::Green), text(h.title) | bold}),
                    text(h.snippet) | dim
                }));
            }
            if (hits.empty()) {
                right_rows.push_back(text("No FTS5 hits for: " + search_query) | dim);
            }
        } else if (ipc_client_ && ipc_client_->is_daemon_alive()) {
            const auto ev_resp = ipc_client_->call("tail_events", "{}");
            if (ev_resp.ok) {
                const auto j = nlohmann::json::parse(ev_resp.body_json, nullptr, false);
                if (j.contains("events") && j["events"].is_array()) {
                    for (const auto& ev : j["events"]) {
                        right_rows.push_back(text(ev.is_string() ? ev.get<std::string>() : ev.dump()));
                    }
                }
            }
        } else {
            for (const auto& h : ov.pending_handoffs) {
                right_rows.push_back(text("Handoff " + h.handoff_id + ": " + h.task_description));
            }
            if (right_rows.empty()) {
                right_rows.push_back(text("Daemon event stream idle (type above to search FTS5).") | dim);
            }
        }

        return vbox({
                   hbox({
                       text(" ag-mesh TUI ") | bold | inverted,
                       text(" Local Node: " + cfg.node_id + " "),
                       filler(),
                       text(" Esc: Quit ") | dim
                   }),
                   separator(),
                   hbox({
                       text(" FTS5 Search: ") | bold,
                       input_search->Render() | flex
                   }) | border,
                   hbox({
                       vbox({
                           text(" Cluster Nodes ") | bold | center,
                           separator(),
                           vbox(std::move(node_rows)) | yframe
                       }) | border | flex,
                       vbox({
                           text(" Active & Recent Sessions ") | bold | center,
                           separator(),
                           vbox(std::move(session_rows)) | yframe
                       }) | border | flex
                   }) | size(HEIGHT, GREATER_THAN, 10),
                   vbox({
                       text(search_query.empty() ? " Live Cluster Events & Handoffs " : " Instant FTS5 Search Results ") | bold,
                       separator(),
                       vbox(std::move(right_rows)) | yframe | flex
                   }) | border | flex
               });
    });

    auto component = CatchEvent(renderer, [&](Event ev) {
        if (ev == Event::Escape) {
            screen.ExitLoopClosure()();
            return true;
        }
        return false;
    });

    screen.Loop(component);
    running.store(false);
    if (refresher.joinable()) refresher.join();
    return 0;
#else
    const auto cfg = config_mgr_.snapshot();
    const auto ov = store_.get_cluster_overview(cfg.node_id);
    std::cout << "ag-mesh TUI (FTXUI disabled at build time). Nodes: " << ov.nodes.size()
              << ", Sessions: " << ov.recent_sessions.size() << "\n";
    (void)ipc_client_;
    return 0;
#endif
}

} // namespace agmesh::tui
