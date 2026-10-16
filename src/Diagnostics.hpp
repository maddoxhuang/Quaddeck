#pragma once

// One diagnostic log for the whole process.
//
// Three translation units used to append to "QuadDeck.log" independently: the
// decoder, the renderer and the application each opened their own stream, two
// of them behind their own private mutex and one behind none at all. Concurrent
// lines could interleave mid-record, and the relative path meant the file
// landed in whatever directory the process happened to be started from.

#include "BuildInfo.hpp"

#include <windows.h>
#include <objbase.h>
#include <shlobj.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
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

// The older half of the log: QuadDeck.log becomes QuadDeck.1.log when it
// reaches the size limit, replacing the one before.
inline std::filesystem::path previousDiagnosticPath(const std::filesystem::path& path) {
    return path.parent_path() / (path.stem().wstring() + L".1" + path.extension().wstring());
}

// Appends one record, first moving a log past `limit` to the older name. A
// HANG: report written just before the limit therefore survives at least
// another limit's worth of records, where emptying the file lost it at
// once. Only when the move fails (another process holds the file without
// sharing deletion) is the file emptied, so it stays bounded either way.
inline void appendDiagnosticTo(const std::filesystem::path& path, const std::string& message,
                               std::uintmax_t limit) {
    std::error_code code;
    const auto size = std::filesystem::file_size(path, code);
    auto mode = std::ios::app;
    const char* note = nullptr;
    if (!code && size > limit) {
        const auto previous = previousDiagnosticPath(path);
        if (MoveFileExW(path.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            note = "--- earlier records are in the .1 log beside this one ---\n";
        } else {
            mode = std::ios::trunc;
            note = "--- log emptied after reaching the size limit; it could not be moved aside ---\n";
        }
    }
    std::ofstream output(path, mode);
    if (!output) return;
    if (note) output << note;
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

}  // namespace detail

// Each half of the log is capped so a long-running session cannot grow it
// without bound; each seek writes several lines, and with four panes the
// performance lines alone come to about 0.6 MB an hour.
inline constexpr std::uintmax_t kMaximumDiagnosticBytes = 4u * 1024u * 1024u;

inline void appendDiagnostic(const std::string& message) {
    std::scoped_lock lock(detail::diagnosticMutex());
    detail::appendDiagnosticTo(detail::diagnosticPath(), message, kMaximumDiagnosticBytes);
}

// The line a run starts its log with: which build wrote what follows. The
// version and commit come from CMake; the image's link time stamp and the
// CodeView record name the exact binary and the PDB that matches it, so a
// HANG: stack's offsets can be read against the right source and symbols.
inline std::string describeBuild(HMODULE module) {
    std::ostringstream line;
    line << "QuadDeck " << kBuildVersion << " started: commit " << kBuildCommit << ", "
         << kBuildConfiguration;
    const auto* base = reinterpret_cast<const unsigned char*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE) return line.str();
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return line.str();

    const DWORD stamp = nt->FileHeader.TimeDateStamp;
    char text[64]{};
    std::snprintf(text, sizeof(text), "%08lX", static_cast<unsigned long>(stamp));
    line << ", image stamp " << text;
    const std::time_t seconds = static_cast<std::time_t>(stamp);
    std::tm utc{};
    if (gmtime_s(&utc, &seconds) == 0) line << " (" << std::put_time(&utc, "%Y-%m-%d %H:%M:%S") << " UTC)";

    // CodeView 7.0 ("RSDS"): the PDB's GUID and age, what a debugger and
    // dbghelp match a PDB by. Only the PDB's file name is given.
    struct CodeViewPdb70 {
        DWORD signature;
        GUID guid;
        DWORD age;
        char path[1];
    };
    const IMAGE_DATA_DIRECTORY& debug = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    const auto* entries = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(base + debug.VirtualAddress);
    const std::size_t count = debug.VirtualAddress ? debug.Size / sizeof(IMAGE_DEBUG_DIRECTORY) : 0;
    for (std::size_t index = 0; index < count; ++index) {
        const auto& entry = entries[index];
        constexpr std::size_t pathOffset = offsetof(CodeViewPdb70, path);
        if (entry.Type != IMAGE_DEBUG_TYPE_CODEVIEW || !entry.AddressOfRawData ||
            entry.SizeOfData <= pathOffset) continue;
        const auto* record = reinterpret_cast<const CodeViewPdb70*>(base + entry.AddressOfRawData);
        if (record->signature != 0x53445352) continue;  // "RSDS"
        std::string pdb(record->path, strnlen(record->path, entry.SizeOfData - pathOffset));
        pdb = pdb.substr(pdb.find_last_of("\\/") + 1);
        const GUID& guid = record->guid;
        std::snprintf(text, sizeof(text), "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                      static_cast<unsigned long>(guid.Data1), guid.Data2, guid.Data3, guid.Data4[0],
                      guid.Data4[1], guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5],
                      guid.Data4[6], guid.Data4[7]);
        line << ", PDB " << pdb << ' ' << text << " age " << record->age;
        break;
    }
    return line.str();
}

}  // namespace quaddeck
