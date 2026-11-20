#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace agmesh::adapter {

enum class FileEventKind {
    Created,
    Modified,
    Removed
};

struct FileWatchEvent {
    std::filesystem::path path;
    FileEventKind kind{FileEventKind::Modified};
    std::int64_t timestamp_ms{0};
};

// Cross-platform asynchronous recursive directory watcher with event debouncing:
// Linux: inotify, Windows: ReadDirectoryChangesW, macOS: FSEvents/kqueue fallback.
class FileWatcher {
public:
    using Callback = std::function<void(const std::vector<FileWatchEvent>&)>;

    explicit FileWatcher(std::uint32_t debounce_ms = 350);
    ~FileWatcher();

    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;

    void add_watch_root(const std::filesystem::path& root);
    bool start(Callback on_events, std::string& error_out);
    void stop();
    [[nodiscard]] bool is_running() const noexcept;

private:
    void run_platform_loop();
    void enqueue_raw_event(const std::filesystem::path& path, FileEventKind kind);
    void flush_debounced_events();

    std::uint32_t debounce_ms_;
    mutable std::mutex mutex_;
    std::vector<std::filesystem::path> watch_roots_;
    Callback callback_;
    std::atomic<bool> running_{false};
    std::thread watch_thread_;
    std::thread debounce_thread_;

    struct PendingEvent {
        FileEventKind kind;
        std::chrono::steady_clock::time_point last_seen;
    };
    std::unordered_map<std::string, PendingEvent> pending_events_;
};

} // namespace agmesh::adapter
