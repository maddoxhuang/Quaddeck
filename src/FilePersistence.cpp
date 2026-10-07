#include "FilePersistence.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <codecvt>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace quaddeck {

namespace {

constexpr std::uint64_t kMaximumDocumentBytes = 16ULL * 1024ULL * 1024ULL;

std::wstring nextTransactionToken() {
    static std::atomic<std::uint64_t> sequence{0};
    const auto tick = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto ordinal = sequence.fetch_add(1, std::memory_order_relaxed);
#ifdef _WIN32
    return std::to_wstring(GetCurrentProcessId()) + L"." +
           std::to_wstring(tick) + L"." + std::to_wstring(ordinal);
#else
    return std::to_wstring(tick) + L"." + std::to_wstring(ordinal);
#endif
}

std::filesystem::path siblingPath(
    const std::filesystem::path& path, std::wstring_view role) {
    return path.parent_path() /
        (path.filename().wstring() + L"." + std::wstring(role) + L"." +
         nextTransactionToken());
}

bool encodeUtf8(const std::wstring& wide, std::string& utf8) {
#ifdef _WIN32
    if (wide.empty()) {
        utf8.clear();
        return true;
    }
    if (wide.size() > static_cast<std::size_t>(INT_MAX)) return false;
    const int wideSize = static_cast<int>(wide.size());
    const int bytes = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), wideSize,
        nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return false;
    utf8.resize(static_cast<std::size_t>(bytes));
    return WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), wideSize,
        utf8.data(), bytes, nullptr, nullptr) == bytes;
#else
    try {
        std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
        utf8 = converter.to_bytes(wide);
        return true;
    } catch (...) {
        return false;
    }
#endif
}

bool decodeUtf8(std::string_view utf8, std::wstring& wide) {
    if (utf8.size() >= 3 &&
        static_cast<unsigned char>(utf8[0]) == 0xEFU &&
        static_cast<unsigned char>(utf8[1]) == 0xBBU &&
        static_cast<unsigned char>(utf8[2]) == 0xBFU) {
        utf8.remove_prefix(3);
    }
#ifdef _WIN32
    if (utf8.empty()) {
        wide.clear();
        return true;
    }
    if (utf8.size() > static_cast<std::size_t>(INT_MAX)) return false;
    const int byteSize = static_cast<int>(utf8.size());
    const int characters = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), byteSize, nullptr, 0);
    if (characters <= 0) return false;
    wide.resize(static_cast<std::size_t>(characters));
    return MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), byteSize,
        wide.data(), characters) == characters;
#else
    try {
        std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
        wide = converter.from_bytes(utf8.data(), utf8.data() + utf8.size());
        return true;
    } catch (...) {
        return false;
    }
#endif
}

#ifdef _WIN32

enum class DurableWriteResult { Success, Collision, FailureUnowned, FailureOwned };

bool readBytes(const std::filesystem::path& path, std::string& bytes) {
    const HANDLE file = CreateFileW(
        path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    bool success = GetFileSizeEx(file, &size) && size.QuadPart >= 0 &&
                   static_cast<std::uint64_t>(size.QuadPart) <= kMaximumDocumentBytes;
    if (success) bytes.resize(static_cast<std::size_t>(size.QuadPart));
    std::size_t offset = 0;
    while (success && offset < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
            bytes.size() - offset, static_cast<std::size_t>(MAXDWORD)));
        DWORD received = 0;
        if (!ReadFile(file, bytes.data() + offset, chunk, &received, nullptr) ||
            received == 0) {
            success = false;
            break;
        }
        offset += received;
    }
    if (!CloseHandle(file)) success = false;
    return success && offset == bytes.size();
}

DurableWriteResult writeDurably(
    const std::filesystem::path& temporary, std::string_view bytes) {
    const HANDLE file = CreateFileW(
        temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS
            ? DurableWriteResult::Collision : DurableWriteResult::FailureUnowned;
    }

    bool success = true;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
            bytes.size() - offset, static_cast<std::size_t>(MAXDWORD)));
        DWORD written = 0;
        if (!WriteFile(file, bytes.data() + offset, chunk, &written, nullptr) ||
            written != chunk) {
            success = false;
            break;
        }
        offset += written;
    }
    if (success && !FlushFileBuffers(file)) success = false;
    if (!CloseHandle(file)) success = false;
    if (!success) {
        DeleteFileW(temporary.c_str());
        return DurableWriteResult::FailureOwned;
    }
    return DurableWriteResult::Success;
}

