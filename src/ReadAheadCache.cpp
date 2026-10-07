#include "ReadAheadCache.hpp"

#include <windows.h>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
}

namespace quaddeck {
namespace {
std::shared_ptr<CacheBudget> processBudget() {
    static auto budget = std::make_shared<CacheBudget>();
    return budget;
}

// Called on the cache worker, never on the window thread. Extended UNC and
// remote drive-letter paths are supported; URLs and local disks bypass it.
bool remotePath(const std::wstring& path) {
    if (path.starts_with(L"\\\\?\\UNC\\")) return true;
    if (path.starts_with(L"\\\\") && !path.starts_with(L"\\\\?\\") &&
        !path.starts_with(L"\\\\.\\")) return true;
    const std::size_t start = path.starts_with(L"\\\\?\\") ? 4 : 0;
    if (path.size() < start + 3 || path[start + 1] != L':' ||
        (path[start + 2] != L'\\' && path[start + 2] != L'/')) return false;
    const std::wstring root = path.substr(start, 2) + L"\\";
    return GetDriveTypeW(root.c_str()) == DRIVE_REMOTE;
}

class WindowsCacheSource final : public CacheSource {
public:
    WindowsCacheSource(std::wstring path, CacheMode mode) : path_(std::move(path)), mode_(mode) {}
    ~WindowsCacheSource() override {
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
    }
    std::int64_t open(const Cancelled& cancelled) override {
        if (mode_ == CacheMode::Off || (mode_ == CacheMode::Network && !remotePath(path_)))
            return kCacheBypass;
        if (cancelled()) return kCacheCancelled;
        // No write/delete sharing: cached bytes must belong to one immutable
        // opened file. Nothing is written to the NAS or to a local cache file.
        file_ = CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        // That share mode refuses a file another process still has open for
        // writing, such as a recording in progress, which FFmpeg's own
        // share-everything open reads fine. Any open failure therefore falls
        // back to ordinary uncached input; FFmpeg then either plays the file
        // or reports the real reason it cannot, instead of a cache error.
        if (file_ == INVALID_HANDLE_VALUE) return cancelled() ? kCacheCancelled : kCacheBypass;
        LARGE_INTEGER length{};
        if (!GetFileSizeEx(file_, &length) || length.QuadPart < 0) return kCacheError;
        return length.QuadPart;
    }
    int read(std::int64_t offset, std::span<std::uint8_t> destination,
             const Cancelled& cancelled) override {
        if (cancelled()) return kCacheCancelled;
        OVERLAPPED operation{};
        operation.Offset = static_cast<DWORD>(offset);
        operation.OffsetHigh = static_cast<DWORD>(static_cast<std::uint64_t>(offset) >> 32);
        operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!operation.hEvent) return kCacheError;
        DWORD bytes{};
        const BOOL immediate = ReadFile(file_, destination.data(),
            static_cast<DWORD>(destination.size()), &bytes, &operation);
        DWORD error = immediate ? ERROR_SUCCESS : GetLastError();
        bool aborted = false;
        if (!immediate && error == ERROR_IO_PENDING) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (WaitForSingleObject(operation.hEvent, 20) == WAIT_TIMEOUT) {
                if (cancelled() || std::chrono::steady_clock::now() >= deadline) {
                    aborted = cancelled();
                    CancelIoEx(file_, &operation);
                    break;
                }
            }
            // The OVERLAPPED and destination stay alive until cancellation has
            // completed; returning early would let the driver write freed RAM.
            error = GetOverlappedResult(file_, &operation, &bytes, TRUE)
                ? ERROR_SUCCESS : GetLastError();
        }
        CloseHandle(operation.hEvent);
        if (aborted || cancelled()) return kCacheCancelled;
        if (error == ERROR_HANDLE_EOF) return 0;
        return error == ERROR_SUCCESS ? static_cast<int>(bytes) : kCacheError;
    }
private:
    std::wstring path_;
    CacheMode mode_;
    HANDLE file_{INVALID_HANDLE_VALUE};
};
}

