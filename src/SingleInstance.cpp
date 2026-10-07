#include "SingleInstance.hpp"

#include <objbase.h>
#include <sddl.h>
#include <wincrypt.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

namespace quaddeck {
namespace {
using Clock = std::chrono::steady_clock;
using Bytes = std::vector<std::uint8_t>;
constexpr std::chrono::milliseconds kConnectionTimeout{1500};
constexpr DWORD kPipeClientAccess = FILE_READ_DATA | FILE_WRITE_DATA |
    FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE;

class Handle {
public:
    explicit Handle(HANDLE value = nullptr) : value_(value) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return value_; }
    explicit operator bool() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
    void reset(HANDLE value = nullptr) {
        if (*this) CloseHandle(value_);
        value_ = value;
    }
private:
    HANDLE value_{};
};

bool nonzeroId(const SingleInstance::RequestId& id) {
    return std::any_of(id.begin(), id.end(), [](auto byte) { return byte != 0; });
}

void append32(Bytes& bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

void append64(Bytes& bytes, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

std::uint32_t read32(const Bytes& bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8)
        value |= static_cast<std::uint32_t>(bytes[offset++]) << shift;
    return value;
}

std::uint64_t read64(const Bytes& bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8)
        value |= static_cast<std::uint64_t>(bytes[offset++]) << shift;
    return value;
}

bool validUtf16(const std::wstring& value) {
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto unit = static_cast<std::uint16_t>(value[i]);
        if (unit < 32 || unit == 127) return false;
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            if (++i == value.size()) return false;
            const auto next = static_cast<std::uint16_t>(value[i]);
            if (next < 0xDC00 || next > 0xDFFF) return false;
        } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
            return false;
        }
    }
    return true;
}

bool driveLetter(wchar_t character) {
    return (character >= L'A' && character <= L'Z') ||
           (character >= L'a' && character <= L'z');
}

bool equalInsensitive(const std::wstring& left, const wchar_t* right) {
    return CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()),
                                right, -1, TRUE) == CSTR_EQUAL;
}

bool filesystemSyntax(const std::wstring& value, bool absolute) {
    if (value.empty() || value.size() > 32767 || !validUtf16(value)) return false;
    if (value.front() == L'-' || value.find_first_of(L"\"<>|*?") != std::wstring::npos)
        return false;
    // Colons can only name a drive. This also excludes URLs and ADS streams.
    const auto colon = value.find(L':');
    if (colon != std::wstring::npos &&
        (colon != 1 || !driveLetter(value.front()) || value.find(L':', 2) != std::wstring::npos))
        return false;
    if (value.starts_with(L"\\??\\") || value.starts_with(L"\\Device\\")) return false;
    if (value.starts_with(L"\\\\")) {
        const auto serverEnd = value.find(L'\\', 2);
        if (serverEnd == std::wstring::npos || serverEnd == 2) return false;
        const auto shareEnd = value.find(L'\\', serverEnd + 1);
        const auto server = value.substr(2, serverEnd - 2);
        auto share = value.substr(serverEnd + 1,
            shareEnd == std::wstring::npos ? std::wstring::npos : shareEnd - serverEnd - 1);
        while (!share.empty() && (share.back() == L'.' || share.back() == L' ')) share.pop_back();
        if (server == L"." || server == L"?" || share.empty() ||
            equalInsensitive(share, L"pipe") || equalInsensitive(share, L"IPC$")) return false;
        return true;
    }
    return !absolute || (value.size() >= 3 && driveLetter(value[0]) &&
        value[1] == L':' && value[2] == L'\\');
}

