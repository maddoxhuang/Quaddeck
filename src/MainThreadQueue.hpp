#pragma once

// Work handed to the window thread from anywhere else. A background job
// posts a closure; App drains the queue once per frame from tick(). The
// queue is shared by pointer so a job that outlives App -- a directory
// listing stuck on a share that went away -- posts into a queue nobody
// drains rather than into freed memory.

#include <deque>
#include <functional>
#include <mutex>
#include <utility>

namespace quaddeck {

class MainThreadQueue {
public:
    void post(std::function<void()> task) {
        std::scoped_lock lock(mutex_);
        tasks_.push_back(std::move(task));
    }
    void drain() {
        std::deque<std::function<void()>> ready;
        {
            std::scoped_lock lock(mutex_);
            ready.swap(tasks_);
        }
        for (auto& task : ready) task();
    }

private:
    std::mutex mutex_;
    std::deque<std::function<void()>> tasks_;
};

}  // namespace quaddeck
