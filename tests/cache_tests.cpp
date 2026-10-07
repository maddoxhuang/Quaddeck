#include "ReadAheadCache.hpp"

#include <windows.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>

extern "C" {
#include <libavformat/avformat.h>
}

using namespace quaddeck;
using namespace std::chrono_literals;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class Predicate> void waitFor(Predicate predicate, const char* message) {
    const auto end = std::chrono::steady_clock::now() + 3s;
    while (!predicate()) {
        require(std::chrono::steady_clock::now() < end, message);
        std::this_thread::sleep_for(2ms);
    }
}
std::uint8_t valueAt(std::int64_t position) {
    return static_cast<std::uint8_t>((position * 37 + position / 251) & 255);
}
struct SlowFile : CacheSource {
    static constexpr std::int64_t length = 512 * 1024;
    std::atomic<bool> offline{};
    std::atomic<bool> blockOpen{};
    std::atomic<unsigned> calls{};
    std::atomic<unsigned> cancelledReads{};
    std::atomic<int> failNext{};
    std::atomic<int> delayMs{1};
    std::int64_t open(const Cancelled& cancelled) override {
        while (blockOpen && !cancelled()) std::this_thread::sleep_for(1ms);
        return cancelled() ? kCacheCancelled : length;
    }
    int read(std::int64_t offset, std::span<std::uint8_t> destination,
             const Cancelled& cancelled) override {
        ++calls;
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs.load());
        while ((offline || std::chrono::steady_clock::now() < end) && !cancelled())
            std::this_thread::sleep_for(1ms);
        if (cancelled()) { ++cancelledReads; return kCacheCancelled; }
        if (failNext.load() > 0) { --failNext; return kCacheError; }
        const auto size = static_cast<std::size_t>(std::min<std::int64_t>(destination.size(), length - offset));
        for (std::size_t i = 0; i < size; ++i) destination[i] = valueAt(offset + i);
        return static_cast<int>(size);
    }
};
void checkBytes(const std::vector<std::uint8_t>& bytes, std::int64_t position, int size) {
    require(size > 0, "Expected real bytes, not EOF/error");
    for (int i = 0; i < size; ++i) require(bytes[i] == valueAt(position + i), "Stale/corrupt cached bytes");
}
void outageAndSeeking() {
    auto budget = std::make_shared<CacheBudget>(128 * 1024);
    auto input = std::make_unique<SlowFile>();
    auto& slow = *input;
    ReadAheadCache cache(std::move(input), budget, 4096, 64 * 1024);
    require(cache.size() == SlowFile::length, "Open size");
    cache.configure(8 * 4096); // simulated playback: 4 KiB/sec, 30 sec target capped by quota
    cache.prime();
    waitFor([&] { return cache.stats().aheadBytes >= 48 * 1024; }, "Background read-ahead never filled");
    slow.offline = true;
    const auto begin = std::chrono::steady_clock::now();
    std::vector<std::uint8_t> bytes(4096);
    for (int block = 0; block < 8; ++block)
        checkBytes(bytes, block * 4096, cache.read(0, bytes));
    require(std::chrono::steady_clock::now() - begin < 200ms, "Cached playback stalled during outage");
    // Audio cursor shares resident bytes and cannot move the video cursor.
    cache.seek(1, 4096, SEEK_SET);
    checkBytes(bytes, 4096, cache.read(1, bytes));
    require(cache.seek(0, 0, SEEK_CUR) == 32768, "Audio moved video cursor");

    cache.seek(0, 400000, SEEK_SET);
    auto blocked = std::async(std::launch::async, [&] { return cache.read(0, bytes); });
    std::this_thread::sleep_for(20ms);
    cache.interrupt(0);
    require(blocked.wait_for(300ms) == std::future_status::ready && blocked.get() == kCacheCancelled,
            "Seek could not cancel old network demand");
    slow.offline = false;
    slow.failNext = 1;
    cache.seek(0, 200000, SEEK_SET);
    checkBytes(bytes, 200000, cache.read(0, bytes));
    require(cache.stats().retries >= 1, "Transient failure was not retried");
    cache.seek(0, -7, SEEK_END);
    checkBytes(bytes, SlowFile::length - 7, cache.read(0, bytes));
    require(cache.read(0, bytes) == 0, "EOF must return zero only at real file end");
    require(cache.seek(0, -SlowFile::length - 1, SEEK_END) < 0, "Negative seek accepted");
    cache.seek(0, 0, SEEK_SET);
    checkBytes(bytes, 0, cache.read(0, bytes));
    std::cout << "Outage, shared audio/video, seek cancellation, retry and EOF PASS\n";
}

