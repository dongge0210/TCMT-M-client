#pragma once

// std::jthread / std::stop_token compatibility for Apple Clang (Xcode < 16).
// Use tcmt::compat::StopToken / tcmt::compat::JThread everywhere instead of std::.

#if defined(__APPLE__) && defined(__clang__) && !defined(TCMT_WINDOWS)

#include <thread>
#include <atomic>
#include <chrono>
#include <algorithm>

namespace tcmt::compat {

class StopSource {
public:
    void request_stop() { stopped_.store(true, std::memory_order_release); }
    [[nodiscard]] bool stop_requested() const { return stopped_.load(std::memory_order_acquire); }
private:
    std::atomic<bool> stopped_{false};
};

class StopToken {
public:
    StopToken() : src_(nullptr) {}
    StopToken(const StopSource* src) : src_(src) {}
    [[nodiscard]] bool stop_requested() const { return src_ && src_->stop_requested(); }
    StopToken(const StopToken&) = default;
    StopToken& operator=(const StopToken&) = default;
    StopToken(StopToken&&) noexcept = default;
    StopToken& operator=(StopToken&&) noexcept = default;
private:
    const StopSource* src_;
    friend class JThread;
};

class JThread {
public:
    JThread() = default;

    template<typename Callable, typename... Args>
    JThread(Callable&& f, Args&&... args) {
        thread_ = std::thread([this,
                               f = std::forward<Callable>(f),
                               ...args = std::forward<Args>(args)]() mutable {
            f(StopToken(&src_), std::move(args)...);
        });
    }

    ~JThread() {
        if (joinable()) {
            request_stop();
            join();
        }
    }

    JThread(JThread&& other) noexcept : thread_(std::move(other.thread_)) {
        if (other.src_.stop_requested()) src_.request_stop();
    }

    JThread& operator=(JThread&& other) noexcept {
        if (joinable()) {
            request_stop();
            join();
        }
        if (other.src_.stop_requested()) src_.request_stop();
        thread_ = std::move(other.thread_);
        return *this;
    }

    JThread(const JThread&) = delete;
    JThread& operator=(const JThread&) = delete;

    void request_stop() { src_.request_stop(); }
    [[nodiscard]] bool joinable() const { return thread_.joinable(); }
    void join() { thread_.join(); }

private:
    StopSource src_;
    std::thread thread_;
};

} // namespace tcmt::compat

#else
// Native C++20 jthread available (Windows MSVC / newer Clang)

#include <thread>
#include <chrono>

namespace tcmt::compat {
    using StopToken = std::stop_token;
    using JThread   = std::jthread;
} // namespace tcmt::compat

#endif
