#pragma once

// UTF-8 <-> UTF-16 without Windows headers, so the pure protocol and
// subtitle headers and their tests need nothing from the platform. Invalid
// input becomes U+FFFD rather than aborting: a subtitle file with one bad
// byte should still show its other thousand lines. As the WHATWG decoder
// does, one replacement stands for a whole truncated sequence.

#include <cstdint>
#include <string>
#include <string_view>

namespace quaddeck {

inline std::wstring utf8ToWideText(std::string_view text) {
    std::wstring result;
    result.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const auto byte = static_cast<unsigned char>(text[i]);
        std::uint32_t code = 0;
        std::size_t extra = 0;
        if (byte < 0x80) { code = byte; }
        else if ((byte & 0xE0) == 0xC0) { code = byte & 0x1F; extra = 1; }
        else if ((byte & 0xF0) == 0xE0) { code = byte & 0x0F; extra = 2; }
        else if ((byte & 0xF8) == 0xF0) { code = byte & 0x07; extra = 3; }
        else { result.push_back(L'\xFFFD'); ++i; continue; }
        // Take the continuation bytes that are there; a sequence cut short
        // is consumed as a whole and replaced once.
        std::size_t taken = 0;
        while (taken < extra && i + 1 + taken < text.size() &&
               (static_cast<unsigned char>(text[i + 1 + taken]) & 0xC0) == 0x80) {
            code = (code << 6) | (static_cast<unsigned char>(text[i + 1 + taken]) & 0x3F);
            ++taken;
        }
        bool valid = taken == extra;
        // Overlong forms, surrogates and values past U+10FFFF are not UTF-8.
        if (valid && ((extra == 1 && code < 0x80) || (extra == 2 && code < 0x800) ||
                      (extra == 3 && code < 0x10000) || code > 0x10FFFF ||
                      (code >= 0xD800 && code <= 0xDFFF))) {
            valid = false;
        }
        if (!valid) {
            result.push_back(L'\xFFFD');
            i += 1 + taken;
            continue;
        }
        i += 1 + extra;
        if (code >= 0x10000) {
            code -= 0x10000;
            result.push_back(static_cast<wchar_t>(0xD800 + (code >> 10)));
            result.push_back(static_cast<wchar_t>(0xDC00 + (code & 0x3FF)));
        } else {
            result.push_back(static_cast<wchar_t>(code));
        }
    }
    return result;
}

inline std::string wideToUtf8Text(std::wstring_view text) {
    std::string result;
    result.reserve(text.size() * 3);
    for (std::size_t i = 0; i < text.size(); ++i) {
        std::uint32_t code = static_cast<std::uint16_t>(text[i]);
        if (code >= 0xD800 && code <= 0xDBFF && i + 1 < text.size()) {
            const std::uint32_t low = static_cast<std::uint16_t>(text[i + 1]);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                ++i;
            }
        }
        if (code >= 0xD800 && code <= 0xDFFF) code = 0xFFFD;
        if (code < 0x80) {
            result.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            result.push_back(static_cast<char>(0xC0 | (code >> 6)));
            result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            result.push_back(static_cast<char>(0xE0 | (code >> 12)));
            result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            result.push_back(static_cast<char>(0xF0 | (code >> 18)));
            result.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }
    return result;
}

}  // namespace quaddeck