bool CacheBudget::reserve(std::size_t bytes) {
    auto current = used.load();
    do {
        if (bytes > limit || current > limit - bytes) return false;
    } while (!used.compare_exchange_weak(current, current + bytes));
    return true;
}

ReadAheadCache::ReadAheadCache(std::unique_ptr<CacheSource> source,
    std::shared_ptr<CacheBudget> budget, std::size_t blockBytes, std::size_t maxFileBytes)
    : source_(std::move(source)), budget_(budget ? std::move(budget) : processBudget()),
      blockBytes_(blockBytes), maxFileBytes_(maxFileBytes) {
    if (!source_ || blockBytes_ == 0 || blockBytes_ > INT_MAX ||
        budget_->limit < blockBytes_ * 4 || maxFileBytes_ < blockBytes_ * 4)
        throw std::invalid_argument("Invalid read-ahead cache capacity");
    worker_ = std::thread(&ReadAheadCache::run, this);
}

ReadAheadCache::~ReadAheadCache() {
    stop();
    // Close the small race between the worker's cancelled() check and entry
    // into a synchronous SMB CreateFile/GetFileSize call. Reissue cancellation
    // until the worker exits, retaining all I/O storage until completion.
    while (!workerDone_) {
        CancelSynchronousIo(worker_.native_handle());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (worker_.joinable()) worker_.join();
    budget_->release(resident_);
    if (registered_) budget_->files.fetch_sub(1);
}

void ReadAheadCache::stop() {
    stopped_.store(true);
    ioEpoch_.fetch_add(1);
    cv_.notify_all();
    // Also cancel a synchronous Windows metadata/open operation, before an
    // OVERLAPPED file handle exists. Ordinary reads use CancelIoEx above.
    if (worker_.joinable()) CancelSynchronousIo(worker_.native_handle());
}

std::int64_t ReadAheadCache::size() {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [&] { return opened_ || stopped_; });
    return stopped_ ? kCacheCancelled : size_;
}

std::size_t ReadAheadCache::quota() const {
    const auto share = budget_->limit / std::max(1U, budget_->files.load());
    return std::max(blockBytes_ * 2, std::min(maxFileBytes_, share) / blockBytes_ * blockBytes_);
}

std::size_t ReadAheadCache::window() const {
    // Leave two blocks for metadata and a differently positioned audio reader.
    return std::min(desiredBytes_, quota() - blockBytes_ * 2);
}

std::size_t ReadAheadCache::ahead() const {
    std::int64_t position = readers_[0].position;
    const auto begin = position;
    while (position < size_) {
        const auto index = position / static_cast<std::int64_t>(blockBytes_);
        const auto found = blocks_.find(index);
        if (found == blocks_.end()) break;
        position = index * static_cast<std::int64_t>(blockBytes_) + found->second.data.size();
    }
    return static_cast<std::size_t>(position - begin);
}

int ReadAheadCache::read(unsigned reader, std::span<std::uint8_t> destination) {
    if (reader >= readers_.size() || destination.empty()) return kCacheError;
    std::unique_lock lock(mutex_);
    auto& cursor = readers_[reader];
    const auto epoch = cursor.epoch;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    if (stopped_) return kCacheCancelled;
    if (!opened_ || size_ < 0) return kCacheError;
    if (cursor.position >= size_) return 0;
    const auto index = cursor.position / static_cast<std::int64_t>(blockBytes_);
    bool missed = false;
    for (;;) {
        if (stopped_ || cursor.epoch != epoch) return kCacheCancelled;
        if (size_ < 0) return kCacheError;
        const auto found = blocks_.find(index);
        if (found != blocks_.end()) {
            const auto offset = static_cast<std::size_t>(cursor.position % blockBytes_);
            const auto count = std::min(destination.size(), found->second.data.size() - offset);
            std::memcpy(destination.data(), found->second.data.data() + offset, count);
            found->second.touched = ++ticks_;
            cursor.position += count;
            cursor.demand.reset();
            if (!missed) ++hits_;
            cv_.notify_all();
            return static_cast<int>(count);
        }
        if (!missed) {
            ++misses_;
            missed = true;
            cursor.demand = index;
            // A waiting reader outranks read-ahead, but not the other reader.
            cancelUnneededFetch(false);
            cv_.notify_all();
        }
        if (cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
            cursor.demand.reset();
            return kCacheError;
        }
    }
}

