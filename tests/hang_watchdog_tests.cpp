#include "HangWatchdog.hpp"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace quaddeck;
using namespace std::chrono_literals;

namespace {

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void firstCaptureAfterTheThreshold() {
    StallTracker tracker({4000, 10000, 3000});
    require(!tracker.poll(1000, 1000).capture, "a fresh beat is no stall");
    for (std::uint64_t now = 1250; now < 5000; now += 250) {
        require(!tracker.poll(now, 1000).capture, "no capture before four silent seconds");
    }
    const auto first = tracker.poll(5000, 1000);
    require(first.capture && first.captureNumber == 1 && first.stalledMs == 4000, "first capture at four seconds");
    for (std::uint64_t now = 5250; now < 11000; now += 250) {
        require(!tracker.poll(now, 1000).capture, "one capture per threshold");
    }
    const auto second = tracker.poll(11000, 1000);
    require(second.capture && second.captureNumber == 2, "second capture at ten seconds");
    for (std::uint64_t now = 11250; now < 30000; now += 250) {
        require(!tracker.poll(now, 1000).capture, "no third capture in one stall");
    }
    const auto back = tracker.poll(30250, 30200);
    require(back.recovered && back.recoveredAfterMs == 29200 && !back.capture, "the end of the stall is reported");
    require(!tracker.poll(30500, 30450).recovered, "recovery is reported once");
}

void recoveryWithoutCaptureIsSilent() {
    StallTracker tracker({4000, 10000, 3000});
    for (std::uint64_t now = 0; now <= 3750; now += 250) tracker.poll(now, 0);
    const auto action = tracker.poll(4000, 3900);
    require(!action.capture && !action.recovered, "a short pause is neither a stall nor a recovery");
}

void sleepIsNotAStall() {
    // The machine slept for an hour: the first poll after waking sees an old
    // beat, but also its own long gap.
    StallTracker tracker({4000, 10000, 3000});
    tracker.poll(1000, 1000);
    require(!tracker.poll(3601000, 1000).capture, "waking from sleep is no stall");
    for (std::uint64_t now = 3601250; now < 3605000; now += 250) {
        require(!tracker.poll(now, 1000).capture, "the window thread gets its own threshold after waking");
    }
    require(tracker.poll(3605000, 1000).capture, "a thread still silent four seconds after waking is a stall");
}

volatile long gWaitsReturned = 0;

// A frame the report must name: it needs the test's PDB. The work after the
// wait keeps the call from becoming a jump that leaves no frame.
__declspec(noinline) void HangWatchdogTestStallsHere(HANDLE release) {
    if (WaitForSingleObject(release, INFINITE) == WAIT_OBJECT_0) gWaitsReturned = gWaitsReturned + 1;
}

void liveCaptureNamesTheWaitingFunction() {
    std::mutex mutex;
    std::vector<std::string> reports;
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    require(release != nullptr, "event");
    HangWatchdog watchdog;
    std::atomic<DWORD> watchedId{};
    std::atomic<bool> started{};
    std::atomic<bool> done{};
    std::thread watched([&] {
        watchedId = GetCurrentThreadId();
        watchdog.start(0, [&](const std::string& report) {
            std::scoped_lock lock(mutex);
            reports.push_back(report);
        }, {300, 100000, 3000}, 50ms);
        started = true;
        for (int i = 0; i < 10; ++i) {
            watchdog.beat();
            std::this_thread::sleep_for(10ms);
        }
        HangWatchdogTestStallsHere(release);
        while (!done) {
            watchdog.beat();
            std::this_thread::sleep_for(10ms);
        }
    });
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    auto count = [&] { std::scoped_lock lock(mutex); return reports.size(); };
    while (count() < 1) {
        require(std::chrono::steady_clock::now() < deadline, "no report for a stalled thread");
        std::this_thread::sleep_for(20ms);
    }
    SetEvent(release);
    while (count() < 2) {
        require(std::chrono::steady_clock::now() < deadline, "no report once the thread ran again");
        std::this_thread::sleep_for(20ms);
    }
    done = true;
    watched.join();
    watchdog.stop();
    CloseHandle(release);

    std::scoped_lock lock(mutex);
    const auto& stall = reports[0];
    std::cout << stall.substr(0, 1500) << "\n...\n" << reports[1] << '\n';
    require(stall.rfind("HANG: the window thread has not run for ", 0) == 0, "stall report heading");
    const auto watchedHeading = "\nthread " + std::to_string(watchedId.load()) + " (window thread)";
    require(stall.find(watchedHeading) != std::string::npos, "the watched thread is named");
    require(stall.find("\nthread ") == stall.find(watchedHeading), "the watched thread comes first");
    const auto watchedStack = stall.substr(stall.find(watchedHeading), 4000);
    require(watchedStack.find("HangWatchdogTestStallsHere") != std::string::npos,
            "the stack names the function the thread waits in");
    require(watchedStack.find("hang_watchdog_tests.cpp:") != std::string::npos, "the stack gives a source line");
    require(stall.find("hang watchdog") == std::string::npos, "the watchdog leaves itself out");
    require(reports[1].rfind("Window thread ran again after ", 0) == 0, "recovery report");
}

}  // namespace

int main() {
    try {
        firstCaptureAfterTheThreshold();
        recoveryWithoutCaptureIsSilent();
        sleepIsNotAStall();
        liveCaptureNamesTheWaitingFunction();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "QuadDeckHangWatchdogTests passed\n";
    return 0;
}