bool hasDosDeviceComponent(const std::wstring& value) {
    std::size_t offset = value.starts_with(L"\\\\") ? value.find(L'\\', 2) : 0;
    if (value.starts_with(L"\\\\")) {
        // Server/share names are filesystem routing, not DOS filenames.
        offset = value.find(L'\\', offset + 1);
        if (offset == std::wstring::npos) return false;
    }
    while (offset < value.size()) {
        const auto end = value.find(L'\\', offset);
        auto part = value.substr(offset,
            end == std::wstring::npos ? std::wstring::npos : end - offset);
        // Only a whole component names a device; Windows drops its trailing
        // dots and spaces. "Con.Air.1997.mkv" is an ordinary file on Windows
        // 11 and on shares. Where older Windows still treats a device name
        // with an extension as the device, GetFullPathNameW has already
        // returned \\.\CON, which filesystemSyntax rejects.
        while (!part.empty() && (part.back() == L' ' || part.back() == L'.')) part.pop_back();
        if (equalInsensitive(part, L"CON") || equalInsensitive(part, L"PRN") ||
            equalInsensitive(part, L"AUX") || equalInsensitive(part, L"NUL") ||
            equalInsensitive(part, L"CONIN$") || equalInsensitive(part, L"CONOUT$")) return true;
        if (part.size() == 4 &&
            (equalInsensitive(part.substr(0, 3), L"COM") || equalInsensitive(part.substr(0, 3), L"LPT")) &&
            ((part[3] >= L'1' && part[3] <= L'9') ||
             part[3] == L'\u00B9' || part[3] == L'\u00B2' || part[3] == L'\u00B3')) return true;
        if (end == std::wstring::npos) break;
        offset = end + 1;
    }
    return false;
}

bool encodeRequest(const SingleInstance::Request& request, Bytes& bytes, std::wstring& error) {
    if (!nonzeroId(request.id) || request.files.size() > SingleInstance::kMaxFiles) {
        error = L"The launch request contains too many files or has no request identifier.";
        return false;
    }
    std::size_t size = SingleInstance::kRequestHeaderBytes;
    for (const auto& file : request.files) {
        if (!filesystemSyntax(file, true) || hasDosDeviceComponent(file)) {
            error = L"Only ordinary Windows filesystem paths can be opened from another launch.";
            return false;
        }
        size += 4 + file.size() * 2;
        if (size > SingleInstance::kMaxPayloadBytes) {
            error = L"The launch request is too large. Open fewer files at a time.";
            return false;
        }
    }
    bytes.clear();
    bytes.reserve(size);
    append32(bytes, SingleInstance::kRequestMagic);
    append32(bytes, SingleInstance::kProtocolVersion);
    append32(bytes, static_cast<std::uint32_t>(size));
    append32(bytes, static_cast<std::uint32_t>(request.files.size()));
    bytes.insert(bytes.end(), request.id.begin(), request.id.end());
    for (const auto& file : request.files) {
        append32(bytes, static_cast<std::uint32_t>(file.size()));
        for (const auto unit : file) {
            bytes.push_back(static_cast<std::uint8_t>(unit));
            bytes.push_back(static_cast<std::uint8_t>(static_cast<std::uint16_t>(unit) >> 8));
        }
    }
    return true;
}

bool decodeRequest(const Bytes& bytes, SingleInstance::Request& request) {
    if (bytes.size() < SingleInstance::kRequestHeaderBytes ||
        bytes.size() > SingleInstance::kMaxPayloadBytes ||
        read32(bytes, 0) != SingleInstance::kRequestMagic ||
        read32(bytes, 4) != SingleInstance::kProtocolVersion ||
        read32(bytes, 8) != bytes.size()) return false;
    const auto count = read32(bytes, 12);
    if (count > SingleInstance::kMaxFiles) return false;
    std::copy_n(bytes.begin() + 16, request.id.size(), request.id.begin());
    if (!nonzeroId(request.id)) return false;
    std::size_t offset = SingleInstance::kRequestHeaderBytes;
    request.files.clear();
    request.files.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        if (bytes.size() - offset < 4) return false;
        const auto units = read32(bytes, offset);
        offset += 4;
        if (units == 0 || units > 32767 || units > (bytes.size() - offset) / 2) return false;
        std::wstring path;
        path.reserve(units);
        for (std::uint32_t unit = 0; unit < units; ++unit) {
            path.push_back(static_cast<wchar_t>(bytes[offset] |
                (static_cast<unsigned>(bytes[offset + 1]) << 8)));
            offset += 2;
        }
        if (!filesystemSyntax(path, true)) return false;
        // Resolve lexical dots again at the receiving boundary. No filesystem
        // lookup, current-directory change, URL or arbitrary command execution.
        std::wstring normalized, error;
        if (!SingleInstance::normalizeLocalPath(path, normalized, error)) return false;
        request.files.push_back(std::move(normalized));
    }
    return offset == bytes.size();
}