std::int64_t ReadAheadCache::seek(unsigned reader, std::int64_t offset, int origin) {
    if (reader >= readers_.size()) return kCacheError;
    std::scoped_lock lock(mutex_);
    if (stopped_) return kCacheCancelled;
    if (size_ < 0) return kCacheError;
    const auto base = origin == SEEK_SET ? 0 : origin == SEEK_CUR ? readers_[reader].position
        : origin == SEEK_END ? size_ : -1;
    if (base < 0 || offset < -base || offset > std::numeric_limits<std::int64_t>::max() - base)
        return kCacheError;
    auto& cursor = readers_[reader];
    cursor.position = base + offset;
    cursor.demand.reset();
    ++cursor.epoch;
    cancelUnneededFetch(true);
    cv_.notify_all();
    return cursor.position;
}

void ReadAheadCache::interrupt(unsigned reader) {
    std::scoped_lock lock(mutex_);
    ++readers_.at(reader).epoch;
    readers_.at(reader).demand.reset();
    cancelUnneededFetch(true);
    cv_.notify_all();
}

// Demuxers reposition their cursors constantly, and the video and audio
// readers do so independently. Cancelling the worker's fetch on every such
// event threw away the other reader's block half-read. Only a fetch that no
// reader is waiting for is cancelled -- and, when keepWindow is set, only if
// it has also left the video read-ahead window.
void ReadAheadCache::cancelUnneededFetch(bool keepWindow) {
    if (!inFlight_) return;
    const auto block = *inFlight_;
    if (std::any_of(readers_.begin(), readers_.end(),
                    [&](const Reader& r) { return r.demand == block; })) return;
    if (keepWindow && configured_) {
        const auto first = readers_[0].position / static_cast<std::int64_t>(blockBytes_);
        const auto count = static_cast<std::int64_t>(std::max<std::size_t>(1, window() / blockBytes_));
        if (block >= first && block < first + count) return;
    }
    ioEpoch_.fetch_add(1);
}

void ReadAheadCache::configure(std::int64_t bitsPerSecond) {
    std::scoped_lock lock(mutex_);
    // Unknown bitrate: reserve a useful, bounded 32 MiB window; no invented
    // seconds are shown in diagnostics. Metadata probing itself is demand-only.
    bytesPerSecond_ = bitsPerSecond > 0 ? static_cast<double>(bitsPerSecond) / 8 : 0;
    desiredBytes_ = static_cast<std::size_t>(std::clamp(bytesPerSecond_ > 0
        ? bytesPerSecond_ * 30 : 32.0 * 1024 * 1024,
        static_cast<double>(blockBytes_), static_cast<double>(maxFileBytes_)));
    configured_ = true;
    cv_.notify_all();
}

void ReadAheadCache::prime(std::chrono::milliseconds maximumWait) {
    std::unique_lock lock(mutex_);
    if (!configured_ || size_ < 0 || stopped_) return;
    const auto epoch = readers_[0].epoch;
    const auto reachable = std::max<std::size_t>(1, window() / blockBytes_) * blockBytes_ -
        static_cast<std::size_t>(readers_[0].position % blockBytes_);
    const auto goal = std::min({window(), reachable, static_cast<std::size_t>(std::max<std::int64_t>(0,
        size_ - readers_[0].position)), static_cast<std::size_t>(bytesPerSecond_ > 0
        ? std::min(bytesPerSecond_ * 3, static_cast<double>(maxFileBytes_)) : blockBytes_ * 4)});
    priming_ = true;
    cv_.notify_all();
    cv_.wait_for(lock, maximumWait, [&] {
        return stopped_ || readers_[0].epoch != epoch || ahead() >= std::min(goal, window());
    });
    priming_ = false;
}

