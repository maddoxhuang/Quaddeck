#include "LocalThumbnails.hpp"

#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace quaddeck {

// The worker starts from the body, after every member it uses exists.
LocalThumbnailer::LocalThumbnailer() { worker_ = std::thread(&LocalThumbnailer::workerMain, this); }

LocalThumbnailer::~LocalThumbnailer() {
    {
        std::scoped_lock lock(mutex_);
        stop_ = true;
        pending_.clear();
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void LocalThumbnailer::request(std::wstring path, int width, int height, Handler handler) {
    {
        std::scoped_lock lock(mutex_);
        pending_.push_back(Job{std::move(path), width, height, std::move(handler)});
    }
    wake_.notify_one();
}

void LocalThumbnailer::pump() {
    std::deque<std::pair<Handler, Result>> ready;
    {
        std::scoped_lock lock(mutex_);
        ready.swap(completed_);
    }
    for (auto& [handler, result] : ready) {
        if (handler) handler(result);
    }
}

void LocalThumbnailer::clearPending() {
    std::scoped_lock lock(mutex_);
    for (auto& job : pending_) {
        if (!job.handler) continue;
        Result cancelled;
        cancelled.cancelled = true;
        completed_.emplace_back(std::move(job.handler), std::move(cancelled));
    }
    pending_.clear();
}

std::size_t LocalThumbnailer::pendingCount() const {
    std::scoped_lock lock(mutex_);
    return pending_.size() + (busy_ ? 1 : 0);
}

void LocalThumbnailer::workerMain() {
    // Thumbnail providers are in-process COM objects; this thread never
    // pumps messages, so it joins the multithreaded apartment.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return stop_ || !pending_.empty(); });
            if (stop_) break;
            job = std::move(pending_.front());
            pending_.pop_front();
            busy_ = true;
        }
        Result result;
        result.bytes = shellThumbnail(job.path, job.width, job.height);
        {
            std::scoped_lock lock(mutex_);
            busy_ = false;
            if (job.handler && !stop_) completed_.emplace_back(std::move(job.handler), std::move(result));
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
