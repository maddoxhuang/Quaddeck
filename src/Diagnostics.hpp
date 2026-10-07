#pragma once

// One diagnostic log for the whole process.
//
// Three translation units used to append to "QuadDeck.log" independently: the
// decoder, the renderer and the application each opened their own stream, two
// of them behind their own private mutex and one behind none at all. Concurrent
// lines could interleave mid-record, and the relative path meant the file
// landed in whatever directory the process happened to be started from.

#include <windows.h>
#include <objbase.h>
#include <shlobj.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <string>
#include <system_error>

namespace quaddeck {

namespace detail {

// Function-local statics in inline functions have one instance across the whole
// program, so every translation unit shares this mutex and this resolved path.
inline std::mutex& diagnosticMutex() {
    static std::mutex mutex;
    return mutex;
}

inline std::filesystem::path resolveDiagnosticPath() {
    std::wstring modulePath(MAX_PATH, L'\0');
    for (;;) {
        const DWORD written = GetModuleFileNameW(
            nullptr, modulePath.data(), static_cast<DWORD>(modulePath.size()));
        if (written == 0) break;
        if (written < modulePath.size()) {
            modulePath.resize(written);
            // Writing next to the executable keeps the log where the README and
            // support requests expect it, but an installation directory is not
            // always writable. Fall back to the same per-user location that
            // already holds settings.qconfig.
            const auto candidate =
                std::filesystem::path(modulePath).parent_path() / L"QuadDeck.log";
            std::ofstream probe(candidate, std::ios::app);
            if (probe) return candidate;
            break;
        }
        modulePath.resize(modulePath.size() * 2);
    }
    PWSTR localAppData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE,
                                       nullptr, &localAppData)) && localAppData) {
        std::filesystem::path directory(localAppData);
        CoTaskMemFree(localAppData);
        directory /= L"QuadDeck";
        std::error_code code;
        std::filesystem::create_directories(directory, code);
        if (!code) return directory / L"QuadDeck.log";
    }
    return std::filesystem::path(L"QuadDeck.log");
}

inline const std::filesystem::path& diagnosticPath() {
    static const std::filesystem::path path = resolveDiagnosticPath();
    return path;
}

}  // namespace detail

// Appended records are capped so a long-running session cannot grow the log
// without bound; each seek writes several lines.
inline constexpr std::uintmax_t kMaximumDiagnosticBytes = 4u * 1024u * 1024u;

inline void appendDiagnostic(const std::string& message) {
    std::scoped_lock lock(detail::diagnosticMutex());
    const auto& path = detail::diagnosticPath();
    std::error_code code;
    const auto size = std::filesystem::file_size(path, code);
    auto mode = std::ios::app;
    bool rotated = false;
    if (!code && size > kMaximumDiagnosticBytes) {
        mode = std::ios::trunc;
        rotated = true;
    }
    std::ofstream output(path, mode);
    if (!output) return;
    if (rotated) output << "--- QuadDeck.log truncated after reaching the size limit ---\n";
    SYSTEMTIME timestamp{};
    GetLocalTime(&timestamp);
    output << std::setfill('0')
           << '[' << std::setw(4) << timestamp.wYear << '-'
           << std::setw(2) << timestamp.wMonth << '-'
           << std::setw(2) << timestamp.wDay << ' '
           << std::setw(2) << timestamp.wHour << ':'
           << std::setw(2) << timestamp.wMinute << ':'
           << std::setw(2) << timestamp.wSecond << '.'
           << std::setw(3) << timestamp.wMilliseconds
           << " tid=" << GetCurrentThreadId() << "] "
           << message << '\n';
}

}  // namespace quaddeck
