#include "agmesh/adapter/file_watcher.hpp"
#include "agmesh/core/types.hpp"

#include <algorithm>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#elif defined(__linux__)
#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>
#endif

namespace agmesh::adapter {

FileWatcher::FileWatcher(std::uint32_t debounce_ms)
    : debounce_ms_(debounce_ms) {}

FileWatcher::~FileWatcher() {
    stop();
}

void FileWatcher::add_watch_root(const std::filesystem::path& root) {
    std::lock_guard<std::mutex> lock(mutex_);
    watch_roots_.push_back(root);
}

bool FileWatcher::start(Callback on_events, std::string& error_out) {
    if (running_.load()) {
        return true;
    }
    if (!on_events) {
        error_out = "FileWatcher callback cannot be empty";
        return false;
    }
    callback_ = std::move(on_events);
    running_.store(true);

    debounce_thread_ = std::thread([this]() {
        while (running_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            flush_debounced_events();
        }
    });

    watch_thread_ = std::thread([this]() { run_platform_loop(); });
    return true;
}

void FileWatcher::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (watch_thread_.joinable()) {
        watch_thread_.join();
    }
    if (debounce_thread_.joinable()) {
        debounce_thread_.join();
    }
}

bool FileWatcher::is_running() const noexcept {
    return running_.load();
}

void FileWatcher::enqueue_raw_event(const std::filesystem::path& path, FileEventKind kind) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_events_[path.string()] = PendingEvent{kind, std::chrono::steady_clock::now()};
}

void FileWatcher::flush_debounced_events() {
    std::vector<FileWatchEvent> ready;
    const auto now_tp = std::chrono::steady_clock::now();
    const auto threshold = std::chrono::milliseconds(debounce_ms_);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = pending_events_.begin(); it != pending_events_.end();) {
            if (now_tp - it->second.last_seen >= threshold) {
                ready.push_back(FileWatchEvent{
                    std::filesystem::path(it->first),
                    it->second.kind,
                    core::now_ms()
                });
                it = pending_events_.erase(it);
            } else {
                ++it;
            }
        }
    }

    if (!ready.empty() && callback_) {
        callback_(ready);
    }
}

void FileWatcher::run_platform_loop() {
    std::vector<std::filesystem::path> roots;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        roots = watch_roots_;
    }

#ifdef _WIN32
    struct WinWatch {
        std::filesystem::path root;
        HANDLE dir_handle{INVALID_HANDLE_VALUE};
        OVERLAPPED ov{};
        std::vector<std::uint8_t> buffer;
        bool pending{false};
    };

    std::vector<std::unique_ptr<WinWatch>> watches;
    for (const auto& root : roots) {
        std::error_code ec;
        if (!std::filesystem::exists(root, ec)) continue;
        HANDLE h = CreateFileW(
            root.wstring().c_str(),
            FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
            nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        auto w = std::make_unique<WinWatch>();
        w->root = root;
        w->dir_handle = h;
        w->ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        w->buffer.resize(64 * 1024);
        watches.push_back(std::move(w));
    }

    auto issue_read = [](WinWatch& w) {
        ResetEvent(w.ov.hEvent);
        DWORD bytes_ret = 0;
        const BOOL ok = ReadDirectoryChangesW(
            w.dir_handle,
            w.buffer.data(),
            static_cast<DWORD>(w.buffer.size()),
            TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE,
            &bytes_ret,
            &w.ov,
            nullptr);
        w.pending = (ok != 0 || GetLastError() == ERROR_IO_PENDING);
    };

    for (auto& w : watches) {
        issue_read(*w);
    }

    while (running_.load()) {
        if (watches.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }
        std::vector<HANDLE> handles;
        handles.reserve(watches.size());
        for (const auto& w : watches) handles.push_back(w->ov.hEvent);

        const DWORD wait_rc = WaitForMultipleObjects(
            static_cast<DWORD>(handles.size()), handles.data(), FALSE, 200);
        if (wait_rc >= WAIT_OBJECT_0 && wait_rc < WAIT_OBJECT_0 + handles.size()) {
            const std::size_t idx = wait_rc - WAIT_OBJECT_0;
            auto& w = *watches[idx];
            DWORD bytes_transferred = 0;
            if (GetOverlappedResult(w.dir_handle, &w.ov, &bytes_transferred, FALSE) && bytes_transferred > 0) {
                std::size_t offset = 0;
                while (offset < bytes_transferred) {
                    const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(w.buffer.data() + offset);
                    std::wstring rel_w(info->FileName, info->FileNameLength / sizeof(WCHAR));
                    FileEventKind kind = FileEventKind::Modified;
                    if (info->Action == FILE_ACTION_ADDED || info->Action == FILE_ACTION_RENAMED_NEW_NAME) {
                        kind = FileEventKind::Created;
                    } else if (info->Action == FILE_ACTION_REMOVED || info->Action == FILE_ACTION_RENAMED_OLD_NAME) {
                        kind = FileEventKind::Removed;
                    }
                    enqueue_raw_event(w.root / std::filesystem::path(rel_w), kind);
                    if (info->NextEntryOffset == 0) break;
                    offset += info->NextEntryOffset;
                }
            }
            issue_read(w);
        }
    }

    for (auto& w : watches) {
        CancelIo(w->dir_handle);
        if (w->ov.hEvent) CloseHandle(w->ov.hEvent);
        if (w->dir_handle != INVALID_HANDLE_VALUE) CloseHandle(w->dir_handle);
    }

#elif defined(__linux__)
    int inotify_fd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotify_fd < 0) {
        return;
    }

    std::unordered_map<int, std::filesystem::path> wd_to_path;
    auto add_dir_recursive = [&](const std::filesystem::path& base, auto& self_ref) -> void {
        std::error_code ec;
        if (!std::filesystem::exists(base, ec) || !std::filesystem::is_directory(base, ec)) return;
        const int wd = ::inotify_add_watch(
            inotify_fd, base.c_str(), IN_CREATE | IN_MODIFY | IN_CLOSE_WRITE | IN_DELETE | IN_MOVED_TO);
        if (wd >= 0) {
            wd_to_path[wd] = base;
        }
        for (const auto& entry : std::filesystem::directory_iterator(base, ec)) {
            if (entry.is_directory(ec)) {
                self_ref(entry.path(), self_ref);
            }
        }
    };

    for (const auto& r : roots) {
        add_dir_recursive(r, add_dir_recursive);
    }

    std::vector<char> buf(64 * 1024);
    while (running_.load()) {
        struct pollfd pfd{inotify_fd, POLLIN, 0};
        const int prc = ::poll(&pfd, 1, 200);
        if (prc <= 0) continue;

        const ssize_t len = ::read(inotify_fd, buf.data(), buf.size());
        if (len <= 0) continue;

        ssize_t offset = 0;
        while (offset < len) {
            const auto* ev = reinterpret_cast<const struct inotify_event*>(buf.data() + offset);
            auto it = wd_to_path.find(ev->wd);
            if (it != wd_to_path.end() && ev->len > 0) {
                const std::filesystem::path full = it->second / ev->name;
                FileEventKind kind = FileEventKind::Modified;
                if (ev->mask & (IN_CREATE | IN_MOVED_TO)) kind = FileEventKind::Created;
                else if (ev->mask & IN_DELETE) kind = FileEventKind::Removed;
                enqueue_raw_event(full, kind);
                if (ev->mask & IN_ISDIR && (ev->mask & IN_CREATE)) {
                    add_dir_recursive(full, add_dir_recursive);
                }
            }
            offset += static_cast<ssize_t>(sizeof(struct inotify_event) + ev->len);
        }
    }
    ::close(inotify_fd);

