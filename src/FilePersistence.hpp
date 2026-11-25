#pragma once

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace quaddeck {

// Writes through a sibling temporary file and only replaces the destination
// after the complete document has been serialized successfully. A failed
// writer or disk write therefore leaves an existing session/settings file
// untouched instead of truncating it to a partial document.
bool writeUtf8FileAtomically(
    const std::filesystem::path& path,
    const std::function<bool(std::wostream&)>& writer);

// Reads a bounded UTF-8 settings/session document and rejects malformed input
// before its format parser sees any state.
bool readUtf8File(const std::filesystem::path& path, std::wstring& text);

// Writes documents on a thread of its own, so that a disk which holds a
// write back holds only that thread: the player's window thread, which also
// draws the video, once waited 6.4 s inside WriteFile for its 4 KB settings
// file (2026-11-26). The latest document wins: one asked for while another
// waits replaces it, and one being written is finished first. A failed
// write is tried again after `retryDelay`, up to `retries` times, unless a
// newer document is asked for meanwhile. The thread starts with the first
// write and ends with finish().
class BackgroundDocumentWriter {
public:
    // Writes the whole text to the path; true when it was written. The
    // default creates the path's folder and writes through
    // writeUtf8FileAtomically.
    using Write = std::function<bool(const std::filesystem::path& path, const std::wstring& text)>;
    // After each attempt, on the writer's thread: whether it was written
    // and, when not, whether it is to be tried again.
    using Outcome = std::function<void(bool written, bool retrying)>;

    BackgroundDocumentWriter(unsigned retries, std::chrono::milliseconds retryDelay, Outcome outcome = {},
                             Write write = {});
    ~BackgroundDocumentWriter();
    BackgroundDocumentWriter(const BackgroundDocumentWriter&) = delete;
    BackgroundDocumentWriter& operator=(const BackgroundDocumentWriter&) = delete;

    // Returns at once.
    void write(std::filesystem::path path, std::wstring text);
    // Waits for the write in progress; a document not yet begun, and a
    // retry, are dropped. Nothing is written once it returns, until the
    // next write().
    void finish();
    // Whether nothing waits and nothing is being written, waiting up to
    // `timeout` for it.
    bool idle(std::chrono::milliseconds timeout);

private:
    struct Document {
        std::filesystem::path path;
        std::wstring text;
    };
    void run();

    const unsigned retries_;
    const std::chrono::milliseconds retryDelay_;
    const Outcome outcome_;
    const Write write_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<Document> pending_;
    bool busy_{};
    bool stopping_{};
    std::thread thread_;
};

}  // namespace quaddeck