bool pathExists(const std::filesystem::path& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool replaceWithTemporaryFile(
    const std::filesystem::path& temporary,
    const std::filesystem::path& destination) {
    if (!pathExists(destination)) {
        return MoveFileExW(temporary.c_str(), destination.c_str(),
                           MOVEFILE_WRITE_THROUGH) != FALSE;
    }

    const auto backup = siblingPath(destination, L"rollback");
    if (ReplaceFileW(destination.c_str(), temporary.c_str(), backup.c_str(),
                     0, nullptr, nullptr)) {
        // The backup exists only to make every documented partial-failure
        // state recoverable; successful saves do not leave media paths behind.
        DeleteFileW(backup.c_str());
        return true;
    }

    const DWORD replaceError = GetLastError();
    // Only ERROR_UNABLE_TO_MOVE_REPLACEMENT_2 documents that ReplaceFileW has
    // moved the original to our backup name. Put that file back when the
    // destination is still absent. On every other failure (or if another
    // writer recreated the destination), never delete an unproven path.
    if (replaceError == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2 &&
        !pathExists(destination) && pathExists(backup)) {
        if (!MoveFileExW(backup.c_str(), destination.c_str(),
                         MOVEFILE_WRITE_THROUGH)) {
            return false;
        }
    }
    return false;
}

#else

enum class DurableWriteResult { Success, Collision, FailureUnowned, FailureOwned };

bool readBytes(const std::filesystem::path& path, std::string& bytes) {
    std::error_code sizeError;
    const auto size = std::filesystem::file_size(path, sizeError);
    if (sizeError || size > kMaximumDocumentBytes) return false;
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return input.eof() && bytes.size() == size;
}

DurableWriteResult writeDurably(
    const std::filesystem::path& temporary, std::string_view bytes) {
    std::error_code existenceError;
    if (std::filesystem::exists(temporary, existenceError)) {
        return DurableWriteResult::Collision;
    }
    if (existenceError) return DurableWriteResult::FailureUnowned;
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) return DurableWriteResult::FailureUnowned;
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.flush();
    const bool success = output.good();
    output.close();
    if (success && !output.fail()) return DurableWriteResult::Success;
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return DurableWriteResult::FailureOwned;
}

bool replaceWithTemporaryFile(
    const std::filesystem::path& temporary,
    const std::filesystem::path& destination) {
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    return !error;
}

#endif

}  // namespace

bool writeUtf8FileAtomically(
    const std::filesystem::path& path,
    const std::function<bool(std::wostream&)>& writer) {
    if (path.empty() || path.filename().empty() || !writer) return false;

    std::wstring serialized;
    try {
        std::wostringstream output;
        output.imbue(std::locale::classic());
        if (!writer(output) || !output.good()) return false;
        serialized = output.str();
    } catch (...) {
        return false;
    }

    std::string utf8;
    if (!encodeUtf8(serialized, utf8)) return false;

    std::filesystem::path temporary;
    bool written = false;
    for (int attempt = 0; attempt < 16 && !written; ++attempt) {
        temporary = siblingPath(path, L"tmp");
        const auto result = writeDurably(temporary, utf8);
        if (result == DurableWriteResult::Success) written = true;
        else if (result == DurableWriteResult::FailureOwned) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return false;
        } else if (result == DurableWriteResult::FailureUnowned) {
            return false;
        }
    }
    // Every collision belongs to another writer (or a stale transaction), so
    // none of those paths may be removed by this operation.
    if (!written) return false;
    if (replaceWithTemporaryFile(temporary, path)) return true;

    std::error_code ignored;
    if (!temporary.empty()) std::filesystem::remove(temporary, ignored);
    return false;
}

bool readUtf8File(const std::filesystem::path& path, std::wstring& text) {
    std::string bytes;
    if (path.empty() || !readBytes(path, bytes)) return false;
    return decodeUtf8(bytes, text);
}

}  // namespace quaddeck