CacheStats ReadAheadCache::stats() const {
    std::scoped_lock lock(mutex_);
    const auto buffered = size_ >= 0 ? ahead() : 0;
    return {opened_ && size_ >= 0, priming_, resident_, buffered, fetched_, hits_, misses_, retries_,
            bytesPerSecond_ > 0 ? buffered / bytesPerSecond_ : 0};
}

void ReadAheadCache::erase(std::map<std::int64_t, Block>::iterator entry) {
    blocks_.erase(entry);
    resident_ -= blockBytes_;
    budget_->release(blockBytes_);
}

bool ReadAheadCache::makeRoom(std::int64_t wanted, bool demand) {
    while (resident_ + blockBytes_ > quota()) {
        auto victim = blocks_.end();
        const auto first = readers_[0].position / static_cast<std::int64_t>(blockBytes_);
        const auto last = first + static_cast<std::int64_t>(window() / blockBytes_);
        for (auto it = blocks_.begin(); it != blocks_.end(); ++it) {
            if (it->first == wanted || std::any_of(readers_.begin(), readers_.end(),
                [&](const Reader& r) { return r.demand == it->first; })) continue;
            if (!demand && it->first >= first && it->first < last) continue;
            if (victim == blocks_.end() || it->second.touched < victim->second.touched) victim = it;
        }
        if (victim == blocks_.end()) return false;
        erase(victim);
    }
    return true;
}

void ReadAheadCache::run() {
    struct Completion {
        std::atomic<bool>& done;
        ~Completion() { done.store(true); }
    } completion{workerDone_};
    try {
        const auto length = source_->open([&] { return stopped_.load(); });
        std::unique_lock lock(mutex_);
        size_ = length;
        opened_ = true;
        if (length >= 0) { budget_->files.fetch_add(1); registered_ = true; }
        cv_.notify_all();
        while (!stopped_ && size_ >= 0) {
            // Shrink old allocations when another file joins the global budget.
            while (resident_ > quota() && !blocks_.empty()) erase(blocks_.begin());
            std::optional<std::int64_t> wanted;
            for (unsigned i = 0; i < readers_.size(); ++i) {
                const auto index = (nextDemand_ + i) % readers_.size();
                const auto& reader = readers_[index];
                if (reader.demand && !blocks_.contains(*reader.demand)) {
                    wanted = reader.demand;
                    nextDemand_ = static_cast<unsigned>((index + 1) % readers_.size());
                    break;
                }
            }
            const bool demand = wanted.has_value();
            if (!wanted && configured_ && readers_[0].position < size_) {
                const auto first = readers_[0].position / static_cast<std::int64_t>(blockBytes_);
                const auto count = std::max<std::size_t>(1, window() / blockBytes_);
                for (std::size_t i = 0; i < count; ++i) {
                    const auto index = first + static_cast<std::int64_t>(i);
                    if (index > (size_ - 1) / static_cast<std::int64_t>(blockBytes_)) break;
                    if (!blocks_.contains(index)) { wanted = index; break; }
                }
            }
            if (!wanted) {
                // Window full: every change that could create work -- a read,
                // seek, interrupt, configure or stop -- notifies. The timeout
                // only picks up a quota change when another file opens or
                // closes, which touches the shared budget, not this cache.
                cv_.wait_for(lock, std::chrono::seconds(1));
                continue;
            }
            if (!makeRoom(*wanted, demand) || !budget_->reserve(blockBytes_)) {
                // Other files hold the budget and do not notify this cache.
                cv_.wait_for(lock, std::chrono::milliseconds(20));
                continue;
            }
            struct Reservation {
                CacheBudget& budget;
                std::size_t bytes;
                ~Reservation() { if (bytes) budget.release(bytes); }
            } reservation{*budget_, blockBytes_};
            const auto index = *wanted;
            inFlight_ = index;
            const auto position = index * static_cast<std::int64_t>(blockBytes_);
            const auto count = static_cast<std::size_t>(std::min<std::int64_t>(blockBytes_, size_ - position));
            const auto epoch = ioEpoch_.load();
            lock.unlock();
            Block block;
            int result = kCacheError;
            try {
                block.data.resize(count);
                std::size_t completed = 0;
                while (completed < count) {
                    result = source_->read(position + completed, std::span(block.data).subspan(completed),
                        [&] { return stopped_ || ioEpoch_.load() != epoch; });
                    if (result <= 0 || static_cast<std::size_t>(result) > count - completed) break;
                    completed += result;
                }
                if (completed == count) result = static_cast<int>(count);
                else if (result >= 0) result = kCacheError;
            } catch (...) { result = kCacheError; }
            lock.lock();
            inFlight_.reset();
            if (result > 0 && !stopped_ && ioEpoch_.load() == epoch) {
                block.touched = ++ticks_;
                blocks_.emplace(index, std::move(block));
                reservation.bytes = 0;
                resident_ += blockBytes_;
                fetched_ += result;
            } else {
                std::vector<std::uint8_t>().swap(block.data);
                if (result == kCacheError) {
                    ++retries_;
                    cv_.wait_for(lock, std::chrono::milliseconds(250));
                }
            }
            cv_.notify_all();
        }
    } catch (...) {
        std::scoped_lock lock(mutex_);
        size_ = kCacheError;
        opened_ = true;
        cv_.notify_all();
    }
}