bool digest(const Bytes& bytes, std::array<std::uint8_t, 32>& result) {
    HCRYPTPROV provider = 0;
    if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
        return false;
    HCRYPTHASH hash = 0;
    bool okay = CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash) != FALSE;
    if (okay) okay = CryptHashData(hash, bytes.data(), static_cast<DWORD>(bytes.size()), 0) != FALSE;
    DWORD size = static_cast<DWORD>(result.size());
    if (okay) okay = CryptGetHashParam(hash, HP_HASHVAL, result.data(), &size, 0) != FALSE &&
        size == result.size();
    if (hash) CryptDestroyHash(hash);
    CryptReleaseContext(provider, 0);
    return okay;
}

bool tokenInfo(HANDLE token, TOKEN_INFORMATION_CLASS kind, Bytes& bytes) {
    DWORD size = 0;
    GetTokenInformation(token, kind, nullptr, 0, &size);
    if (size == 0) return false;
    bytes.resize(size);
    return GetTokenInformation(token, kind, bytes.data(), size, &size) != FALSE;
}

bool sidString(PSID sid, std::wstring& result) {
    LPWSTR text = nullptr;
    if (!ConvertSidToStringSidW(sid, &text)) return false;
    result = text;
    LocalFree(text);
    return true;
}

DWORD remaining(Clock::time_point deadline) {
    if (deadline == Clock::time_point::max()) return INFINITE;
    const auto value = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
    if (value.count() <= 0) return 0;
    return static_cast<DWORD>(std::min<std::int64_t>(value.count(), MAXDWORD - 1));
}
} // namespace

struct SingleInstance::Impl {
    struct Replay {
        std::array<std::uint8_t, 32> hash{};
        std::size_t bytes{};
        bool pending{true};
        Clock::time_point completed{};
    };

    explicit Impl(std::wstring testSuffix = {}, bool isolated = false, HANDLE startGate = nullptr)
        : suffix(std::move(testSuffix)) {
        if (isolated && (suffix.empty() || suffix.size() > 96 ||
            !std::all_of(suffix.begin(), suffix.end(), [](wchar_t value) {
                return driveLetter(value) || (value >= L'0' && value <= L'9') ||
                    value == L'.' || value == L'_' || value == L'-';
            }))) constructionError = L"The isolated IPC namespace is invalid.";
        if (isolated) suffix.insert(0, L".test.");
        if (isolated && startGate && constructionError.empty()) {
            HANDLE duplicated = nullptr;
            if (!DuplicateHandle(GetCurrentProcess(), startGate, GetCurrentProcess(), &duplicated,
                                 SYNCHRONIZE, FALSE, 0))
                constructionError = L"The isolated IPC listener gate is invalid.";
            else listenerStartGate.reset(duplicated);
        }
    }

    ~Impl() {
        stopAccepting();
        if (ownsMutex) ReleaseMutex(mutex.get());
    }

    void stopAccepting() {
        {
            std::lock_guard guard(queueMutex);
            accepting = false;
        }
        if (stop) SetEvent(stop.get());
        if (pipe) CancelIoEx(pipe.get(), nullptr);
        if (worker.joinable()) worker.join();
        pipe.reset();
        window.store(nullptr);
    }

