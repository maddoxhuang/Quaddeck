#pragma once

#include <windows.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace quaddeck {

// Election and the pipe are established before App creates a player window.
// The worker only accepts bounded batches; App takes and completes them on
// its own thread. Destruction must occur on the launch/election thread.
class SingleInstance {
public:
    using RequestId = std::array<std::uint8_t, 16>;
    struct Request {
        RequestId id{};
        std::vector<std::wstring> files;
    };
    enum class LaunchResult { Primary, Forwarded, Error };

    // Tests inject a unique namespace explicitly. Production never reads an
    // environment variable or command-line switch to change its namespace.
    struct IsolatedNamespace {
        std::wstring suffix;
        // Optional test barrier before the first ConnectNamedPipe. The handle
        // is duplicated; closing the caller's handle cannot strand shutdown.
        HANDLE listenerStartGate{};
    };

    static constexpr std::size_t kMaxFiles = 256;
    static constexpr std::size_t kMaxPayloadBytes = 512 * 1024;
    static constexpr std::size_t kMaxPendingRequests = 64;
    static constexpr std::size_t kMaxPendingBytes = 2 * 1024 * 1024;
    static constexpr std::size_t kMaxReplayEntries = 1024;
    static constexpr std::chrono::seconds kReplayRetention{60};
    static constexpr std::chrono::milliseconds kDefaultTimeout{8000};

    // One message: LE u32 magic/version/totalBytes/fileCount, 16-byte id,
    // then fileCount repetitions of LE u32 UTF-16-unit count + raw UTF-16.
    // No NUL terminators, no trailing data. Reply is exactly 40 bytes:
    // LE u32 magic/version/status/serverPid, LE u64 HWND, 16-byte id.
    static constexpr std::uint32_t kRequestMagic = 0x51444950;
    static constexpr std::uint32_t kReplyMagic = 0x5144414B;
    static constexpr std::uint32_t kProtocolVersion = 1;
    static constexpr std::size_t kRequestHeaderBytes = 32;
    static constexpr std::size_t kReplyBytes = 40;

    SingleInstance();
    explicit SingleInstance(IsolatedNamespace isolatedNamespace);
    ~SingleInstance();
    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;

    static bool normalizeLocalPath(const std::wstring& path,
                                   std::wstring& normalized, std::wstring& error);
    static bool prepareRequest(const std::vector<std::wstring>& arguments,
                               Request& request, std::wstring& error);

    // Primary leaves this initial batch with its caller, for App startup.
    // Forwarded means the server has safely reserved and queued this id.
    // Retry the same Request/id; completed ids remain replayable for 60s.
    LaunchResult launch(const Request& request, std::wstring& error,
                        std::chrono::milliseconds timeout = kDefaultTimeout);
    bool isPrimary() const;
    void setWindow(HWND window);
    // Stop/join the listener before App begins teardown; retain election
    // ownership until destruction so a second player cannot overlap teardown.
    void stopAccepting();
    std::vector<Request> takeRequests(std::size_t maximum = 8);
    // Releases a reservation only after App finishes or cancels the batch.
    void completeRequest(const RequestId& id);

    const std::wstring& pipeName() const;
    const std::wstring& mutexName() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace quaddeck