std::shared_ptr<ReadAheadCache> createFileCache(const std::wstring& path, CacheMode mode) {
    if (mode == CacheMode::Off) return {};
    return std::make_shared<ReadAheadCache>(std::make_unique<WindowsCacheSource>(path, mode));
}

CachedAvio::~CachedAvio() {
    if (io_) { av_freep(&io_->buffer); avio_context_free(&io_); }
}

int CachedAvio::attach(AVFormatContext* format) {
    if (!cache_) return 0;
    const auto length = cache_->size();
    if (length == kCacheBypass) return 0;
    if (length < 0) return length == kCacheCancelled ? AVERROR_EXIT : AVERROR(EIO);
    auto* buffer = static_cast<std::uint8_t*>(av_malloc(64 * 1024));
    if (!buffer) return AVERROR(ENOMEM);
    io_ = avio_alloc_context(buffer, 64 * 1024, 0, this, readPacket, nullptr, seekPacket);
    if (!io_) { av_free(buffer); return AVERROR(ENOMEM); }
    io_->seekable = AVIO_SEEKABLE_NORMAL;
    format->pb = io_;
    format->flags |= AVFMT_FLAG_CUSTOM_IO;
    return 1;
}

void CachedAvio::clearError() {
    if (io_) { io_->error = 0; io_->eof_reached = 0; }
}

int CachedAvio::readPacket(void* opaque, std::uint8_t* buffer, int count) {
    auto& self = *static_cast<CachedAvio*>(opaque);
    const int result = self.cache_->read(self.reader_, {buffer, static_cast<std::size_t>(count)});
    if (result == 0) return AVERROR_EOF;
    if (result == kCacheCancelled) return AVERROR_EXIT;
    return result < 0 ? AVERROR(EIO) : result;
}

std::int64_t CachedAvio::seekPacket(void* opaque, std::int64_t offset, int origin) {
    auto& self = *static_cast<CachedAvio*>(opaque);
    if (origin & AVSEEK_SIZE) return self.cache_->size();
    const auto result = self.cache_->seek(self.reader_, offset, origin & ~AVSEEK_FORCE);
    return result < 0 ? AVERROR(EINVAL) : result;
}
} // namespace quaddeck
