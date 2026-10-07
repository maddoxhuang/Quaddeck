#pragma once

// Writes the stacks of every thread to the log when the window thread stops.
//
// Windows records a frozen player only as "QuadDeck.exe stopped interacting
// with Windows" and keeps no dump of it, and QuadDeck.log says what was done
// before the freeze, not where the window thread is waiting. A second thread
// watches a heartbeat the window thread gives once per frame; when it stays
// silent past a threshold, every thread is suspended in turn, its stack is
// unwound, and the stacks are written with function names and source lines
// from QuadDeck.pdb beside the executable.

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace quaddeck {

// When to capture, free of threads and clocks so it can be tested on its own.
// Times are milliseconds on any monotonic clock.
class StallTracker {
public:
    struct Timing {
        std::uint64_t firstCaptureMs{4000};
        // A second look tells a thread that is stuck from one that is slow.
        std::uint64_t secondCaptureMs{10000};
        // The watchdog polls several times a second. A longer gap between two
        // polls means the whole machine was asleep or the watchdog itself was
        // starved, and the silence it saw says nothing about the window thread.
        std::uint64_t pollGapMs{3000};
    };

    struct Action {
        bool capture{};
        int captureNumber{};        // 1 or 2 when capture is set
        std::uint64_t stalledMs{};  // how long the window thread has been silent
        bool recovered{};
        std::uint64_t recoveredAfterMs{};  // when recovered: the length of the stall
    };

    StallTracker() = default;
    explicit StallTracker(Timing timing) : timing_(timing) {}

    Action poll(std::uint64_t now, std::uint64_t lastBeat) {
        Action action;
        if (polled_ && now - lastPoll_ > timing_.pollGapMs) baseline_ = now;
        polled_ = true;
        lastPoll_ = now;
        if (captures_ > 0 && lastBeat > stallStart_) {
            action.recovered = true;
            action.recoveredAfterMs = lastBeat - stallStart_;
            captures_ = 0;
        }
        const std::uint64_t since = lastBeat > baseline_ ? lastBeat : baseline_;
        const std::uint64_t stalled = now > since ? now - since : 0;
        const std::uint64_t due = captures_ == 0 ? timing_.firstCaptureMs
                                : captures_ == 1 ? timing_.secondCaptureMs : 0;
        if (due > 0 && stalled >= due) {
            if (captures_ == 0) stallStart_ = since;
            action.capture = true;
            action.captureNumber = ++captures_;
            action.stalledMs = stalled;
        }
        return action;
    }

private:
    Timing timing_{};
    bool polled_{};
    std::uint64_t lastPoll_{};
    std::uint64_t baseline_{};
    std::uint64_t stallStart_{};
    int captures_{};
};

// The stacks of every thread of this process but the caller, the watched
// thread first, as text for the log. Threads with the same stack are folded
// into the first one.
std::string describeAllThreadStacks(DWORD watchedThread);

class HangWatchdog {
public:
    using Sink = std::function<void(const std::string&)>;

    HangWatchdog() = default;
    HangWatchdog(const HangWatchdog&) = delete;
    HangWatchdog& operator=(const HangWatchdog&) = delete;
    ~HangWatchdog() { stop(); }

    // Watches the calling thread when watchedThread is 0. The sink runs on
    // the watchdog's own thread.
    void start(DWORD watchedThread, Sink sink, StallTracker::Timing timing = {},
               std::chrono::milliseconds pollInterval = std::chrono::milliseconds(250));
    void beat() noexcept { lastBeat_.store(nowMs(), std::memory_order_relaxed); }
    void stop();

    static std::uint64_t nowMs() noexcept {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }

private:
    void run(DWORD watchedThread, Sink sink, StallTracker tracker, std::chrono::milliseconds pollInterval);

    std::atomic<std::uint64_t> lastBeat_{};
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_{};
    std::thread worker_;
};

}  // namespace quaddeck
