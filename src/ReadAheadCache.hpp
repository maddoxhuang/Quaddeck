#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

struct AVFormatContext;
struct AVIOContext;

namespace quaddeck {

// Negative results are deliberately distinct from EOF (zero bytes).
inline constexpr int kCacheError = -1;
inline constexpr int kCacheCancelled = -2;
inline constexpr int kCacheBypass = -3;
enum class CacheMode { Off, Network, AllFiles }; // AllFiles also serves native probes.

class CacheSource {
public:
    using Cancelled = std::function<bool()>;
    virtual ~CacheSource() = default;
    virtual std::int64_t open(const Cancelled& cancelled) = 0;
    virtual int read(std::int64_t offset, std::span<std::uint8_t> destination,
                     const Cancelled& cancelled) = 0;
};

class CacheBudget {
public:
    explicit CacheBudget(std::size_t bytes = 1024ULL * 1024 * 1024) : limit(bytes) {}
    const std::size_t limit;
    std::atomic<std::size_t> used{};
    std::atomic<unsigned> files{};
    bool reserve(std::size_t bytes);
    void release(std::size_t bytes) { used.fetch_sub(bytes); }
};

struct CacheStats {
    bool enabled{};
    bool priming{};
    std::size_t residentBytes{};
    std::size_t aheadBytes{};
    std::uint64_t fetchedBytes{};
    std::uint64_t hits{};
    std::uint64_t misses{};
    std::uint64_t retries{};
    double aheadSeconds{};
};

// One cancellable reader worker per file. Video and audio AVIO cursors share
// immutable compressed blocks but never share a demux position or seek state.
class ReadAheadCache {
public:
    explicit ReadAheadCache(std::unique_ptr<CacheSource> source,
        std::shared_ptr<CacheBudget> budget = {}, std::size_t blockBytes = 1024 * 1024,
        std::size_t maxFileBytes = 256ULL * 1024 * 1024);
    ~ReadAheadCache();
    ReadAheadCache(const ReadAheadCache&) = delete;
    ReadAheadCache& operator=(const ReadAheadCache&) = delete;
    void stop();
    std::int64_t size(); // waits only on background open; stop wakes waiters
    int read(unsigned reader, std::span<std::uint8_t> destination);
    std::int64_t seek(unsigned reader, std::int64_t offset, int origin);
    void interrupt(unsigned reader);
    void configure(std::int64_t bitsPerSecond);
    void prime(std::chrono::milliseconds maximumWait = std::chrono::seconds(5));
    CacheStats stats() const;

private:
    struct Reader {
        std::int64_t position{};
        std::uint64_t epoch{};
        std::optional<std::int64_t> demand;
    };
    struct Block { std::vector<std::uint8_t> data; std::uint64_t touched{}; };
    void run();
    std::size_t quota() const;
    std::size_t window() const;
    std::size_t ahead() const;
    void erase(std::map<std::int64_t, Block>::iterator entry);
    bool makeRoom(std::int64_t wanted, bool demand);
    void cancelUnneededFetch(bool keepWindow);
    std::unique_ptr<CacheSource> source_;
    std::shared_ptr<CacheBudget> budget_;
    const std::size_t blockBytes_;
    const std::size_t maxFileBytes_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    std::atomic<bool> stopped_{};
    std::atomic<bool> workerDone_{};
    std::atomic<std::uint64_t> ioEpoch_{};
    std::int64_t size_{kCacheError};
    bool opened_{};
    bool registered_{};
    bool configured_{};
    bool priming_{};
    std::array<Reader, 2> readers_{};
    unsigned nextDemand_{};
    std::map<std::int64_t, Block> blocks_;
    std::optional<std::int64_t> inFlight_;
    std::size_t desiredBytes_{};
    std::size_t resident_{};
    double bytesPerSecond_{};
    std::uint64_t ticks_{};
    std::uint64_t fetched_{};
    std::uint64_t hits_{};
    std::uint64_t misses_{};
    std::uint64_t retries_{};
};

std::shared_ptr<ReadAheadCache> createFileCache(const std::wstring& path, CacheMode mode);

class CachedAvio {
public:
    CachedAvio(std::shared_ptr<ReadAheadCache> cache, unsigned reader)
        : cache_(std::move(cache)), reader_(reader) {}
    ~CachedAvio();
    // 1: attached, 0: ordinary FFmpeg input, negative: real open failure.
    int attach(AVFormatContext* format);
    void clearError();
private:
    static int readPacket(void*, std::uint8_t*, int);
    static std::int64_t seekPacket(void*, std::int64_t, int);
    std::shared_ptr<ReadAheadCache> cache_;
    unsigned reader_{};
    AVIOContext* io_{};
};
} // namespace quaddeck
