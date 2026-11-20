#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace agmesh::ipc {

struct IpcRequest {
    std::string command;
    std::string payload_json;
};

struct IpcResponse {
    bool ok{false};
    std::string body_json;
    std::string error;
};

// Cross-platform Local IPC (Unix Domain Socket with mode 0600 on POSIX, Named Pipe on Windows).
class LocalIpcServer {
public:
    using RequestHandler = std::function<IpcResponse(const IpcRequest&)>;
    using StreamSubscriber = std::function<void(const std::string& event_json_line)>;

    explicit LocalIpcServer(std::string endpoint_path);
    ~LocalIpcServer();

    LocalIpcServer(const LocalIpcServer&) = delete;
    LocalIpcServer& operator=(const LocalIpcServer&) = delete;

    bool start(RequestHandler handler, std::string& error_out);
    void stop();
    [[nodiscard]] bool is_running() const noexcept;

    // Broadcasts a live event line to `ag-mesh tail` subscribers or stores in recent ring buffer.
    void publish_stream_event(const std::string& json_line);
    [[nodiscard]] std::vector<std::string> recent_events(std::size_t max_count = 50) const;

    [[nodiscard]] static std::string default_endpoint_for_user(const std::string& data_dir = "");

private:
    void run_server_loop();

    std::string endpoint_;
    RequestHandler handler_;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;

#ifdef _WIN32
    void* stop_event_{nullptr};
#else
    int listen_fd_{-1};
#endif

    mutable std::mutex events_mutex_;
    std::vector<std::string> event_ring_buffer_;
};

class LocalIpcClient {
public:
    explicit LocalIpcClient(std::string endpoint_path);

    [[nodiscard]] IpcResponse call(const std::string& command,
                                   const std::string& payload_json = "{}",
                                   int timeout_ms = 3000) const;
    [[nodiscard]] bool is_daemon_alive() const;

private:
    std::string endpoint_;
};

} // namespace agmesh::ipc
