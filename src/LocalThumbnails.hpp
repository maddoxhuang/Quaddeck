#pragma once

// Pictures of local video files, from the Windows shell -- the same
// thumbnails Explorer shows and caches -- fetched on a worker thread because
// a provider may have to open the file, and a file may be on a share. The
// answers are delivered on the thread that calls pump(), like EmbyClient's.

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace quaddeck {

class LocalThumbnailer {
public:
    struct Result {
        bool cancelled{};
        // An encoded image (BMP) WIC can decode; empty when the shell has none.
        std::string bytes;
    };
    using Handler = std::function<void(const Result&)>;

    LocalThumbnailer();
    ~LocalThumbnailer();
    LocalThumbnailer(const LocalThumbnailer&) = delete;
    LocalThumbnailer& operator=(const LocalThumbnailer&) = delete;

    // A picture at least `width` x `height` pixels where the shell has one.
    void request(std::wstring path, int width, int height, Handler handler);
    // Runs the handlers of finished requests. Call from one thread only.
    void pump();
    // Drops the requests not yet started; their handlers run from the next
    // pump() with `cancelled` set.
    void clearPending();
    std::size_t pendingCount() const;

    // The shell's thumbnail of a file as BMP bytes, or nothing. Blocking;
    // COM must be initialised on the calling thread.
    static std::string shellThumbnail(const std::wstring& path, int width, int height);

private:
    struct Job {
        std::wstring path;
        int width{};
        int height{};
        Handler handler;
    };
    void workerMain();

    std::thread worker_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> pending_;
    std::deque<std::pair<Handler, Result>> completed_;
    bool stop_{};
    bool busy_{};
};

}  // namespace quaddeck