void readersDoNotCancelEachOther() {
    auto input = std::make_unique<SlowFile>();
    auto& slow = *input;
    slow.delayMs = 150;
    ReadAheadCache cache(std::move(input), std::make_shared<CacheBudget>(128 * 1024), 4096, 64 * 1024);
    require(cache.size() == SlowFile::length, "Open size");
    std::vector<std::uint8_t> video(4096);
    std::vector<std::uint8_t> audio(4096);
    // Demand-only (unconfigured) so every source read belongs to a reader.
    cache.seek(0, 10 * 4096, SEEK_SET);
    auto pending = std::async(std::launch::async, [&] { return cache.read(0, video); });
    waitFor([&] { return slow.calls >= 1; }, "Video demand never reached the source");
    // The audio demuxer repositions and misses while video's block is in flight.
    cache.seek(1, 50 * 4096, SEEK_SET);
    checkBytes(audio, 50 * 4096, cache.read(1, audio));
    checkBytes(video, 10 * 4096, pending.get());
    cache.interrupt(1);
    require(slow.cancelledReads == 0, "One reader's seek or miss cancelled the other reader's fetch");
    std::cout << "Video and audio demands do not cancel each other PASS\n";
}

void aggregateBudget() {
    auto budget = std::make_shared<CacheBudget>(256 * 1024);
    std::vector<std::unique_ptr<ReadAheadCache>> caches;
    for (int i = 0; i < 5; ++i) {
        caches.push_back(std::make_unique<ReadAheadCache>(std::make_unique<SlowFile>(), budget, 4096, 256 * 1024));
        require(caches.back()->size() > 0, "Parallel open failed");
        caches.back()->configure(8 * 1024 * 1024);
    }
    waitFor([&] {
        require(budget->used <= budget->limit, "Global budget exceeded (including in-flight blocks)");
        return std::all_of(caches.begin(), caches.end(), [](const auto& c) {
            return c->stats().aheadBytes >= 32 * 1024;
        });
    }, "One file monopolized shared capacity");
    for (const auto& cache : caches) require(cache->stats().residentBytes <= 52 * 1024, "Unequal file quota");
    caches.clear();
    require(budget->used == 0 && budget->files == 0, "Cache close leaked memory budget");
    std::cout << "Five-file bounded/fair allocation and cleanup PASS\n";
}

void cancelOpenAndClose() {
    auto budget = std::make_shared<CacheBudget>(65536);
    {
        auto input = std::make_unique<SlowFile>();
        input->blockOpen = true;
        ReadAheadCache cache(std::move(input), budget, 4096, 65536);
        auto pending = std::async(std::launch::async, [&] { return cache.size(); });
        cache.stop();
        require(pending.wait_for(300ms) == std::future_status::ready && pending.get() == kCacheCancelled,
                "Close did not interrupt source open");
    }
    {
        auto input = std::make_unique<SlowFile>();
        input->offline = true;
        ReadAheadCache cache(std::move(input), budget, 4096, 65536);
        require(cache.size() > 0, "Open failed");
        std::vector<std::uint8_t> bytes(4096);
        auto pending = std::async(std::launch::async, [&] { return cache.read(0, bytes); });
        std::this_thread::sleep_for(20ms);
        cache.stop();
        require(pending.wait_for(300ms) == std::future_status::ready && pending.get() == kCacheCancelled,
                "Close did not interrupt source read");
    }
    require(budget->used == 0, "Cancelled in-flight block leaked budget");
    std::cout << "Cancelled opening and offline reads PASS\n";
}

