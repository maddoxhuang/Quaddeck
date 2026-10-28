#pragma once

// The sound outputs Windows has, which one it plays media on, and word when
// either changes.

#include <atomic>
#include <string>
#include <vector>

struct IMMDeviceEnumerator;

namespace quaddeck {

struct AudioDeviceInfo {
    std::wstring id;
    std::wstring name;
};

class AudioDevices {
public:
    AudioDevices() = default;
    ~AudioDevices();
    AudioDevices(const AudioDevices&) = delete;
    AudioDevices& operator=(const AudioDevices&) = delete;

    // Opens Windows' device list and starts listening to it. False when it
    // cannot; the lists are then empty and nothing ever changes.
    bool start();
    // Stops listening and lets the list go, before COM is shut down on the
    // thread that started it.
    void stop();
    // Every output that is connected and enabled, by name.
    std::vector<AudioDeviceInfo> outputs() const;
    // The output Windows plays media on, empty when there is none.
    std::wstring defaultOutput() const;
    // True once after an output came or went, or another became the
    // default. Set on Windows' own thread; read on the window's.
    bool takeChanged() { return changed_.exchange(false); }

private:
    class Listener;
    IMMDeviceEnumerator* enumerator_{};
    Listener* listener_{};
    std::atomic<bool> changed_{false};
};

}  // namespace quaddeck
