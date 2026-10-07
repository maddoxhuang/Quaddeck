#include "EmbyClient.hpp"

#include "Diagnostics.hpp"

#include <windows.h>
#include <winhttp.h>
#include <dpapi.h>
#include <wincrypt.h>
#include <objbase.h>

#include <algorithm>
#include <chrono>
#include <sstream>

#pragma comment(lib, "winhttp")
#pragma comment(lib, "crypt32")

namespace quaddeck {

namespace {

constexpr std::size_t kMaximumBody = 64u * 1024u * 1024u;

std::wstring widen(const std::string& text) { return utf8ToWideText(text); }

}  // namespace

// The worker starts from the body, after every member it uses exists.
EmbyClient::EmbyClient() { worker_ = std::thread(&EmbyClient::workerMain, this); }

EmbyClient::~EmbyClient() {
    {
        std::scoped_lock lock(mutex_);
        stop_ = true;
        // Closing the handle from here aborts a blocked call on the worker,
        // which then finds activeRequest_ null and does not close it again.
        if (activeRequest_) WinHttpCloseHandle(static_cast<HINTERNET>(activeRequest_));
        activeRequest_ = nullptr;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void EmbyClient::configure(const emby::Session& session) {
    std::scoped_lock lock(mutex_);
    session_ = session;
}

void EmbyClient::request(std::string method, std::string pathAndQuery, std::string body, Handler handler) {
    {
        std::scoped_lock lock(mutex_);
        pending_.push_back(Job{std::move(method), std::move(pathAndQuery), std::move(body), accept_,
                               std::move(handler), session_});
    }
    wake_.notify_one();
}

void EmbyClient::pump() {
    std::deque<std::pair<Handler, Response>> ready;
    {
        std::scoped_lock lock(mutex_);
        ready.swap(completed_);
    }
    for (auto& [handler, response] : ready) {
        if (handler) handler(response);
    }
}

void EmbyClient::clearPending() {
    std::scoped_lock lock(mutex_);
    for (auto& job : pending_) {
        if (!job.handler) continue;
        Response cancelled;
        cancelled.error = "Cancelled";
        completed_.emplace_back(std::move(job.handler), std::move(cancelled));
    }
    pending_.clear();
}

void EmbyClient::setAccept(std::string accept) {
    std::scoped_lock lock(mutex_);
    accept_ = std::move(accept);
}

void EmbyClient::drain(unsigned milliseconds) {
    std::unique_lock lock(mutex_);
    idle_.wait_for(lock, std::chrono::milliseconds(milliseconds),
                   [&] { return pending_.empty() && !busy_; });
}

std::size_t EmbyClient::pendingCount() const {
    std::scoped_lock lock(mutex_);
    return pending_.size() + (busy_ ? 1 : 0);
}

void EmbyClient::workerMain() {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return stop_ || !pending_.empty(); });
            if (stop_) return;
            job = std::move(pending_.front());
            pending_.pop_front();
            busy_ = true;
        }
        Response response = perform(job);
        {
            std::scoped_lock lock(mutex_);
            busy_ = false;
            if (job.handler) completed_.emplace_back(std::move(job.handler), std::move(response));
        }
        idle_.notify_all();
    }
}

