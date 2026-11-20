#pragma once

#include "agmesh/core/config.hpp"
#include "agmesh/ipc/local_ipc.hpp"
#include "agmesh/storage/sqlite_store.hpp"

#include <string>

namespace agmesh::tui {

// Interactive FTXUI terminal dashboard displaying node matrix, RTT, active chats, live stream, and FTS5 search.
class DashboardTui {
public:
    DashboardTui(core::ConfigManager& config_mgr,
                 storage::SqliteStore& store,
                 ipc::LocalIpcClient* ipc_client_or_null);

    int run();

private:
    core::ConfigManager& config_mgr_;
    storage::SqliteStore& store_;
    ipc::LocalIpcClient* ipc_client_;
};

} // namespace agmesh::tui
