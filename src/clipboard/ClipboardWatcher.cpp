#include "ClipboardWatcher.h"

#include <array>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace rodent::clipboard {

namespace {

std::optional<std::vector<uint8_t>> RunAndCaptureStdout(const std::vector<std::string>& args)
{
    if (args.empty()) {
        return std::nullopt;
    }

    int pipe_fds[2] = {-1, -1};
    if (::pipe(pipe_fds) != 0) {
        return std::nullopt;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        return std::nullopt;
    }

    if (pid == 0) {
        ::close(pipe_fds[0]);
        if (::dup2(pipe_fds[1], STDOUT_FILENO) < 0) {
            _exit(127);
        }
        ::close(pipe_fds[1]);

        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        ::execvp(argv[0], argv.data());
        _exit(127);
    }

    ::close(pipe_fds[1]);

    std::vector<uint8_t> output;
    std::array<uint8_t, 4096> buf {};
    while (true) {
        const ssize_t n = ::read(pipe_fds[0], buf.data(), buf.size());
        if (n == 0) {
            break;
        }
        if (n < 0) {
            ::close(pipe_fds[0]);
            int status = 0;
            (void)::waitpid(pid, &status, 0);
            return std::nullopt;
        }
        output.insert(output.end(), buf.begin(), buf.begin() + n);
    }
    ::close(pipe_fds[0]);

    int status = 0;
    if (::waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return std::nullopt;
    }

    return output;
}

}  // namespace

WaylandClipboardWatcher::WaylandClipboardWatcher(WatchConfig config)
    : config_(std::move(config))
{
}

WaylandClipboardWatcher::~WaylandClipboardWatcher()
{
    Stop();
}

bool WaylandClipboardWatcher::Start()
{
    if (running_.exchange(true)) {
        return true;
    }
    watch_thread_ = std::thread(&WaylandClipboardWatcher::WatchLoop, this);
    return true;
}

void WaylandClipboardWatcher::Stop()
{
    if (!running_.exchange(false)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(process_mutex_);
        if (active_pid_ > 0) {
            ::kill(active_pid_, SIGTERM);
        }
    }
    if (watch_thread_.joinable()) {
        watch_thread_.join();
    }
}

bool WaylandClipboardWatcher::PollEvent(ClipboardEvent& event)
{
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (queue_.empty()) {
        return false;
    }
    event = std::move(queue_.front());
    queue_.pop_front();
    return true;
}

std::optional<std::vector<uint8_t>> WaylandClipboardWatcher::ReadClipboardBytes() const
{
    std::vector<std::string> args = {"wl-paste", "--no-newline"};
    if (config_.primary) {
        args.emplace_back("--primary");
    }
    if (config_.seat.has_value() && !config_.seat->empty()) {
        args.emplace_back("--seat");
        args.push_back(*config_.seat);
    }
    return RunAndCaptureStdout(args);
}

ClipboardState WaylandClipboardWatcher::ParseState(const std::string& state)
{
    if (state == "data") {
        return ClipboardState::Data;
    }
    if (state == "nil") {
        return ClipboardState::Nil;
    }
    if (state == "clear") {
        return ClipboardState::Clear;
    }
    if (state == "sensitive") {
        return ClipboardState::Sensitive;
    }
    return ClipboardState::Unknown;
}

void WaylandClipboardWatcher::WatchLoop()
{
    while (running_.load()) {
        int pipe_fds[2] = {-1, -1};
        if (::pipe(pipe_fds) != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        const pid_t pid = ::fork();
        if (pid < 0) {
            ::close(pipe_fds[0]);
            ::close(pipe_fds[1]);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        if (pid == 0) {
            ::close(pipe_fds[0]);
            if (::dup2(pipe_fds[1], STDOUT_FILENO) < 0) {
                _exit(127);
            }
            ::close(pipe_fds[1]);

            std::vector<std::string> args = {
                "wl-paste",
            };
            if (config_.primary) {
                args.emplace_back("--primary");
            }
            if (config_.seat.has_value() && !config_.seat->empty()) {
                args.emplace_back("--seat");
                args.push_back(*config_.seat);
            }
            args.emplace_back("--watch");
            args.emplace_back("sh");
            args.emplace_back("-c");
            args.emplace_back("printf '%s\\n' \"${CLIPBOARD_STATE:-}\"");

            std::vector<char*> argv;
            argv.reserve(args.size() + 1);
            for (const auto& arg : args) {
                argv.push_back(const_cast<char*>(arg.c_str()));
            }
            argv.push_back(nullptr);

            ::execvp("wl-paste", argv.data());
            _exit(127);
        }

        ::close(pipe_fds[1]);
        {
            std::lock_guard<std::mutex> lock(process_mutex_);
            active_pid_ = pid;
            active_read_fd_ = pipe_fds[0];
        }
        FILE* stream = ::fdopen(pipe_fds[0], "r");
        if (stream == nullptr) {
            ::close(pipe_fds[0]);
            ::kill(pid, SIGTERM);
            int status = 0;
            (void)::waitpid(pid, &status, 0);
            {
                std::lock_guard<std::mutex> lock(process_mutex_);
                active_pid_ = -1;
                active_read_fd_ = -1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        char* line = nullptr;
        size_t cap = 0;
        while (running_.load()) {
            const ssize_t read_len = ::getline(&line, &cap, stream);
            if (read_len < 0) {
                break;
            }

            std::string raw_state(line, static_cast<size_t>(read_len));
            while (!raw_state.empty() && (raw_state.back() == '\n' || raw_state.back() == '\r')) {
                raw_state.pop_back();
            }

            ClipboardEvent event;
            event.state = ParseState(raw_state);
            event.raw_state = std::move(raw_state);
            std::lock_guard<std::mutex> lock(queue_mutex_);
            queue_.push_back(std::move(event));
        }

        if (line != nullptr) {
            std::free(line);
        }
        ::fclose(stream);
        {
            std::lock_guard<std::mutex> lock(process_mutex_);
            active_pid_ = -1;
            active_read_fd_ = -1;
        }

        ::kill(pid, SIGTERM);
        int status = 0;
        (void)::waitpid(pid, &status, 0);

        if (running_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }
}

}  // namespace rodent::clipboard
