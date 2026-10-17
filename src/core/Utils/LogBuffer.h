#pragma once

#include <string>
#include <vector>
#include <deque>
#include <mutex>

namespace tcmt {

// Log ring buffer for TUI log panel
class LogBuffer {
public:
    static constexpr size_t MAX_LINES = 2000;

    void Push(const std::string& line) {
        std::lock_guard<std::mutex> lock(mutex_);
        lines_.push_back(line);
        ++version_;
        while (lines_.size() > MAX_LINES) {
            lines_.pop_front();
        }
    }

    std::vector<std::string> GetRecent(size_t count) const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> result;
        size_t start = (lines_.size() > count) ? lines_.size() - count : 0;
        for (size_t i = start; i < lines_.size(); ++i) {
            result.push_back(lines_[i]);
        }
        return result;
    }

    size_t Size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lines_.size();
    }

    // Monotonic write counter — lets consumers detect content changes even
    // when the ring buffer is full and Size() stops growing.
    size_t Version() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return version_;
    }

private:
    mutable std::mutex mutex_;
    std::deque<std::string> lines_;
    size_t version_ = 0;
};

} // namespace tcmt
