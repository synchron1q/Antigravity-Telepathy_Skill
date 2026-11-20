#include "agmesh/ipc/local_ipc.hpp"
#include "agmesh/core/config.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace agmesh::ipc {

std::string LocalIpcServer::default_endpoint_for_user(const std::string& data_dir) {
#ifdef _WIN32
    char user_buf[128]{};
    DWORD len = sizeof(user_buf);
    std::string uname = "user";
    if (GetUserNameA(user_buf, &len) != 0 && len > 1) {
        uname = std::string(user_buf, len - 1);
    }
    (void)data_dir;
    return std::string(R"(\\.\pipe\ag-mesh-)") + uname;
#else
    if (!data_dir.empty()) {
        return (std::filesystem::path(data_dir) / "ag-mesh.sock").string();
    }
    return (core::ConfigManager::default_data_dir() / "ag-mesh.sock").string();
#endif
}

LocalIpcServer::LocalIpcServer(std::string endpoint_path)
    : endpoint_(std::move(endpoint_path)) {}

LocalIpcServer::~LocalIpcServer() {
    stop();
}

void LocalIpcServer::publish_stream_event(const std::string& json_line) {
    std::lock_guard<std::mutex> lock(events_mutex_);
    event_ring_buffer_.push_back(json_line);
    if (event_ring_buffer_.size() > 500) {
        event_ring_buffer_.erase(event_ring_buffer_.begin(),
                                 event_ring_buffer_.begin() + static_cast<std::ptrdiff_t>(event_ring_buffer_.size() - 500));
    }
}

std::vector<std::string> LocalIpcServer::recent_events(std::size_t max_count) const {
    std::lock_guard<std::mutex> lock(events_mutex_);
    if (event_ring_buffer_.size() <= max_count) {
        return event_ring_buffer_;
    }
    return std::vector<std::string>(
        event_ring_buffer_.end() - static_cast<std::ptrdiff_t>(max_count),
        event_ring_buffer_.end());
}

