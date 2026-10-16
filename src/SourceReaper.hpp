#pragma once

// Sources let go of off the window thread. Closing a source joins its
// decoder threads, and some of what they may be inside cannot be called
// off: a read FFmpeg's file protocol has started on a share that stopped
// answering (the NAS cache off, or a file another program is writing),
// the read-ahead cache's wait for a cancelled read to complete, a host name
// being looked up. Joined on the window thread, that wait froze the window
// and every other pane until the redirector gave up, tens of seconds.
//
// retire() tells the source to stop at once and closes it on a thread of its
// own, so five panes closing also no longer wait for each other. drain()
// waits for all of them; device recovery calls it before the device the
// decoders share is let go, and shutdown before the player's audio and
// renderer go, as both always waited.

#include "VideoSource.hpp"

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>

namespace quaddeck {

class SourceReaper {
public:
    SourceReaper() = default;
    SourceReaper(const SourceReaper&) = delete;
    SourceReaper& operator=(const SourceReaper&) = delete;
    ~SourceReaper() { drain(); }

    // With kMaxClosing already closing -- a stuck share under each -- this
    // waits for one to finish, so stuck sources and the decoder memory they
    // hold cannot pile up without end.
    void retire(std::unique_ptr<VideoSource> source) {
        if (!source) return;
        source->beginClose();
        {
            std::unique_lock lock(state_->mutex);
            state_->idle.wait(lock, [&] { return state_->closing < kMaxClosing; });
            ++state_->closing;
        }
        try {
            std::thread([state = state_, source = std::move(source)]() mutable {
                source.reset();
                {
                    std::scoped_lock lock(state->mutex);
                    --state->closing;
                }
                state->idle.notify_all();
            }).detach();
        } catch (const std::system_error&) {
            // No thread to be had: the source was closed here, as it always was.
            std::scoped_lock lock(state_->mutex);
            --state_->closing;
        }
    }

    void drain() {
        std::unique_lock lock(state_->mutex);
        state_->idle.wait(lock, [&] { return state_->closing == 0; });
    }

    std::size_t closing() const {
        std::scoped_lock lock(state_->mutex);
        return state_->closing;
    }

    static constexpr std::size_t kMaxClosing = 4;

private:
    // Shared with each closing thread until all source shutdowns finish.
    struct State {
        mutable std::mutex mutex;
        std::condition_variable idle;
        std::size_t closing{};
    };
    std::shared_ptr<State> state_{std::make_shared<State>()};
};

}  // namespace quaddeck
