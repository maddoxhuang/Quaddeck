#include "LocalThumbnails.hpp"

#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

namespace quaddeck {

namespace {
// How long closing waits for a picture being made before leaving it.
constexpr auto kStopWait = std::chrono::seconds(1);
constexpr DWORD kStopPollMs = 50;
}  // namespace

struct LocalThumbnailer::State {
    struct Job {
        std::wstring path;
        int width{};
        int height{};
        Handler handler;
    };
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::deque<Job> pending;
    std::deque<std::pair<Handler, Result>> completed;
    bool stop{};
};

LocalThumbnailer::LocalThumbnailer()
    : state_(std::make_shared<State>()), worker_(&LocalThumbnailer::workerMain, state_) {}

// A picture being made cannot be called off: a provider may be opening a
// file on a share that has just gone away, for as long as SMB takes to give
// up. Joining it kept the closed player's process, and its single-instance
// mutex, alive and invisible for that long, and a launch meanwhile was
// refused. So closing waits a moment, cancelling the synchronous I/O the
// worker started on its own thread, and then leaves it: it holds its own
// share of the state, no handler runs off the window thread, and the process
// ending takes it, as it does the folder readers'.
LocalThumbnailer::~LocalThumbnailer() {
    {
        std::scoped_lock lock(state_->mutex);
        state_->stop = true;
        state_->pending.clear();
    }
    state_->wake.notify_all();
    if (!worker_.joinable()) return;
    const HANDLE thread = worker_.native_handle();
    const auto deadline = std::chrono::steady_clock::now() + kStopWait;
    while (WaitForSingleObject(thread, kStopPollMs) == WAIT_TIMEOUT) {
        if (std::chrono::steady_clock::now() >= deadline) {
            worker_.detach();
            return;
        }
        CancelSynchronousIo(thread);
    }
    worker_.join();
}

void LocalThumbnailer::request(std::wstring path, int width, int height, Handler handler) {
    {
        std::scoped_lock lock(state_->mutex);
        state_->pending.push_back(State::Job{std::move(path), width, height, std::move(handler)});
    }
    state_->wake.notify_one();
}

void LocalThumbnailer::pump() {
    std::deque<std::pair<Handler, Result>> ready;
    {
        std::scoped_lock lock(state_->mutex);
        ready.swap(state_->completed);
    }
    for (auto& [handler, result] : ready) {
        if (handler) handler(result);
    }
}

void LocalThumbnailer::clearPending() {
    std::scoped_lock lock(state_->mutex);
    for (auto& job : state_->pending) {
        if (!job.handler) continue;
        Result cancelled;
        cancelled.cancelled = true;
        state_->completed.emplace_back(std::move(job.handler), std::move(cancelled));
    }
    state_->pending.clear();
}

void LocalThumbnailer::workerMain(std::shared_ptr<State> state) {
    // Thumbnail providers are in-process COM objects; this thread never
    // pumps messages, so it joins the multithreaded apartment.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        State::Job job;
        {
            std::unique_lock lock(state->mutex);
            state->wake.wait(lock, [&] { return state->stop || !state->pending.empty(); });
            if (state->stop) break;
            job = std::move(state->pending.front());
            state->pending.pop_front();
        }
        Result result;
        result.bytes = shellThumbnail(job.path, job.width, job.height);
        {
            std::scoped_lock lock(state->mutex);
            // After closing, the handler is only let go, never run, here.
            if (job.handler && !state->stop) {
                state->completed.emplace_back(std::move(job.handler), std::move(result));
            }
        }
    }
    if (SUCCEEDED(com)) CoUninitialize();
}

std::string LocalThumbnailer::shellThumbnail(const std::wstring& path, int width, int height) {
    if (path.empty() || width <= 0 || height <= 0) return {};
    Microsoft::WRL::ComPtr<IShellItemImageFactory> factory;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&factory)))) return {};
    HBITMAP bitmap = nullptr;
    const SIZE wanted{width, height};
    // A thumbnail or nothing: the file type's icon would say nothing about
    // the video, and the tile shows the title's initial instead. The shell
    // scales it to fit the size asked for, so the bytes kept stay small.
    if (FAILED(factory->GetImage(wanted, SIIGBF_THUMBNAILONLY, &bitmap)) || !bitmap) {
        return {};
    }
    BITMAP info{};
    std::string bytes;
    if (GetObjectW(bitmap, sizeof(info), &info) == sizeof(info) && info.bmWidth > 0 && info.bmHeight > 0 &&
        info.bmWidth <= 8192 && info.bmHeight <= 8192) {
        BITMAPINFOHEADER header{};
        header.biSize = sizeof(header);
        header.biWidth = info.bmWidth;
        header.biHeight = info.bmHeight;   // bottom-up, as a BMP file is
        header.biPlanes = 1;
        header.biBitCount = 32;
        header.biCompression = BI_RGB;
        const std::size_t pixelBytes = static_cast<std::size_t>(info.bmWidth) *
                                       static_cast<std::size_t>(info.bmHeight) * 4u;
        std::vector<std::uint8_t> pixels(pixelBytes);
        if (const HDC screen = GetDC(nullptr)) {
            BITMAPINFO request{};
            request.bmiHeader = header;
            const int lines = GetDIBits(screen, bitmap, 0, static_cast<UINT>(info.bmHeight), pixels.data(),
                                        &request, DIB_RGB_COLORS);
            ReleaseDC(nullptr, screen);
            if (lines == info.bmHeight) {
                BITMAPFILEHEADER file{};
                file.bfType = 0x4D42;   // "BM"
                file.bfOffBits = sizeof(file) + sizeof(header);
                file.bfSize = static_cast<DWORD>(file.bfOffBits + pixelBytes);
                header.biSizeImage = static_cast<DWORD>(pixelBytes);
                bytes.resize(file.bfSize);
                std::memcpy(bytes.data(), &file, sizeof(file));
                std::memcpy(bytes.data() + sizeof(file), &header, sizeof(header));
                std::memcpy(bytes.data() + file.bfOffBits, pixels.data(), pixelBytes);
            }
        }
    }
    DeleteObject(bitmap);
    return bytes;
}

}  // namespace quaddeck
