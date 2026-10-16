#pragma once

// HTTP to an Emby server on a worker thread, with the answers delivered on
// the thread that calls pump() -- the window thread, once per frame -- so
// nothing App owns is ever touched from the worker. Requests carry the
// session they were queued with; signing in configures the next ones.
//
// The token is kept on disk under DPAPI so a copied settings folder does not
// carry a working login for whoever reads it.

#include "EmbyApi.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace quaddeck {

class EmbyClient {
public:
    struct Response {
        bool ok{};      // 2xx
        int status{};   // 0 when the request never reached the server
        std::string body;
        std::string error;
        // The reply ran past the client's body limit and was cut off.
        bool tooLarge{};
    };
    // The response is the handler's own: it may move the body away, to be
    // read off the window thread.
    using Handler = std::function<void(Response&)>;

    EmbyClient();
    ~EmbyClient();
    EmbyClient(const EmbyClient&) = delete;
    EmbyClient& operator=(const EmbyClient&) = delete;

    void configure(const emby::Session& session);
    const emby::Session& session() const { return session_; }

    // Queues a call. `handler` may be empty for fire-and-forget reports.
    // A call whose `dropped` is set by the time the worker comes to it is
    // not made: its handler runs with status 0 and the error "Cancelled".
    // A search typed on past leaves the libraries it had yet to ask so.
    void request(std::string method, std::string pathAndQuery, std::string body, Handler handler,
                 std::shared_ptr<const std::atomic<bool>> dropped = {});
    // Runs the handlers of finished calls. Call from one thread only.
    void pump();
    // Drops the calls not yet started; their handlers run from the next
    // pump() with status 0 and the error "Cancelled". The browser forgets
    // the pictures of a page it has left this way.
    void clearPending();
    // The Accept header of every call from here on: JSON unless told
    // otherwise (the picture client asks for images).
    void setAccept(std::string accept);
    // The largest reply body of every call from here on; one larger ends
    // there with "The reply is too large". 64 MiB unless told otherwise
    // (the subtitle client takes what a subtitle file may be).
    void setBodyLimit(std::size_t bytes);
    // Waits for queued calls to finish, up to `milliseconds`; for shutdown,
    // so the final Stopped report gets out.
    void drain(unsigned milliseconds);

    static std::string protectSecret(const std::string& secret);
    static std::string unprotectSecret(const std::string& protectedBase64);
    static std::string newDeviceId();

private:
    struct Job {
        std::string method, path, body, accept;
        Handler handler;
        emby::Session session;
        std::size_t bodyLimit{};
        std::shared_ptr<const std::atomic<bool>> dropped;
    };
    void workerMain();
    Response perform(const Job& job);

    emby::Session session_;
    std::string accept_{"application/json"};
    std::size_t bodyLimit_{64u * 1024u * 1024u};
    std::thread worker_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable idle_;
    std::deque<Job> pending_;
    std::deque<std::pair<Handler, Response>> completed_;
    bool stop_{};
    bool busy_{};
    void* activeRequest_{};
};

}  // namespace quaddeck
