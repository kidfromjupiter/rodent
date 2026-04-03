#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <sys/types.h>
#include <string>
#include <thread>
#include <vector>

namespace rodent::clipboard {

enum class ClipboardState : uint8_t {
    Data,
    Nil,
    Clear,
    Sensitive,
    Unknown,
};

struct ClipboardEvent {
    ClipboardState state = ClipboardState::Unknown;
    std::string raw_state;
};

struct WatchConfig {
    std::optional<std::string> seat;
    bool primary = false;
};

class WaylandClipboardWatcher final {
public:
    explicit WaylandClipboardWatcher(WatchConfig config);
    ~WaylandClipboardWatcher();

    WaylandClipboardWatcher(const WaylandClipboardWatcher&) = delete;
    WaylandClipboardWatcher& operator=(const WaylandClipboardWatcher&) = delete;

    bool Start();
    void Stop();

    [[nodiscard]] bool PollEvent(ClipboardEvent& event);
    [[nodiscard]] std::optional<std::vector<uint8_t>> ReadClipboardBytes() const;

private:
    static ClipboardState ParseState(const std::string& state);
    void WatchLoop();

    WatchConfig config_;
    std::atomic<bool> running_ = false;
    std::thread watch_thread_;
    mutable std::mutex queue_mutex_;
    std::deque<ClipboardEvent> queue_;
    mutable std::mutex process_mutex_;
    pid_t active_pid_ = -1;
    int active_read_fd_ = -1;
};

}  // namespace rodent::clipboard
