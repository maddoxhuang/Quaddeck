#pragma once

#include "BuildInfo.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}

namespace quaddeck {

// How long one HTTP connect or read may wait with nothing arriving. A server
// that goes silent without closing the connection -- a NAS falling asleep, a
// stalled transcoder, Wi-Fi roaming -- would otherwise hold the worker's read,
// and the pane, for good. Past this the read fails, and the reconnect options
// below resume at the same offset or give up with an error. It runs only while
// a read waits, never while a paused pane or a full queue reads nothing.
// Generous: a disk spinning up before the server answers a Range request costs
// at most a needless reconnect.
inline constexpr std::chrono::microseconds kHttpReadTimeout = std::chrono::seconds(15);

namespace media_input_detail {

inline int denySecondaryOpen(AVFormatContext*, AVIOContext**, const char*, int, AVDictionary**) {
    return AVERROR(EACCES);
}

inline int openAuthenticatedFile(AVFormatContext* context, AVIOContext** io, const char* url,
                                  int flags, AVDictionary** options) {
    // Static Emby media is one file. A playlist or container must not reuse
    // its credentials for another resource, even without an HTTP redirect.
    context->io_open = denySecondaryOpen;
    if (!context->url || std::strcmp(context->url, url) != 0 || flags != AVIO_FLAG_READ) {
        return AVERROR(EACCES);
    }
    return avio_open2(io, url, flags, &context->interrupt_callback, options);
}

}  // namespace media_input_detail

// Shared by video, photos and the independent audio demuxer. Keep the policy
// on the HTTP context itself: seeking and reconnecting can redirect too.
inline int openMediaInput(AVFormatContext** context, const std::string& path,
                          const std::string& headers) {
    AVDictionary* dictionary = nullptr;
    if (!headers.empty()) {
        if (!context || !*context || (*context)->pb) return AVERROR(EINVAL);
        const char* protocol = avio_find_protocol_name(path.c_str());
        if (!protocol || (std::strcmp(protocol, "http") != 0 && std::strcmp(protocol, "https") != 0)) {
            return AVERROR(EACCES);
        }
        // Check the loaded dependency before any request. An ignored option
        // discovered after opening would already have leaked the credential.
        const AVClass* protocolClass = avio_protocol_get_class(protocol);
        const AVOption* redirects = protocolClass ? av_opt_find(&protocolClass, "max_redirects", nullptr,
            0, AV_OPT_SEARCH_FAKE_OBJ) : nullptr;
        if (!redirects || redirects->type != AV_OPT_TYPE_INT || redirects->min > 0) {
            return AVERROR(ENOSYS);
        }
        const auto set = [&](const char* name, const char* value) {
            return av_dict_set(&dictionary, name, value, 0);
        };
        int result = set("max_redirects", "0");
        if (result >= 0) result = set("headers", headers.c_str());
        if (result >= 0) result = set("user_agent", buildUserAgent().c_str());
        if (result >= 0) result = set("reconnect", "1");
        if (result >= 0) result = set("reconnect_on_network_error", "1");
        if (result >= 0) result = set("reconnect_delay_max", "4");
        // The generic URL option, which FFmpeg copies to the TCP/TLS
        // connection under it and again to every reconnect.
        if (result >= 0) result = set("rw_timeout", std::to_string(kHttpReadTimeout.count()).c_str());
        if (result < 0) {
            av_dict_free(&dictionary);
            return result;
        }
        (*context)->io_open = media_input_detail::openAuthenticatedFile;
    }
    const int result = avformat_open_input(context, path.c_str(), nullptr,
                                           dictionary ? &dictionary : nullptr);
    av_dict_free(&dictionary);
    return result;
}

}  // namespace quaddeck
