#include "single_instance_test_support.hpp"
#include "TextEncoding.hpp"

#include <shellapi.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <utility>

namespace quaddeck {
namespace {
using namespace std::chrono_literals;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class Handle {
public:
    explicit Handle(HANDLE value = nullptr) : value_(value) {}
    ~Handle() { if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return value_; }
private:
    HANDLE value_;
};

SingleInstance::Request requestFor(const std::vector<std::wstring>& files) {
    SingleInstance::Request request;
    std::wstring error;
    check(SingleInstance::prepareRequest(files, request, error), "Cannot prepare isolated test request");
    return request;
}

SingleInstance::LaunchResult launchFromWorker(const std::wstring& name, const SingleInstance::Request& request,
                                             std::chrono::milliseconds timeout = 2000ms) {
    // A Windows mutex is recursively acquired by its owning thread. A
    // distinct worker here models another launcher; process scenarios below
    // use actual distinct executables, including all player-window assertions.
    return std::async(std::launch::async, [name, request, timeout] {
        SingleInstance sender(SingleInstance::IsolatedNamespace{name});
        std::wstring error;
        return sender.launch(request, error, timeout);
    }).get();
}

std::wstring isolatedName() {
    const auto id = requestFor({}).id;
    constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring name = L"regression-" + std::to_wstring(GetCurrentProcessId()) + L"-";
    for (const auto byte : id) { name += digits[byte >> 4]; name += digits[byte & 15]; }
    return name;
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        std::wstring path(32768, L'\0');
        const DWORD length = GetTempPathW(static_cast<DWORD>(path.size()), path.data());
        check(length && length < path.size(), "Cannot locate isolated test temporary directory");
        path.resize(length);
        directory = std::filesystem::path(path) / isolatedName();
        check(std::filesystem::create_directory(directory), "Cannot create isolated test directory");
    }
    ~TemporaryDirectory() {
        // directory is a fresh, absolute, GUID-named child of GetTempPathW.
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
    std::filesystem::path directory;
};

std::wstring quoteArgument(const std::wstring& value) {
    std::wstring quoted = L"\"";
    std::size_t slashes = 0;
    for (const wchar_t character : value) {
        if (character == L'\\') { ++slashes; continue; }
        quoted.append(character == L'\"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        quoted += character;
    }
    quoted.append(slashes * 2, L'\\');
    return quoted + L'\"';
}

std::wstring executablePath() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    check(length && length < path.size(), "Cannot find regression executable");
    path.resize(length);
    return path;
}

class ChildProcess {
public:
    explicit ChildProcess(const std::vector<std::wstring>& arguments) {
        const auto executable = executablePath();
        std::wstring command = quoteArgument(executable);
        for (const auto& argument : arguments) command += L" " + quoteArgument(argument);
        STARTUPINFOW startup{sizeof(startup)};
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        check(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process_) != FALSE,
            "Cannot start isolated regression child");
        CloseHandle(process_.hThread);
        process_.hThread = nullptr;
    }
    ~ChildProcess() {
        if (!process_.hProcess) return;
        if (WaitForSingleObject(process_.hProcess, 0) == WAIT_TIMEOUT) {
            // Only this exact CreateProcess handle can be terminated. First
            // request graceful shutdown; a bounded timeout is the sole kill.
            EnumWindows([](HWND window, LPARAM context) -> BOOL {
                DWORD pid = 0;
                GetWindowThreadProcessId(window, &pid);
                if (pid == static_cast<DWORD>(context))
                    PostMessageW(window, kSingleInstanceTestControl,
                        static_cast<WPARAM>(SingleInstanceTestControl::Stop), 0);
                return TRUE;
            }, static_cast<LPARAM>(process_.dwProcessId));
            if (WaitForSingleObject(process_.hProcess, 1500) == WAIT_TIMEOUT) {
                TerminateProcess(process_.hProcess, 99);
                WaitForSingleObject(process_.hProcess, 1500);
            }
        }
        CloseHandle(process_.hProcess);
    }
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    bool running() const { return WaitForSingleObject(process_.hProcess, 0) == WAIT_TIMEOUT; }
    DWORD exitCode() const {
        DWORD status = 99;
        GetExitCodeProcess(process_.hProcess, &status);
        return status;
    }
    void requireSuccess(DWORD timeout = 10000) const {
        requireExitCode(0, timeout);
    }
    void requireExitCode(DWORD expected, DWORD timeout = 10000) const {
        check(WaitForSingleObject(process_.hProcess, timeout) == WAIT_OBJECT_0, "Regression child timed out");
        DWORD status = 99;
        check(GetExitCodeProcess(process_.hProcess, &status) && status == expected, "Regression child failed");
    }
    DWORD pid() const { return process_.dwProcessId; }
private:
    PROCESS_INFORMATION process_{};
};

std::vector<std::wstring> childArguments(const std::wstring& name, const std::filesystem::path& directory,
                                        const std::vector<std::wstring>& files,
                                        const std::wstring& gate = {}) {
    std::vector<std::wstring> arguments{L"--si-child", name, directory.wstring(), gate};
    arguments.insert(arguments.end(), files.begin(), files.end());
    return arguments;
}

nlohmann::json readState(const std::filesystem::path& directory) {
    Handle file(CreateFileW((directory / L"state.json").c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(file.get(), &length) || length.QuadPart <= 0 || length.QuadPart > 1024 * 1024) return {};
    std::string contents(static_cast<std::size_t>(length.QuadPart), '\0');
    DWORD bytes = 0;
    if (!ReadFile(file.get(), contents.data(), static_cast<DWORD>(contents.size()), &bytes, nullptr) || bytes != contents.size()) return {};
    try { return nlohmann::json::parse(contents); }
    catch (const nlohmann::json::exception&) { return {}; }
}

nlohmann::json childLaunchResult(const std::filesystem::path& directory, DWORD pid) {
    std::ifstream file(directory / (L"child-" + std::to_wstring(pid) + L"-launch.json"), std::ios::binary);
    check(static_cast<bool>(file), "Child did not record its actual launch classification");
    return nlohmann::json::parse(file);
}

template<class Predicate>
nlohmann::json awaitState(const std::filesystem::path& directory, Predicate predicate, DWORD timeout = 10000) {
    const ULONGLONG deadline = GetTickCount64() + timeout;
    do {
        const auto state = readState(directory);
        if (state.contains("failure")) throw std::runtime_error(state.at("failure").get<std::string>());
        if (!state.empty() && predicate(state)) return state;
        Sleep(10);
    } while (GetTickCount64() < deadline);
    const auto state = readState(directory);
    throw std::runtime_error("Timed out waiting for isolated receiver state: " + state.dump());
}

HWND exactlyOneReceiver(const std::wstring& name, const std::set<DWORD>& allowedPids, const char* stage) {
    struct Search {
        const std::set<DWORD>& pids;
        HWND found{};
        unsigned count{};
        bool unsafe{};
        nlohmann::json ownedWindows{nlohmann::json::array()};
    } search{allowedPids};
    SetLastError(ERROR_SUCCESS);
    const BOOL enumerated = EnumWindows([](HWND window, LPARAM context) -> BOOL {
        auto& result = *reinterpret_cast<Search*>(context);
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (!result.pids.contains(pid)) return TRUE;
        wchar_t className[128]{};
        GetClassNameW(window, className, static_cast<int>(std::size(className)));
        result.ownedWindows.push_back(nlohmann::json{{"pid", pid}, {"class", wideToUtf8Text(className)},
            {"visible", IsWindowVisible(window) != FALSE}, {"hwnd", reinterpret_cast<std::uintptr_t>(window)}});
        if (std::wstring(className) != kSingleInstanceTestWindowClass) return TRUE;
        ++result.count;
        result.found = window;
        result.unsafe |= IsWindowVisible(window) != FALSE;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    const DWORD enumerationError = GetLastError();
    if (search.count != 1 || search.unsafe) {
        throw std::runtime_error(std::string(stage) + ": expected exactly one hidden receiving player harness window; " +
            "count=" + std::to_string(search.count) + ", visible=" + std::to_string(search.unsafe) +
            ", EnumWindows=" + std::to_string(enumerated) + ", error=" + std::to_string(enumerationError) +
            ", namespace=" + wideToUtf8Text(name) + ", ownedWindows=" + search.ownedWindows.dump());
    }
    return search.found;
}

void control(HWND window, SingleInstanceTestControl command, unsigned serial) {
    check(PostMessageW(window, kSingleInstanceTestControl, static_cast<WPARAM>(command), serial) != FALSE,
          "Cannot command isolated receiving child");
}

void sendChild(const std::wstring& name, const std::filesystem::path& directory,
               const std::vector<std::wstring>& files) {
    ChildProcess sender(childArguments(name, directory, files));
    sender.requireSuccess();
}

std::vector<std::wstring> syntheticFiles(const std::filesystem::path& directory) {
    std::vector<std::wstring> files;
    for (unsigned index = 0; index < 14; ++index) {
        const auto file = directory / (L"Synthetic " + std::to_wstring(index) + L" \x65e5\x672c\x8a9e \xd83c\xdfac.mp4");
        std::ofstream output(file, std::ios::binary);
        output << "Synthetic placeholder: no decoder ever opens this file.\n";
        check(static_cast<bool>(output), "Cannot create isolated synthetic placeholder");
        files.push_back(file.wstring());
    }
    return files;
}

void processAddAndHandover(const std::filesystem::path& directory, const std::vector<std::wstring>& files) {
    const auto name = isolatedName();
    ChildProcess primary(childArguments(name, directory, {files[0]}));
    std::set<DWORD> pids{primary.pid()};
    auto state = awaitState(directory, [](const auto& value) { return value.value("panes", 0) == 1; });
    HWND window = exactlyOneReceiver(name, pids, "initial primary");
    check(state.value("audioMask", 0U) == 1 && state.value("preserved", false), "Fresh startup did not retain its first audible pane");

    sendChild(name, directory, {files[1]});
    state = awaitState(directory, [](const auto& value) { return value.value("received", 0) == 1 && value.value("panes", 0) == 2; });
    check(state.value("preserved", false) && state["paths"][1] == wideToUtf8Text(files[1]) &&
          state.value("audioMask", 0U) == 1 && state["addedMuted"][1].get<bool>() &&
          state["sourceTimes"][1].get<double>() == 0.0 && !state.value("playing", true),
          "Second process open did not Add muted at zero while preserving first serial, timeline, pause, audio and queue");
    exactlyOneReceiver(name, pids, "second process Add");
    const auto serials = state.at("serials");

    // Spelling aliases and duplicates are forwarded intact, then focused by
    // App without reopening any source or consuming another pane.
    const auto alias = (std::filesystem::path(files[1]).parent_path() / L"." / std::filesystem::path(files[1]).filename()).wstring();
    sendChild(name, directory, {alias, files[1], files[0]});
    state = awaitState(directory, [](const auto& value) { return value.value("received", 0) == 2; });
    check(state.value("panes", 0) == 2 && state.at("serials") == serials && state.value("preserved", false),
          "Duplicate Explorer opens restarted media or consumed panes");
    sendChild(name, directory, {});
    state = awaitState(directory, [](const auto& value) { return value.value("received", 0) == 3; });
    check(state.value("panes", 0) == 2 && state.value("activations", 0) >= 3, "No-file launch did not activate the existing receiver");

    control(window, SingleInstanceTestControl::Minimize, 1);
    awaitState(directory, [](const auto& value) { return value.value("command", 0) == 1 && value.value("iconic", false); });
    sendChild(name, directory, {files[1]});
    state = awaitState(directory, [](const auto& value) { return value.value("received", 0) == 4; });
    check(!state.value("iconic", true) && !state.value("visible", true) && state.value("preserved", false),
          "Forwarded open failed the isolated minimized-window activation seam");

    sendChild(name, directory, {files[2], files[3], files[4], files[5], files[6]});
    state = awaitState(directory, [](const auto& value) { return value.value("received", 0) == 5 && value.value("pending", false); });
    check(state.value("panes", 0) == 5 && state.at("pendingPath") == wideToUtf8Text(files[5]) && state.value("preserved", false),
          "Multi-file open silently overwrote a full deck or lost request order");
    const auto fullPaths = state.at("paths");
    sendChild(name, directory, {files[7]});
    awaitState(directory, [](const auto& value) { return value.value("received", 0) == 6; });
    control(window, SingleInstanceTestControl::Cancel, 2);
    state = awaitState(directory, [&](const auto& value) {
        return value.value("command", 0) == 2 && value.value("pendingPath", std::string()) == wideToUtf8Text(files[7]);
    });
    check(state.at("paths") == fullPaths && state.value("preserved", false),
          "Cancel changed playback, kept canceled batch remainder, or dropped a later independent batch");
    control(window, SingleInstanceTestControl::ReplaceLast, 3);
    state = awaitState(directory, [](const auto& value) { return value.value("command", 0) == 3 && !value.value("pending", true); });
    check(state["paths"][4] == wideToUtf8Text(files[7]) && state.value("preserved", false) &&
          state.value("queued", 99) == 0, "Explicit replacement did not finish only the later accepted batch");
    exactlyOneReceiver(name, pids, "full-deck explicit replacement");
    control(window, SingleInstanceTestControl::StopAccepting, 4);
    awaitState(directory, [](const auto& value) { return value.value("command", 0) == 4; });
    const ULONGLONG unavailableStarted = GetTickCount64();
    {
        ChildProcess unavailable(childArguments(name, directory, {files[9]}));
        unavailable.requireExitCode(1);
        const auto launch = childLaunchResult(directory, unavailable.pid());
        check(launch.value("result", std::string()) == "Error",
              "Unavailable-secondary exit code came from a child exception instead of transport rejection");
    }
    DWORD survivingPid = 0;
    GetWindowThreadProcessId(window, &survivingPid);
    std::cout << "Stopped-acceptance diagnostic: primaryRunning=" << primary.running()
        << ", knownWindow=" << (IsWindow(window) != FALSE) << ", windowPid=" << survivingPid
        << ", primaryPid=" << primary.pid() << ", primaryExitCode=" << primary.exitCode()
        << ", secondaryElapsedMs=" << GetTickCount64() - unavailableStarted << '\n';
    if (!primary.running()) std::cout << "Primary last state: " << readState(directory).dump() << '\n';
    {
        std::ifstream diagnostic(directory / (L"child-" + std::to_wstring(primary.pid()) + L"-error.log"), std::ios::binary);
        if (diagnostic) std::cout << "Primary error: " << std::string(std::istreambuf_iterator<char>(diagnostic), {}) << '\n';
    }
    check(primary.running() && IsWindow(window) && survivingPid == primary.pid(),
          "Stopping acceptance unexpectedly exited the primary child or destroyed its known owned HWND");
    exactlyOneReceiver(name, pids, "alive receiver stopped accepting");
    control(window, SingleInstanceTestControl::Stop, 5);
    primary.requireSuccess();

    std::filesystem::remove(directory / L"state.json");
    ChildProcess successor(childArguments(name, directory, {files[8]}));
    pids.insert(successor.pid());
    state = awaitState(directory, [](const auto& value) { return value.value("panes", 0) == 1; });
    window = exactlyOneReceiver(name, pids, "graceful successor");
    check(state["paths"][0] == wideToUtf8Text(files[8]), "Graceful primary handover lost the successor startup file");
    // This secondary retains an open handle to the elected mutex across the
    // owner's abrupt process exit. The kernel object therefore remains alive
    // and the next real child must recover its abandoned ownership.
    SingleInstance observer(SingleInstance::IsolatedNamespace{name});
    std::wstring error;
    check(observer.launch(requestFor({}), error) == SingleInstance::LaunchResult::Forwarded,
          "Cannot retain mutex observer for abandoned-owner test");
    awaitState(directory, [](const auto& value) { return value.value("received", 0) == 1; });
    control(window, SingleInstanceTestControl::AbruptExit, 1);
    successor.requireSuccess();
    std::filesystem::remove(directory / L"state.json");
    ChildProcess recovered(childArguments(name, directory, {files[10]}));
    pids.insert(recovered.pid());
    state = awaitState(directory, [](const auto& value) { return value.value("panes", 0) == 1; });
    window = exactlyOneReceiver(name, pids, "abandoned owner successor");
    check(state["paths"][0] == wideToUtf8Text(files[10]), "Abandoned mutex recovery lost successor startup media");
    control(window, SingleInstanceTestControl::Stop, 1);
    recovered.requireSuccess();
}

void simultaneousLaunches(const std::filesystem::path& directory, const std::vector<std::wstring>& files) {
    const auto name = isolatedName();
    const std::wstring gateName = L"Local\\QuadDeck.SingleInstance.TestGate." + name;
    Handle gate(CreateEventW(nullptr, TRUE, FALSE, gateName.c_str()));
    check(gate.get() != nullptr, "Cannot create isolated launch barrier");
    std::vector<std::unique_ptr<ChildProcess>> children;
    std::set<DWORD> pids;
    for (unsigned index = 0; index < 6; ++index) {
        children.push_back(std::make_unique<ChildProcess>(childArguments(name, directory, {files[index]}, gateName)));
        pids.insert(children.back()->pid());
    }
    check(SetEvent(gate.get()) != FALSE, "Cannot release simultaneous launch barrier");
    const auto state = awaitState(directory, [](const auto& value) { return value.value("received", 0) == 5; }, 15000);
    const HWND window = exactlyOneReceiver(name, pids, "six simultaneous launches");
    std::set<std::string> delivered;
    for (const auto& file : state.at("receivedFiles")) delivered.insert(file.get<std::string>());
    for (unsigned index = 0; index < 6; ++index)
        check(delivered.contains(wideToUtf8Text(files[index])), "A simultaneous process launch lost its startup batch");
    check(delivered.size() == 6 && state.value("panes", 0) == 5 && state.value("pending", false),
          "Simultaneous launches spawned extra receivers or skipped full-deck confirmation");
    control(window, SingleInstanceTestControl::Stop, 1);
    for (const auto& child : children) child->requireSuccess();
    // Acceptance is queued before a secondary writes its classification.
    // Joining every child ensures these per-PID files are complete/closed.
    unsigned elected = 0, forwarded = 0;
    for (const auto& child : children) {
        const auto result = childLaunchResult(directory, child->pid()).value("result", std::string());
        elected += result == "Primary";
        forwarded += result == "Forwarded";
    }
    check(elected == 1 && forwarded == 5, "Simultaneous processes did not classify exactly one primary and five forwarded launches");
}

void append32(std::vector<std::uint8_t>& data, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) data.push_back(static_cast<std::uint8_t>(value >> shift));
}

void set32(std::vector<std::uint8_t>& data, std::size_t offset, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) data.at(offset++) = static_cast<std::uint8_t>(value >> shift);
}

std::vector<std::uint8_t> payload(const SingleInstance::Request& request) {
    std::vector<std::uint8_t> bytes;
    append32(bytes, SingleInstance::kRequestMagic);
    append32(bytes, SingleInstance::kProtocolVersion);
    append32(bytes, 0);
    append32(bytes, static_cast<std::uint32_t>(request.files.size()));
    bytes.insert(bytes.end(), request.id.begin(), request.id.end());
    for (const auto& file : request.files) {
        append32(bytes, static_cast<std::uint32_t>(file.size()));
        for (const wchar_t unit : file) {
            bytes.push_back(static_cast<std::uint8_t>(unit));
            bytes.push_back(static_cast<std::uint8_t>(unit >> 8));
        }
    }
    set32(bytes, 8, static_cast<std::uint32_t>(bytes.size()));
    return bytes;
}

void rawMessage(const std::wstring& pipeName, const std::vector<std::uint8_t>& bytes, bool readReply = true) {
    check(WaitNamedPipeW(pipeName.c_str(), 2000) != FALSE, "Isolated pipe did not become available");
    Handle pipe(CreateFileW(pipeName.c_str(), FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES |
        FILE_WRITE_ATTRIBUTES | SYNCHRONIZE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
    check(pipe.get() != INVALID_HANDLE_VALUE, "Cannot connect malformed test client");
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    OVERLAPPED operation{};
    operation.hEvent = event.get();
    DWORD transferred = 0;
    const BOOL written = WriteFile(pipe.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &transferred, &operation);
    if (!written && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(event.get(), 2000) != WAIT_OBJECT_0) CancelIoEx(pipe.get(), &operation);
        if (!GetOverlappedResult(pipe.get(), &operation, &transferred, TRUE)) {
            const DWORD error = GetLastError();
            if (readReply && (error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA)) return;
            check(false, "Raw test write failed");
        }
    } else if (!written) {
        const DWORD error = GetLastError();
        if (readReply && (error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA)) return;
        check(false, "Raw test write failed");
    }
    std::array<std::uint8_t, SingleInstance::kReplyBytes> reply{};
    ResetEvent(event.get());
    operation = {};
    operation.hEvent = event.get();
    transferred = 0;
    const BOOL read = ReadFile(pipe.get(), reply.data(), static_cast<DWORD>(reply.size()), &transferred, &operation);
    if (!read && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(event.get(), 2000) != WAIT_OBJECT_0) CancelIoEx(pipe.get(), &operation);
        GetOverlappedResult(pipe.get(), &operation, &transferred, TRUE);
    }
    if (!readReply) {
        // The ACK is deliberately discarded and no receipt is written. Its
        // full receipt here proves server acceptance before the retry, rather
        // than accidentally testing a disconnect before authentication.
        check(transferred == reply.size() && reply[8] == 0,
              "Uncertain-ACK fixture was not accepted before disconnect");
    }
    // Invalid clients may receive a rejection or be disconnected. The queue,
    // then a valid request, establishes rejection and continued availability.
}

void listenerSurvivesEarlyDisconnect(const std::vector<std::wstring>& files) {
    const auto name = isolatedName();
    Handle gate(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    check(gate.get(), "Cannot create isolated listener gate");
    SingleInstance primary(SingleInstance::IsolatedNamespace{name, gate.get()});
    std::wstring error;
    check(primary.launch(requestFor({}), error) == SingleInstance::LaunchResult::Primary,
          "Cannot start gated isolated primary");
    {
        // The worker has not called ConnectNamedPipe. Closing this client
        // forces ERROR_NO_DATA when the gate releases, deterministically.
        Handle early(CreateFileW(primary.pipeName().c_str(), FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES |
            FILE_WRITE_ATTRIBUTES | SYNCHRONIZE, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
        check(early.get() != INVALID_HANDLE_VALUE, "Cannot connect before gated listener starts");
    }
    check(SetEvent(gate.get()), "Cannot release isolated listener gate");
    const auto request = requestFor({files[0]});
    check(launchFromWorker(name, request) == SingleInstance::LaunchResult::Forwarded,
          "Connect-before-listen disconnect permanently stopped IPC acceptance");
    const auto received = primary.takeRequests();
    check(received.size() == 1 && received[0].id == request.id,
          "Recovered listener lost the request after early disconnect");
    primary.completeRequest(request.id);

    Handle blockedGate(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    const auto blockedName = isolatedName();
    SingleInstance blocked(SingleInstance::IsolatedNamespace{blockedName, blockedGate.get()});
    check(blocked.launch(requestFor({}), error) == SingleInstance::LaunchResult::Primary,
          "Cannot start shutdown-gated isolated primary");
    const auto started = std::chrono::steady_clock::now();
    blocked.stopAccepting();
    check(std::chrono::steady_clock::now() - started < 1000ms,
          "Shutdown did not cancel a gated listener promptly");
}

void transportBoundsAndReplay(const std::vector<std::wstring>& files) {
    const auto name = isolatedName();
    SingleInstance primary(SingleInstance::IsolatedNamespace{name});
    std::wstring error;
    check(primary.launch(requestFor({}), error) == SingleInstance::LaunchResult::Primary, "Cannot elect isolated transport primary");
    check(primary.takeRequests().empty(), "Primary startup batch leaked into forwarded queue");
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        check(WaitNamedPipeW(primary.pipeName().c_str(), 2000), "Early-disconnect stress stopped the pipe listener");
        Handle early(CreateFileW(primary.pipeName().c_str(), FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES |
            FILE_WRITE_ATTRIBUTES | SYNCHRONIZE, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
        check(early.get() != INVALID_HANDLE_VALUE, "Early-disconnect stress could not connect");
        // Intentionally close without writing. A server must remain available
        // even when EOF precedes ConnectNamedPipe or the first payload read.
    }
    const auto request = requestFor({files[0], files[1]});
    check(launchFromWorker(name, request) == SingleInstance::LaunchResult::Forwarded, "Transport rejected a valid multi-file request");
    check(launchFromWorker(name, request) == SingleInstance::LaunchResult::Forwarded, "Accepted request ID was not replayable");
    const auto received = primary.takeRequests();
    check(received.size() == 1 && received[0].id == request.id && received[0].files == request.files,
          "Retry delivered duplicate or reordered UTF-16 batches");
    primary.completeRequest(request.id);
    check(launchFromWorker(name, request) == SingleInstance::LaunchResult::Forwarded, "Completed request replay was not acknowledged");
    check(primary.takeRequests().empty(), "Completed request replay opened files again");
    auto changedReplay = request;
    changedReplay.files = {files[2]};
    check(launchFromWorker(name, changedReplay) == SingleInstance::LaunchResult::Error && primary.takeRequests().empty(),
          "Replay ID with changed media was accepted");

    const auto validBytes = payload(requestFor({files[2]}));
    std::vector<std::vector<std::uint8_t>> malformed;
    auto bytes = validBytes;
    set32(bytes, 0, 0); malformed.push_back(bytes);
    bytes = validBytes; set32(bytes, 4, 99); malformed.push_back(bytes);
    bytes = validBytes; set32(bytes, 8, static_cast<std::uint32_t>(bytes.size() + 2)); malformed.push_back(bytes);
    bytes = validBytes; set32(bytes, 12, SingleInstance::kMaxFiles + 1); malformed.push_back(bytes);
    bytes = validBytes; bytes.push_back(42); set32(bytes, 8, static_cast<std::uint32_t>(bytes.size())); malformed.push_back(bytes);
    bytes = validBytes; set32(bytes, 32, 0x7fffffff); malformed.push_back(bytes);
    bytes = validBytes; bytes[36] = bytes[37] = 0; malformed.push_back(bytes);
    bytes = validBytes; bytes[36] = 0x00; bytes[37] = 0xdc; malformed.push_back(bytes);
    bytes = validBytes; std::fill(bytes.begin() + 16, bytes.begin() + 32, 0); malformed.push_back(bytes);
    bytes = validBytes; bytes.resize(12); malformed.push_back(bytes);
    bytes = validBytes; bytes.resize(SingleInstance::kMaxPayloadBytes + 1, 42);
    set32(bytes, 8, static_cast<std::uint32_t>(bytes.size())); malformed.push_back(bytes);
    for (const auto& invalid : malformed) {
        rawMessage(primary.pipeName(), invalid);
        check(primary.takeRequests().empty(), "Malformed IPC payload reached App intake");
    }
    const auto uncertain = requestFor({files[3]});
    rawMessage(primary.pipeName(), payload(uncertain), false);
    check(launchFromWorker(name, uncertain) == SingleInstance::LaunchResult::Forwarded, "Lost-ACK request could not be retried");
    const auto exactlyOnce = primary.takeRequests();
    check(exactlyOnce.size() == 1 && exactlyOnce[0].id == uncertain.id,
          "Lost acknowledgement retry applied a batch twice");
    primary.completeRequest(uncertain.id);

    std::vector<SingleInstance::RequestId> reservations;
    for (std::size_t index = 0; index < SingleInstance::kMaxPendingRequests; ++index) {
        const auto held = requestFor({files[index % files.size()]});
        check(launchFromWorker(name, held, 1000ms) == SingleInstance::LaunchResult::Forwarded, "Pending request capacity filled early");
        reservations.push_back(held.id);
    }
    // Taking from the transport is not completion: App's chooser can hold
    // requests indefinitely, so their reservations must remain bounded.
    check(primary.takeRequests(SingleInstance::kMaxPendingRequests).size() == reservations.size(), "Pending FIFO lost a reserved request");
    check(launchFromWorker(name, requestFor({files[0]}), 250ms) == SingleInstance::LaunchResult::Error,
          "Full intake silently accepted an unreserved request");
    check(primary.takeRequests().empty(), "Rejected over-capacity request reached App");
    for (const auto& id : reservations) primary.completeRequest(id);
    {
        const auto released = requestFor({files[0]});
        check(launchFromWorker(name, released, 1000ms) == SingleInstance::LaunchResult::Forwarded, "Completed reservations did not restore intake capacity");
        primary.completeRequest(released.id);
    }

    SingleInstance::Request invalid;
    check(!SingleInstance::prepareRequest({L"https://example.invalid/video.mp4"}, invalid, error), "IPC accepted a network protocol as a local file");
    check(!SingleInstance::prepareRequest({std::wstring(L"C:\\nul") + L'\0' + L".mp4"}, invalid, error), "IPC accepted an embedded path NUL");
    check(!SingleInstance::prepareRequest(std::vector<std::wstring>(SingleInstance::kMaxFiles + 1, files[0]), invalid, error), "IPC accepted too many files");
    std::wstring normalized;
    check(SingleInstance::normalizeLocalPath(L"\\\\synthetic.invalid\\share\\Unicode \x65e5.mp4", normalized, error) &&
          normalized.starts_with(L"\\\\synthetic.invalid\\share\\"), "Lexical UNC normalization attempted or rejected network access");
    // Real titles begin with a device name and a dot. A share never maps
    // them to a device; a local drive defers to whether this Windows does.
    check(SingleInstance::normalizeLocalPath(L"\\\\synthetic.invalid\\media\\Aux.Armes.2019\\Con.Air.1997.mkv",
                                             normalized, error), "A share title beginning AUX./CON. was refused");
    for (const wchar_t* title : {L"C:\\QuadDeckSynthetic\\Con.Air.1997.1080p.mkv",
                                 L"C:\\QuadDeckSynthetic\\Nul.Points.2019\\video.mkv",
                                 L"C:\\QuadDeckSynthetic\\COM1.Story.mkv"}) {
        wchar_t resolved[MAX_PATH]{};
        check(GetFullPathNameW(title, MAX_PATH, resolved, nullptr) > 0, "Cannot resolve a device-name title");
        const bool deviceHere = std::wstring_view(resolved).starts_with(L"\\\\.\\");
        check(SingleInstance::normalizeLocalPath(title, normalized, error) == !deviceHere,
              "A title beginning with a device name was not judged the way this Windows resolves it");
    }
    for (const wchar_t* device : {L"C:\\QuadDeckSynthetic\\CON", L"C:\\QuadDeckSynthetic\\nul",
                                  L"C:\\QuadDeckSynthetic\\COM1.", L"C:\\QuadDeckSynthetic\\aux \\video.mkv",
                                  L"\\\\synthetic.invalid\\share\\PRN"}) {
        check(!SingleInstance::normalizeLocalPath(device, normalized, error), "A whole device-name component was accepted");
    }
    check(WaitNamedPipeW(primary.pipeName().c_str(), 2000), "Pipe not ready for pending-connection shutdown");
    Handle stalled(CreateFileW(primary.pipeName().c_str(), FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES |
        FILE_WRITE_ATTRIBUTES | SYNCHRONIZE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
    check(stalled.get() != INVALID_HANDLE_VALUE, "Cannot open stalled raw client");
    const auto shutdownStarted = std::chrono::steady_clock::now();
    primary.stopAccepting();
    check(std::chrono::steady_clock::now() - shutdownStarted < 1000ms,
          "Stopping acceptance did not cancel a pending pipe read promptly");
    check(primary.isPrimary() && launchFromWorker(name, requestFor({files[1]}), 250ms) == SingleInstance::LaunchResult::Error,
          "Stopped receiver ACKed a request or relinquished election before its App exited");
}

int processTests() {
    TemporaryDirectory temporary;
    const auto files = syntheticFiles(temporary.directory);
    const auto normal = temporary.directory / L"normal";
    const auto race = temporary.directory / L"race";
    std::filesystem::create_directory(normal);
    std::filesystem::create_directory(race);
    listenerSurvivesEarlyDisconnect(files);
    transportBoundsAndReplay(files);
    processAddAndHandover(normal, files);
    simultaneousLaunches(race, files);
    for (const auto& directory : {normal, race}) {
        for (const auto& file : std::filesystem::directory_iterator(directory)) {
            if (!file.path().filename().wstring().starts_with(L"publish-") || file.path().extension() != L".json") continue;
            std::ifstream record(file.path(), std::ios::binary);
            std::cout << "Observed snapshot publication retry: " << nlohmann::json::parse(record).dump() << '\n';
        }
    }
    std::cout << "Single-instance cross-process App harness passed: one hidden receiver, Add/preservation, "
                 "Unicode/duplicates, batch cancel, race/handover, malformed/replayed/bounded IPC.\n";
    return 0;
}
} // namespace

HWND createSingleInstanceTestWindow(const std::wstring& testNamespace) {
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = kSingleInstanceTestWindowClass;
    check(RegisterClassW(&windowClass) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS, "Cannot register hidden receiver class");
    const HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, kSingleInstanceTestWindowClass, testNamespace.c_str(),
        WS_POPUP, 0, 0, 640, 480, nullptr, nullptr, windowClass.hInstance, nullptr);
    check(window != nullptr && !IsWindowVisible(window), "Cannot create hidden receiving player harness window");
    return window;
}

void writeSingleInstanceTestState(const std::filesystem::path& directory, const nlohmann::json& state) {
    static std::uint64_t serial = 0;
    static bool publicationRetryRecorded = false;
    const auto temporary = directory / (L"state-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(++serial) + L".next");
    const auto destination = directory / L"state.json";
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file << state.dump();
    file.close();
    if (!file) throw std::runtime_error("Cannot write isolated receiver state; writerPid=" +
        std::to_string(GetCurrentProcessId()) + ", errno=" + std::to_string(errno));
    const ULONGLONG deadline = GetTickCount64() + 1000;
    DWORD error = ERROR_SUCCESS;
    DWORD firstError = ERROR_SUCCESS;
    unsigned attempts = 0;
    do {
        if (MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            if (attempts && !publicationRetryRecorded) {
                std::ofstream record(directory / (L"publish-" + std::to_wstring(GetCurrentProcessId()) + L"-retries.json"), std::ios::binary);
                record << nlohmann::json{{"writerPid", GetCurrentProcessId()}, {"firstError", firstError},
                    {"attempts", attempts}, {"recovered", true}}.dump();
                record.close();
                check(static_cast<bool>(record), "Cannot record observed snapshot publication retry");
                publicationRetryRecorded = true;
            }
            return;
        }
        error = GetLastError();
        if (!attempts) firstError = error;
        ++attempts;
        // All test readers share DELETE. Host scanners can still hold short
        // locks on a fresh snapshot; retry only those specific file errors.
        if (error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED) break;
        Sleep(5);
    } while (GetTickCount64() < deadline);
    throw std::runtime_error("Cannot publish isolated receiver state; writerPid=" +
        std::to_string(GetCurrentProcessId()) + ", MoveFileExError=" + std::to_string(error));
}

std::optional<int> singleInstanceProcessMode(int argc, char** argv) {
    if (argc < 2) return std::nullopt;
    const std::string mode = argv[1];
    if (mode == "--single-instance-process") return processTests();
    if (mode != "--si-child") return std::nullopt;
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    check(arguments && count >= 5, "Invalid isolated child arguments");
    struct ArgumentsLifetime { LPWSTR* value; ~ArgumentsLifetime() { LocalFree(value); } } lifetime{arguments};
    const std::wstring name = arguments[2];
    const std::filesystem::path directory(arguments[3]);
    const std::wstring gateName = arguments[4];
    if (!gateName.empty()) {
        Handle gate(OpenEventW(SYNCHRONIZE, FALSE, gateName.c_str()));
        check(gate.get() && WaitForSingleObject(gate.get(), 15000) == WAIT_OBJECT_0, "Child launch barrier timed out");
    }
    std::vector<std::wstring> files;
    for (int index = 5; index < count; ++index) files.emplace_back(arguments[index]);
    SingleInstance instance(SingleInstance::IsolatedNamespace{name});
    std::wstring error;
    const auto request = requestFor(files);
    const auto result = instance.launch(request, error);
    {
        const char* classification = result == SingleInstance::LaunchResult::Primary ? "Primary" :
            result == SingleInstance::LaunchResult::Forwarded ? "Forwarded" : "Error";
        std::ofstream record(directory / (L"child-" + std::to_wstring(GetCurrentProcessId()) + L"-launch.json"), std::ios::binary);
        record << nlohmann::json{{"pid", GetCurrentProcessId()}, {"result", classification}, {"error", wideToUtf8Text(error)}}.dump();
        record.close();
        check(static_cast<bool>(record), "Cannot record isolated child launch classification");
    }
    if (result == SingleInstance::LaunchResult::Forwarded) return 0;
    if (result == SingleInstance::LaunchResult::Error) {
        std::ofstream diagnostic(directory / (L"child-" + std::to_wstring(GetCurrentProcessId()) + L"-error.log"), std::ios::binary);
        diagnostic << wideToUtf8Text(error);
        std::cerr << "Isolated child launch failed: " << wideToUtf8Text(error) << '\n';
        return 1;
    }
    // Both first and subsequent invocations take this same production
    // election path. Only the primary branch can create a player HWND.
    try { return singleInstanceAppReceiver(instance, request, name, directory); }
    catch (const std::exception& exception) {
        std::ofstream diagnostic(directory / (L"child-" + std::to_wstring(GetCurrentProcessId()) + L"-error.log"), std::ios::binary);
        diagnostic << exception.what();
        throw;
    }
}
} // namespace quaddeck