    bool initialize(std::wstring& error) {
        if (!constructionError.empty()) { error = constructionError; return false; }
        if (initialized) return true;
        if (mutexSecurity) { LocalFree(mutexSecurity); mutexSecurity = nullptr; }
        if (pipeSecurity) { LocalFree(pipeSecurity); pipeSecurity = nullptr; }
        Handle token;
        HANDLE rawToken = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) {
            error = L"QuadDeck could not read the current Windows identity.";
            return false;
        }
        token.reset(rawToken);
        Bytes user, groups;
        DWORD bytes = 0;
        if (!tokenInfo(token.get(), TokenUser, user) ||
            !tokenInfo(token.get(), TokenGroups, groups) ||
            !GetTokenInformation(token.get(), TokenSessionId, &session, sizeof(session), &bytes)) {
            error = L"QuadDeck could not determine the current Windows session.";
            return false;
        }
        const auto userSid = reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid;
        sid.resize(GetLengthSid(userSid));
        if (!CopySid(static_cast<DWORD>(sid.size()), sid.data(), userSid)) {
            error = L"QuadDeck could not copy the current Windows identity.";
            return false;
        }
        std::wstring userName, logonName;
        if (!sidString(userSid, userName)) {
            error = L"QuadDeck could not name the current Windows identity.";
            return false;
        }
        const auto tokenGroups = reinterpret_cast<const TOKEN_GROUPS*>(groups.data());
        for (DWORD index = 0; index < tokenGroups->GroupCount; ++index) {
            if ((tokenGroups->Groups[index].Attributes & SE_GROUP_LOGON_ID) == SE_GROUP_LOGON_ID) {
                if (!sidString(tokenGroups->Groups[index].Sid, logonName)) {
                    error = L"QuadDeck could not name the current Windows logon.";
                    return false;
                }
                break;
            }
        }
        if (logonName.empty()) {
            error = L"QuadDeck requires an interactive Windows logon session.";
            return false;
        }
        const auto name = L"QuadDeck.SingleInstance.v1." + userName + L"." +
            std::to_wstring(session) + suffix;
        mutexName = L"Local\\" + name;
        pipeName = L"\\\\.\\pipe\\" + name;
        // Only this logon can synchronize/modify the mutex. The pipe grants
        // data/attribute rights, deliberately excluding FILE_CREATE_PIPE_INSTANCE
        // (the same bit as FILE_APPEND_DATA / part of GENERIC_WRITE).
        const auto mutexAcl = L"D:P(A;;0x00100001;;;" + logonName + L")";
        const auto pipeAcl = L"D:P(A;;0x00100183;;;" + logonName + L")";
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                mutexAcl.c_str(), SDDL_REVISION_1, &mutexSecurity, nullptr) ||
            !ConvertStringSecurityDescriptorToSecurityDescriptorW(
                pipeAcl.c_str(), SDDL_REVISION_1, &pipeSecurity, nullptr)) {
            error = L"QuadDeck could not protect its single-instance objects.";
            return false;
        }
        stop.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!stop) { error = L"QuadDeck could not create its IPC shutdown event."; return false; }
        initialized = true;
        return true;
    }

    bool tokenMatches(HANDLE token) const {
        Bytes user;
        DWORD otherSession = MAXDWORD, bytes = 0;
        return tokenInfo(token, TokenUser, user) &&
            GetTokenInformation(token, TokenSessionId, &otherSession, sizeof(otherSession), &bytes) &&
            otherSession == session && EqualSid(const_cast<std::uint8_t*>(sid.data()),
                reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid);
    }

    bool verifyServer(HANDLE connection, DWORD& pid, Handle& process) const {
        ULONG processId = 0;
        if (!GetNamedPipeServerProcessId(connection, &processId) || processId == 0) return false;
        process.reset(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId));
        HANDLE rawToken = nullptr;
        if (!process || !OpenProcessToken(process.get(), TOKEN_QUERY, &rawToken)) return false;
        Handle token(rawToken);
        if (!tokenMatches(token.get())) return false;
        pid = processId;
        return true;
    }

    bool verifyClient(HANDLE connection) const {
        if (!ImpersonateNamedPipeClient(connection)) return false;
        HANDLE rawToken = nullptr;
        bool okay = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &rawToken) != FALSE;
        Handle token(rawToken);
        if (okay) okay = tokenMatches(token.get());
        // Never continue under a client's identity, even after validation fails.
        if (!RevertToSelf()) std::terminate();
        return okay;
    }

    bool waitIo(HANDLE file, OVERLAPPED& operation, Clock::time_point deadline,
                DWORD& transferred) const {
        const HANDLE events[] = {stop.get(), operation.hEvent};
        const auto wait = WaitForMultipleObjects(2, events, FALSE, remaining(deadline));
        if (wait == WAIT_OBJECT_0 + 1)
            return GetOverlappedResult(file, &operation, &transferred, FALSE) != FALSE;
        CancelIoEx(file, &operation);
        // The OVERLAPPED and its buffer stay alive until canceled I/O completes.
        GetOverlappedResult(file, &operation, &transferred, TRUE);
        return false;
    }

    bool transfer(HANDLE file, bool writing, std::uint8_t* buffer, DWORD size,
                  Clock::time_point deadline, DWORD& transferred) const {
        Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event || remaining(deadline) == 0 ||
            WaitForSingleObject(stop.get(), 0) != WAIT_TIMEOUT) return false;
        OVERLAPPED operation{};
        operation.hEvent = event.get();
        transferred = 0;
        const BOOL immediate = writing
            ? WriteFile(file, buffer, size, &transferred, &operation)
            : ReadFile(file, buffer, size, &transferred, &operation);
        if (immediate) return true;
        if (GetLastError() != ERROR_IO_PENDING) return false;
        return waitIo(file, operation, deadline, transferred);
    }

    bool acquireMutex(std::wstring& error) {
        if (!mutex) {
            SECURITY_ATTRIBUTES attributes{sizeof(attributes), mutexSecurity, FALSE};
            SetLastError(ERROR_SUCCESS);
            mutex.reset(CreateMutexExW(&attributes, mutexName.c_str(),
                                      CREATE_MUTEX_INITIAL_OWNER, SYNCHRONIZE | MUTEX_MODIFY_STATE));
            const auto creationError = GetLastError();
            if (!mutex) {
                error = L"QuadDeck could not access the single-instance mutex (Windows error " +
                    std::to_wstring(creationError) + L").";
                return false;
            }
            if (creationError != ERROR_ALREADY_EXISTS) {
                ownsMutex = true;
                electionThread = GetCurrentThreadId();
                return true;
            }
        }
        const auto wait = WaitForSingleObject(mutex.get(), 0);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
            ownsMutex = true;
            electionThread = GetCurrentThreadId();
            return true;
        }
        if (wait != WAIT_TIMEOUT) error = L"QuadDeck could not check the existing instance.";
        return false;
    }

    bool startPrimary(std::wstring& error) {
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), pipeSecurity, FALSE};
        pipe.reset(CreateNamedPipeW(pipeName.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, 4096, static_cast<DWORD>(SingleInstance::kMaxPayloadBytes), 0, &attributes));
        if (!pipe) {
            error = L"QuadDeck could not create its protected IPC pipe (Windows error " +
                std::to_wstring(GetLastError()) + L").";
            return false;
        }
        try {
            worker = std::thread([this] { serve(); });
        } catch (...) {
            pipe.reset();
            error = L"QuadDeck could not start its IPC listener.";
            return false;
        }
        primary = true;
        return true;
    }

    void pruneReplay(Clock::time_point now) {
        for (auto it = replay.begin(); it != replay.end();) {
            if (!it->second.pending && now - it->second.completed >= SingleInstance::kReplayRetention)
                it = replay.erase(it);
            else ++it;
        }
    }

    bool enqueue(Request&& request, const Bytes& bytes) {
        std::array<std::uint8_t, 32> hash{};
        if (!digest(bytes, hash)) return false;
        std::lock_guard guard(queueMutex);
        if (!accepting) return false;
        pruneReplay(Clock::now());
        if (const auto previous = replay.find(request.id); previous != replay.end())
            return previous->second.hash == hash;
        if (pendingCount >= SingleInstance::kMaxPendingRequests ||
            bytes.size() > SingleInstance::kMaxPendingBytes - pendingBytes ||
            replay.size() >= SingleInstance::kMaxReplayEntries) return false;
        const auto id = request.id;
        // Allocation failure must not ACK a batch which the UI cannot take.
        auto [entry, inserted] = replay.emplace(id, Replay{hash, bytes.size()});
        if (!inserted) return false;
        try { queue.push_back(std::move(request)); }
        catch (...) { replay.erase(entry); throw; }
        ++pendingCount;
        pendingBytes += bytes.size();
        return true;
    }

    void serveConnection() {
        Bytes bytes(SingleInstance::kMaxPayloadBytes);
        DWORD transferred = 0;
        const auto deadline = Clock::now() + kConnectionTimeout;
        if (!transfer(pipe.get(), false, bytes.data(), static_cast<DWORD>(bytes.size()),
                      deadline, transferred)) return;
        bytes.resize(transferred);
        if (!verifyClient(pipe.get())) return;
        Request request;
        bool accepted = decodeRequest(bytes, request);
        RequestId id{};
        if (bytes.size() >= SingleInstance::kRequestHeaderBytes)
            std::copy_n(bytes.begin() + 16, id.size(), id.begin());
        if (accepted) accepted = enqueue(std::move(request), bytes);
        Bytes reply;
        reply.reserve(SingleInstance::kReplyBytes);
        append32(reply, SingleInstance::kReplyMagic);
        append32(reply, SingleInstance::kProtocolVersion);
        append32(reply, accepted ? 0 : 1);
        append32(reply, GetCurrentProcessId());
        append64(reply, reinterpret_cast<std::uintptr_t>(window.load()));
        reply.insert(reply.end(), id.begin(), id.end());
        if (!transfer(pipe.get(), true, reply.data(), static_cast<DWORD>(reply.size()),
                      deadline, transferred) || transferred != reply.size()) return;
        // DisconnectNamedPipe discards unread replies. Wait for a one-byte
        // receipt (or disconnect/timeout) instead of unbounded FlushFileBuffers.
        std::uint8_t receipt = 0;
        transfer(pipe.get(), false, &receipt, 1, deadline, transferred);
    }

    void serve() noexcept {
        if (listenerStartGate) {
            const HANDLE events[] = {stop.get(), listenerStartGate.get()};
            if (WaitForMultipleObjects(2, events, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) return;
        }
        while (WaitForSingleObject(stop.get(), 0) == WAIT_TIMEOUT) {
            Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            if (!event) return;
            OVERLAPPED operation{};
            operation.hEvent = event.get();
            bool connected = ConnectNamedPipe(pipe.get(), &operation) != FALSE;
            DWORD connectError = connected ? ERROR_SUCCESS : GetLastError();
            if (!connected) {
                if (connectError == ERROR_PIPE_CONNECTED) connected = true;
                else if (connectError == ERROR_IO_PENDING) {
                    DWORD unused = 0;
                    // Listening has no activity deadline; shutdown always wakes it.
                    connected = waitIo(pipe.get(), operation, Clock::time_point::max(), unused);
                    if (!connected) connectError = GetLastError();
                }
            }
            if (!connected) {
                if (WaitForSingleObject(stop.get(), 0) != WAIT_TIMEOUT) return;
                // A client may open and close between pipe creation and this
                // connect, or while the overlapped connect completes. That is
                // one dropped connection, not failure of the elected listener.
                if (connectError == ERROR_NO_DATA || connectError == ERROR_BROKEN_PIPE ||
                    connectError == ERROR_PIPE_NOT_CONNECTED || connectError == ERROR_OPERATION_ABORTED) {
                    DisconnectNamedPipe(pipe.get());
                    continue;
                }
                return;
            }
            try { serveConnection(); }
            catch (...) { /* Never ACK a request after an allocation failure. */ }
            DisconnectNamedPipe(pipe.get());
        }
    }

    enum class ForwardResult { Accepted, Retry, Rejected };
    ForwardResult forward(const Request& request, Bytes& bytes, Clock::time_point deadline,
                          std::wstring& error) {
        Handle connection(CreateFileW(pipeName.c_str(), kPipeClientAccess, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
        if (!connection) return ForwardResult::Retry;
        DWORD serverPid = 0;
        // Hold the verified process handle through the exchange, so its PID
        // cannot be recycled before foreground permission or reply validation.
        Handle serverProcess;
        if (!verifyServer(connection.get(), serverPid, serverProcess)) {
            error = L"QuadDeck rejected an IPC endpoint outside the current user/session.";
            return ForwardResult::Rejected;
        }
        DWORD mode = PIPE_READMODE_MESSAGE;
        if (!SetNamedPipeHandleState(connection.get(), &mode, nullptr, nullptr)) {
            error = L"QuadDeck could not establish the IPC message protocol.";
            return ForwardResult::Rejected;
        }
        AllowSetForegroundWindow(serverPid);
        deadline = std::min(deadline, Clock::now() + kConnectionTimeout);
        DWORD transferred = 0;
        if (!transfer(connection.get(), true, bytes.data(), static_cast<DWORD>(bytes.size()),
                      deadline, transferred) || transferred != bytes.size()) return ForwardResult::Retry;
        Bytes reply(SingleInstance::kReplyBytes);
        if (!transfer(connection.get(), false, reply.data(), static_cast<DWORD>(reply.size()),
                      deadline, transferred) || transferred != reply.size()) return ForwardResult::Retry;
        RequestId replyId{};
        std::copy_n(reply.begin() + 24, replyId.size(), replyId.begin());
        const auto hwnd = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(read64(reply, 16)));
        DWORD windowPid = 0;
        if (hwnd) GetWindowThreadProcessId(hwnd, &windowPid);
        if (read32(reply, 0) != SingleInstance::kReplyMagic ||
            read32(reply, 4) != SingleInstance::kProtocolVersion ||
            read32(reply, 12) != serverPid || replyId != request.id ||
            (hwnd && windowPid != serverPid)) {
            error = L"QuadDeck received an invalid response from the existing instance.";
            return ForwardResult::Rejected;
        }
        std::uint8_t receipt = 0;
        transfer(connection.get(), true, &receipt, 1, deadline, transferred);
        if (read32(reply, 8) != 0) {
            error = L"The existing QuadDeck instance rejected the request; its pending queue may be full.";
            return ForwardResult::Rejected;
        }
        return ForwardResult::Accepted;
    }

    std::wstring suffix, constructionError, pipeName, mutexName;
    Bytes sid;
    DWORD session{}, electionThread{};
    PSECURITY_DESCRIPTOR mutexSecurity{}, pipeSecurity{};
    bool initialized{}, ownsMutex{}, primary{}, accepting{true};
    Handle mutex, pipe, stop, listenerStartGate;
    std::thread worker;
    std::atomic<HWND> window{};
    std::mutex queueMutex;
    std::deque<Request> queue;
    std::map<RequestId, Replay> replay;
    std::size_t pendingBytes{}, pendingCount{};
};

SingleInstance::SingleInstance() : impl_(std::make_unique<Impl>()) {}
SingleInstance::SingleInstance(IsolatedNamespace isolatedNamespace)
    : impl_(std::make_unique<Impl>(std::move(isolatedNamespace.suffix), true,
                                  isolatedNamespace.listenerStartGate)) {}
SingleInstance::~SingleInstance() {
    // Security descriptors were allocated with LocalAlloc by the SDDL helper.
    const auto mutexSecurity = impl_->mutexSecurity;
    const auto pipeSecurity = impl_->pipeSecurity;
    impl_.reset();
    if (mutexSecurity) LocalFree(mutexSecurity);
    if (pipeSecurity) LocalFree(pipeSecurity);
}

bool SingleInstance::normalizeLocalPath(const std::wstring& path,
                                       std::wstring& normalized, std::wstring& error) {
    std::wstring value = path;
    std::replace(value.begin(), value.end(), L'/', L'\\');
    if (!filesystemSyntax(value, false)) {
        error = L"Only Windows filesystem paths are accepted; URLs, options and device paths are unsupported.";
        return false;
    }
    const auto required = GetFullPathNameW(value.c_str(), 0, nullptr, nullptr);
    if (required == 0 || required > 32768) {
        error = L"QuadDeck could not resolve a launch filename.";
        return false;
    }
    std::wstring full(required, L'\0');
    const auto length = GetFullPathNameW(value.c_str(), required, full.data(), nullptr);
    if (length == 0 || length >= required) {
        error = L"QuadDeck could not resolve a launch filename.";
        return false;
    }
    full.resize(length);
    if (!filesystemSyntax(full, true) || hasDosDeviceComponent(full)) {
        error = L"The launch filename does not resolve to an ordinary drive or UNC path.";
        return false;
    }
    normalized = std::move(full);
    error.clear();
    return true;
}

bool SingleInstance::prepareRequest(const std::vector<std::wstring>& arguments,
                                   Request& request, std::wstring& error) {
    if (arguments.size() > kMaxFiles) {
        error = L"Too many launch files. Open at most " + std::to_wstring(kMaxFiles) + L" at a time.";
        return false;
    }
    Request prepared;
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) { error = L"QuadDeck could not identify the launch request."; return false; }
    static_assert(sizeof(guid) == sizeof(prepared.id));
    std::memcpy(prepared.id.data(), &guid, sizeof(guid));
    prepared.files.reserve(arguments.size());
    for (const auto& path : arguments) {
        std::wstring normalized;
        if (!normalizeLocalPath(path, normalized, error)) return false;
        prepared.files.push_back(std::move(normalized));
    }
    Bytes encoded;
    if (!encodeRequest(prepared, encoded, error)) return false;
    request = std::move(prepared);
    error.clear();
    return true;
}