EmbyClient::Response EmbyClient::perform(const Job& job) {
    Response response;
    const std::wstring url = widen(job.session.serverUrl);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256]{};
    wchar_t prefix[1024]{};
    parts.lpszHostName = host;
    parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
    parts.lpszUrlPath = prefix;
    parts.dwUrlPathLength = static_cast<DWORD>(std::size(prefix));
    if (url.empty() || !WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts)) {
        response.error = "The server address is not a valid URL";
        return response;
    }
    const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    std::wstring path = prefix;
    if (path == L"/") path.clear();
    path += widen(job.path);

    const auto failure = [](const char* step) {
        std::ostringstream out;
        out << step << " failed (WinHTTP error " << GetLastError() << ")";
        return out.str();
    };
    HINTERNET session = WinHttpOpen(L"QuadDeck/1.0.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        response.error = failure("Opening WinHTTP");
        return response;
    }
    WinHttpSetTimeouts(session, 5000, 5000, 15000, 30000);
    HINTERNET connection = WinHttpConnect(session, host, parts.nPort, 0);
    if (!connection) {
        response.error = failure("Connecting");
        WinHttpCloseHandle(session);
        return response;
    }
    HINTERNET request = WinHttpOpenRequest(
        connection, widen(job.method).c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
        response.error = failure("Preparing the request");
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return response;
    }
    // The token must not follow a redirect to another host.
    DWORD disabled = WINHTTP_DISABLE_REDIRECTS;
    WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled));
    {
        // From here the request handle is shared with the destructor, which
        // closes it to abort; whoever nulls activeRequest_ first closes it.
        std::scoped_lock lock(mutex_);
        if (stop_) {
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            response.error = "Shutting down";
            return response;
        }
        activeRequest_ = request;
    }
    std::wstring headers = L"X-Emby-Authorization: " +
                           widen(emby::authorizationHeader(job.session.deviceId)) + L"\r\n";
    if (!job.session.token.empty()) headers += L"X-Emby-Token: " + widen(job.session.token) + L"\r\n";
    headers += L"Accept: " + widen(job.accept.empty() ? std::string("application/json") : job.accept) + L"\r\n";
    if (!job.body.empty()) headers += L"Content-Type: application/json\r\n";
    WinHttpAddRequestHeaders(request, headers.c_str(), static_cast<DWORD>(headers.size()),
                             WINHTTP_ADDREQ_FLAG_ADD);
    const auto bodySize = static_cast<DWORD>(job.body.size());
    const BOOL sent = WinHttpSendRequest(
        request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        bodySize ? const_cast<char*>(job.body.data()) : WINHTTP_NO_REQUEST_DATA, bodySize, bodySize, 0);
    if (sent && WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0;
        DWORD size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        response.status = static_cast<int>(status);
        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available)) {
                response.error = "Reading the reply failed";
                break;
            }
            if (available == 0) break;
            const std::size_t offset = response.body.size();
            if (offset + available > kMaximumBody) {
                response.error = "The reply is too large";
                break;
            }
            response.body.resize(offset + available);
            DWORD read = 0;
            if (!WinHttpReadData(request, response.body.data() + offset, available, &read)) {
                response.error = "Reading the reply failed";
                break;
            }
            response.body.resize(offset + read);
            if (read == 0) break;
        }
        response.ok = response.error.empty() && response.status >= 200 && response.status < 300;
        if (!response.ok && response.error.empty()) {
            response.error = response.status >= 300 && response.status < 400
                ? "HTTP " + std::to_string(response.status) + " (the server redirects; enter its final address)"
                : "HTTP " + std::to_string(response.status);
        }
    } else {
        response.error = failure("The request");
    }
    {
        std::scoped_lock lock(mutex_);
        if (activeRequest_) {
            WinHttpCloseHandle(static_cast<HINTERNET>(activeRequest_));
            activeRequest_ = nullptr;
        }
    }
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return response;
}

std::string EmbyClient::protectSecret(const std::string& secret) {
    if (secret.empty()) return {};
    DATA_BLOB input{static_cast<DWORD>(secret.size()),
                    reinterpret_cast<BYTE*>(const_cast<char*>(secret.data()))};
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"QuadDeck Emby token", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        return {};
    }
    DWORD length = 0;
    std::string encoded;
    if (CryptBinaryToStringA(output.pbData, output.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                             nullptr, &length) && length > 0) {
        encoded.resize(length);
        if (CryptBinaryToStringA(output.pbData, output.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                                 encoded.data(), &length)) {
            encoded.resize(length);
            while (!encoded.empty() && encoded.back() == '\0') encoded.pop_back();
        } else {
            encoded.clear();
        }
    }
    LocalFree(output.pbData);
    return encoded;
}

std::string EmbyClient::unprotectSecret(const std::string& protectedBase64) {
    if (protectedBase64.empty()) return {};
    DWORD length = 0;
    if (!CryptStringToBinaryA(protectedBase64.c_str(), 0, CRYPT_STRING_BASE64, nullptr, &length,
                              nullptr, nullptr) || length == 0) {
        return {};
    }
    std::string bytes(length, '\0');
    if (!CryptStringToBinaryA(protectedBase64.c_str(), 0, CRYPT_STRING_BASE64,
                              reinterpret_cast<BYTE*>(bytes.data()), &length, nullptr, nullptr)) {
        return {};
    }
    DATA_BLOB input{length, reinterpret_cast<BYTE*>(bytes.data())};
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        return {};
    }
    std::string secret(reinterpret_cast<const char*>(output.pbData), output.cbData);
    LocalFree(output.pbData);
    return secret;
}

std::string EmbyClient::newDeviceId() {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) {
        return "quaddeck-" + std::to_string(GetTickCount64());
    }
    char text[40]{};
    std::snprintf(text, sizeof(text), "%08lx%04x%04x%02x%02x%02x%02x%02x%02x%02x%02x",
                  static_cast<unsigned long>(guid.Data1), guid.Data2, guid.Data3,
                  guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
                  guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
    return text;
}

}  // namespace quaddeck