bool LocalIpcServer::start(RequestHandler handler, std::string& error_out) {
    if (running_.load()) return true;
    handler_ = std::move(handler);

#ifdef _WIN32
    stop_event_ = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    running_.store(true);
    accept_thread_ = std::thread([this]() { run_server_loop(); });
    (void)error_out;
    return true;
#else
    const std::filesystem::path p(endpoint_);
    if (p.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
    }
    ::unlink(endpoint_.c_str());

    listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        error_out = "Failed to create AF_UNIX socket";
        return false;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (endpoint_.size() >= sizeof(addr.sun_path)) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        error_out = "Unix domain socket path too long: " + endpoint_;
        return false;
    }
    std::strncpy(addr.sun_path, endpoint_.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        error_out = "Failed to bind Unix socket at " + endpoint_;
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    // Restrict socket access strictly to current user (0600)
    ::chmod(endpoint_.c_str(), S_IRUSR | S_IWUSR);

    if (::listen(listen_fd_, 32) != 0) {
        error_out = "Failed to listen on Unix socket";
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    running_.store(true);
    accept_thread_ = std::thread([this]() { run_server_loop(); });
    return true;
#endif
}

void LocalIpcServer::stop() {
    if (!running_.exchange(false)) return;
#ifdef _WIN32
    if (stop_event_) {
        SetEvent(static_cast<HANDLE>(stop_event_));
    }
    // Wake blocking ConnectNamedPipe by briefly connecting a dummy handle if needed
    HANDLE dummy = CreateFileA(endpoint_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (dummy != INVALID_HANDLE_VALUE) {
        CloseHandle(dummy);
    }
    if (accept_thread_.joinable()) accept_thread_.join();
    if (stop_event_) {
        CloseHandle(static_cast<HANDLE>(stop_event_));
        stop_event_ = nullptr;
    }
#else
    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (accept_thread_.joinable()) accept_thread_.join();
    ::unlink(endpoint_.c_str());
#endif
}

bool LocalIpcServer::is_running() const noexcept {
    return running_.load();
}

void LocalIpcServer::run_server_loop() {
    auto dispatch_line = [&](const std::string& raw_req) -> std::string {
        IpcResponse resp;
        try {
            const auto j = nlohmann::json::parse(raw_req);
            IpcRequest req;
            req.command = j.value("command", "");
            req.payload_json = j.contains("payload") ? j["payload"].dump() : "{}";
            if (req.command == "tail_events") {
                const auto evs = recent_events(50);
                resp.ok = true;
                resp.body_json = nlohmann::json{{"events", evs}}.dump();
            } else if (handler_) {
                resp = handler_(req);
            } else {
                resp.error = "No handler registered";
            }
        } catch (const std::exception& ex) {
            resp.ok = false;
            resp.error = ex.what();
        }
        nlohmann::json out = {
            {"ok", resp.ok},
            {"error", resp.error},
            {"body", resp.body_json.empty() ? nlohmann::json::object() : nlohmann::json::parse(resp.body_json, nullptr, false)}
        };
        if (out["body"].is_discarded()) {
            out["body"] = resp.body_json;
        }
        return out.dump() + "\n";
    };

#ifdef _WIN32
    while (running_.load()) {
        HANDLE pipe = CreateNamedPipeA(
            endpoint_.c_str(),
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            65536,
            65536,
            0,
            nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        const BOOL connected = ConnectNamedPipe(pipe, nullptr)
                                   ? TRUE
                                   : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (connected && running_.load()) {
            std::string req_buf;
            char chunk[4096];
            DWORD bytes_read = 0;
            while (ReadFile(pipe, chunk, sizeof(chunk), &bytes_read, nullptr) && bytes_read > 0) {
                req_buf.append(chunk, bytes_read);
                if (req_buf.find('\n') != std::string::npos) break;
            }
            if (!req_buf.empty()) {
                const std::string reply = dispatch_line(req_buf);
                DWORD written = 0;
                WriteFile(pipe, reply.data(), static_cast<DWORD>(reply.size()), &written, nullptr);
                FlushFileBuffers(pipe);
            }
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
#else
    while (running_.load() && listen_fd_ >= 0) {
        struct pollfd pfd{listen_fd_, POLLIN, 0};
        const int prc = ::poll(&pfd, 1, 200);
        if (prc <= 0) continue;

        const int client_fd = ::accept(listen_fd_, nullptr, nullptr);
        if (client_fd < 0) continue;

        std::string req_buf;
        char chunk[4096];
        while (true) {
            const ssize_t n = ::read(client_fd, chunk, sizeof(chunk));
            if (n <= 0) break;
            req_buf.append(chunk, static_cast<std::size_t>(n));
            if (req_buf.find('\n') != std::string::npos) break;
        }
        if (!req_buf.empty()) {
            const std::string reply = dispatch_line(req_buf);
            std::size_t sent = 0;
            while (sent < reply.size()) {
                const ssize_t w = ::write(client_fd, reply.data() + sent, reply.size() - sent);
                if (w <= 0) break;
                sent += static_cast<std::size_t>(w);
            }
        }
        ::close(client_fd);
    }
#endif
}

LocalIpcClient::LocalIpcClient(std::string endpoint_path)
    : endpoint_(std::move(endpoint_path)) {}

bool LocalIpcClient::is_daemon_alive() const {
    const auto res = call("ping", "{}", 1000);
    return res.ok;
}

IpcResponse LocalIpcClient::call(const std::string& command,
                                 const std::string& payload_json,
                                 int timeout_ms) const {
    IpcResponse resp;
    nlohmann::json req = {
        {"command", command},
        {"payload", nlohmann::json::parse(payload_json, nullptr, false)}
    };
    if (req["payload"].is_discarded()) {
        req["payload"] = nlohmann::json::object();
    }
    const std::string wire = req.dump() + "\n";

#ifdef _WIN32
    if (!WaitNamedPipeA(endpoint_.c_str(), static_cast<DWORD>(timeout_ms))) {
        resp.error = "Daemon IPC pipe unavailable: " + endpoint_;
        return resp;
    }
    HANDLE pipe = CreateFileA(endpoint_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        resp.error = "Failed to open daemon IPC pipe: " + endpoint_;
        return resp;
    }
    DWORD written = 0;
    WriteFile(pipe, wire.data(), static_cast<DWORD>(wire.size()), &written, nullptr);
    std::string reply;
    char chunk[4096];
    DWORD read_bytes = 0;
    while (ReadFile(pipe, chunk, sizeof(chunk), &read_bytes, nullptr) && read_bytes > 0) {
        reply.append(chunk, read_bytes);
        if (reply.find('\n') != std::string::npos) break;
    }
    CloseHandle(pipe);
#else
    (void)timeout_ms;
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        resp.error = "Cannot create AF_UNIX client socket";
        return resp;
    }
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (endpoint_.size() >= sizeof(addr.sun_path)) {
        ::close(fd);
        resp.error = "Socket path too long";
        return resp;
    }
    std::strncpy(addr.sun_path, endpoint_.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        resp.error = "Daemon IPC socket not reachable: " + endpoint_;
        return resp;
    }
    std::size_t sent = 0;
    while (sent < wire.size()) {
        const ssize_t w = ::write(fd, wire.data() + sent, wire.size() - sent);
        if (w <= 0) break;
        sent += static_cast<std::size_t>(w);
    }
    std::string reply;
    char chunk[4096];
    while (true) {
        const ssize_t n = ::read(fd, chunk, sizeof(chunk));
        if (n <= 0) break;
        reply.append(chunk, static_cast<std::size_t>(n));
        if (reply.find('\n') != std::string::npos) break;
    }
    ::close(fd);
#endif

    if (reply.empty()) {
        resp.error = "Empty response from daemon";
        return resp;
    }
    try {
        const auto j = nlohmann::json::parse(reply);
        resp.ok = j.value("ok", false);
        resp.error = j.value("error", "");
        if (j.contains("body")) {
            resp.body_json = j["body"].is_string() ? j["body"].get<std::string>() : j["body"].dump();
        }
    } catch (const std::exception& ex) {
        resp.error = std::string("Malformed IPC reply: ") + ex.what();
    }
    return resp;
}

} // namespace agmesh::ipc