SingleInstance::LaunchResult SingleInstance::launch(const Request& request, std::wstring& error,
                                                    std::chrono::milliseconds timeout) {
    error.clear();
    if (impl_->primary) return LaunchResult::Primary;
    Bytes bytes;
    if (!encodeRequest(request, bytes, error) || !impl_->initialize(error)) return LaunchResult::Error;
    // A caller can choose a shorter bound for an isolated test, never a retry
    // window longer than the replay cache's documented retention.
    timeout = std::clamp(timeout, std::chrono::milliseconds{1}, kDefaultTimeout);
    const auto deadline = Clock::now() + timeout;
    do {
        if (impl_->acquireMutex(error)) {
            if (impl_->startPrimary(error)) return LaunchResult::Primary;
            ReleaseMutex(impl_->mutex.get());
            impl_->ownsMutex = false;
            return LaunchResult::Error;
        }
        if (!error.empty()) return LaunchResult::Error;
        const auto outcome = impl_->forward(request, bytes, deadline, error);
        if (outcome == Impl::ForwardResult::Accepted) return LaunchResult::Forwarded;
        if (outcome == Impl::ForwardResult::Rejected) return LaunchResult::Error;
        const auto delay = std::min<DWORD>(remaining(deadline), 25);
        if (delay) WaitForSingleObject(impl_->stop.get(), delay);
    } while (remaining(deadline) != 0);
    error = L"QuadDeck is already running but did not accept this request in time. Try again after it finishes starting.";
    return LaunchResult::Error;
}