#elif defined(__APPLE__)
    struct FsCtx {
        FileWatcher* self;
    };
    FsCtx ctx{this};

    auto callback_fn = [](ConstFSEventStreamRef,
                          void* client_info,
                          std::size_t num_events,
                          void* event_paths,
                          const FSEventStreamEventFlags event_flags[],
                          const FSEventStreamEventId[]) {
        auto* c = static_cast<FsCtx*>(client_info);
        char** paths = static_cast<char**>(event_paths);
        for (std::size_t i = 0; i < num_events; ++i) {
            FileEventKind kind = FileEventKind::Modified;
            if (event_flags[i] & kFSEventStreamEventFlagItemCreated) kind = FileEventKind::Created;
            else if (event_flags[i] & kFSEventStreamEventFlagItemRemoved) kind = FileEventKind::Removed;
            c->self->enqueue_raw_event(std::filesystem::path(paths[i]), kind);
        }
    };

    CFMutableArrayRef cf_paths = CFArrayCreateMutable(nullptr, 0, &kCFTypeArrayCallBacks);
    for (const auto& r : roots) {
        std::error_code ec;
        if (!std::filesystem::exists(r, ec)) continue;
        CFStringRef cf_str = CFStringCreateWithCString(nullptr, r.c_str(), kCFStringEncodingUTF8);
        if (cf_str) {
            CFArrayAppendValue(cf_paths, cf_str);
            CFRelease(cf_str);
        }
    }

    if (CFArrayGetCount(cf_paths) > 0) {
        FSEventStreamContext fs_ctx{0, &ctx, nullptr, nullptr, nullptr};
        FSEventStreamRef stream = FSEventStreamCreate(
            nullptr,
            callback_fn,
            &fs_ctx,
            cf_paths,
            kFSEventStreamEventIdSinceNow,
            0.2,
            kFSEventStreamCreateFlagFileEvents);
        if (stream) {
            dispatch_queue_t q = dispatch_queue_create("io.antigravity.agmesh.fsevents", DISPATCH_QUEUE_SERIAL);
            FSEventStreamSetDispatchQueue(stream, q);
            FSEventStreamStart(stream);
            while (running_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
            }
            FSEventStreamStop(stream);
            FSEventStreamInvalidate(stream);
            FSEventStreamRelease(stream);
            dispatch_release(q);
        }
    } else {
        while (running_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }
    CFRelease(cf_paths);
#endif
}

} // namespace agmesh::adapter