void avioContract() {
    auto cache = std::make_shared<ReadAheadCache>(std::make_unique<SlowFile>(),
        std::make_shared<CacheBudget>(128 * 1024), 4096, 65536);
    AVFormatContext* format = avformat_alloc_context();
    require(format != nullptr, "Format allocation");
    {
        CachedAvio io(cache, 0);
        require(io.attach(format) == 1, "Custom AVIO attach");
        require(avio_size(format->pb) == SlowFile::length, "AVSEEK_SIZE incorrect");
        std::vector<std::uint8_t> bytes(17000);
        checkBytes(bytes, 0, avio_read(format->pb, bytes.data(), static_cast<int>(bytes.size())));
        require(avio_seek(format->pb, 300001, SEEK_SET) == 300001, "AVIO seek failed");
        checkBytes(bytes, 300001, avio_read(format->pb, bytes.data(), static_cast<int>(bytes.size())));
        // Public avio_seek only accepts SET/CUR; demuxers query AVSEEK_SIZE
        // then seek absolutely. The underlying callback also supports END.
        require(avio_seek(format->pb, avio_size(format->pb) - 5, SEEK_SET) == SlowFile::length - 5,
                "AVIO end seek failed");
        require(avio_read(format->pb, bytes.data(), 100) == 5, "Short tail lost");
        require(avio_read(format->pb, bytes.data(), 100) == AVERROR_EOF, "False EOF contract");
        require(avio_seek(format->pb, 0, SEEK_SET) == 0, "Seek back after EOF failed");
        checkBytes(bytes, 0, avio_read(format->pb, bytes.data(), 100));
        format->pb = nullptr;
    }
    avformat_free_context(format);
    auto local = createFileCache(L"C:\\not-opened-local-test.mp4", CacheMode::Network);
    require(local->size() == kCacheBypass, "Local disk should bypass cache before opening the path");
    std::cout << "FFmpeg custom AVIO contract and local bypass PASS\n";
}

void unopenableFilesBypass() {
    // AllFiles runs the real Windows source against a local temporary file.
    const auto path = std::filesystem::temp_directory_path() /
        (L"QuadDeckCacheShare-" + std::to_wstring(GetCurrentProcessId()) + L".bin");
    { std::ofstream(path, std::ios::binary) << std::string(10000, 'x'); }
    {
        auto readable = createFileCache(path.wstring(), CacheMode::AllFiles);
        require(readable->size() == 10000, "An ordinary file should be cached");
    }
    // A recorder keeps its output open for writing and shares reading.
    HANDLE writer = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(writer != INVALID_HANDLE_VALUE, "Writer handle");
    std::int64_t held{};
    {
        auto cache = createFileCache(path.wstring(), CacheMode::AllFiles);
        held = cache->size();
    }
    // The bypass is only useful because FFmpeg's own file open shares writing.
    const auto utf8 = path.u8string();
    AVIOContext* direct{};
    const int directResult = avio_open(&direct,
        reinterpret_cast<const char*>(utf8.c_str()), AVIO_FLAG_READ);
    const std::int64_t directSize = directResult >= 0 ? avio_size(direct) : directResult;
    avio_closep(&direct);
    CloseHandle(writer);
    const auto missing = createFileCache(path.wstring() + L".missing", CacheMode::AllFiles)->size();
    std::filesystem::remove(path);
    require(held == kCacheBypass, "A file open for writing must fall back to direct FFmpeg input");
    require(directSize == 10000, "FFmpeg could not open a file held open for writing");
    require(missing == kCacheBypass, "A missing file must leave the real error to FFmpeg");
    std::cout << "Writer-held and missing files bypass the cache PASS\n";
}
}

int main() {
    try {
        outageAndSeeking();
        readersDoNotCancelEachOther();
        aggregateBudget();
        cancelOpenAndClose();
        avioContract();
        unopenableFilesBypass();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Cache regression FAILED: " << error.what() << '\n';
        return 1;
    }
}