bool SingleInstance::isPrimary() const { return impl_->primary; }
void SingleInstance::setWindow(HWND window) { impl_->window.store(window); }
void SingleInstance::stopAccepting() { impl_->stopAccepting(); }
std::vector<SingleInstance::Request> SingleInstance::takeRequests(std::size_t maximum) {
    std::vector<Request> requests;
    maximum = std::min(maximum, kMaxPendingRequests);
    std::lock_guard guard(impl_->queueMutex);
    requests.reserve(std::min(maximum, impl_->queue.size()));
    while (!impl_->queue.empty() && requests.size() < maximum) {
        requests.push_back(std::move(impl_->queue.front()));
        impl_->queue.pop_front();
    }
    return requests;
}

void SingleInstance::completeRequest(const RequestId& id) {
    std::lock_guard guard(impl_->queueMutex);
    const auto entry = impl_->replay.find(id);
    if (entry == impl_->replay.end() || !entry->second.pending) return;
    entry->second.pending = false;
    entry->second.completed = Clock::now();
    --impl_->pendingCount;
    impl_->pendingBytes -= entry->second.bytes;
}

const std::wstring& SingleInstance::pipeName() const { return impl_->pipeName; }
const std::wstring& SingleInstance::mutexName() const { return impl_->mutexName; }

} // namespace quaddeck
