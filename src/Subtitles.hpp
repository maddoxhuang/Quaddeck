#pragma once

// Text subtitles: SRT, WebVTT and ASS/SSA parsed into timed cues, the lookup
// a frame makes, and the choice among several tracks. Pure -- the file is a
// string, the answer is a string -- so the Emby test binary covers the
// formats without a player.
//
// Two readings of one file. libass (AssSubtitles.hpp) draws a script as it
// is written -- fonts, colours, positions, movement, karaoke -- and is fed
// from here: an ASS script as it came, an SRT or WebVTT file turned into one
// (`plainSubtitleScript`). The cues are the other reading, plain text with
// only whether a line belongs at the top of the picture or at its bottom;
// the Overlay draws those itself where libass cannot.

#include "TextEncoding.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <initializer_list>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace quaddeck {

struct SubtitleCue {
    double start{};
    double end{};
    std::wstring text;
    // At the top of the picture instead of its bottom: the file put the line
    // there (\an7-9, a style aligned so, a \pos in the upper half).
    bool top{};
};

struct SubtitleTrack {
    std::vector<SubtitleCue> cues;  // sorted by start
    // Optional lookup index: prefixMaxEnd[i] is the latest end in cues[0..i].
    // Parsed tracks build it once and are immutable during playback. Rebuild
    // or clear it whenever changing cues in a manually constructed track.
    std::vector<double> prefixMaxEnd;
    // The file the cues were read from, as UTF-8, and whether it is an ASS
    // or SSA script: what libass draws. Empty for cues gathered otherwise.
    std::string text;
    bool ass{};

    void rebuildLookupIndex() {
        prefixMaxEnd.clear();
        prefixMaxEnd.reserve(cues.size());
        for (const auto& cue : cues) {
            prefixMaxEnd.push_back(prefixMaxEnd.empty() ? cue.end : std::max(prefixMaxEnd.back(), cue.end));
        }
    }
};

// What an ASS script says before its events that a text-only reading still
// needs: the height positions are measured against, and where each style
// puts its lines.
struct AssHeader {
    double playResY{288.0};
    std::vector<std::pair<std::wstring, int>> styles;  // name, alignment as \an counts

    int alignment(std::wstring_view style) const {
        // An event may name its style with a leading asterisk ("*Default").
        while (!style.empty() && style.front() == L'*') style.remove_prefix(1);
        for (const auto& [name, value] : styles) {
            if (name == style) return value;
        }
        return 2;
    }
};

namespace subtitle_detail {

inline std::wstring_view trim(std::wstring_view text) {
    while (!text.empty() && std::iswspace(text.front())) text.remove_prefix(1);
    while (!text.empty() && std::iswspace(text.back())) text.remove_suffix(1);
    return text;
}

inline bool startsWith(std::wstring_view text, std::wstring_view prefix) {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

inline std::wstring lowered(std::wstring_view text) {
    std::wstring result(text);
    for (auto& c : result) c = static_cast<wchar_t>(std::towlower(c));
    return result;
}

inline bool digitsOnly(std::wstring_view text) {
    return !text.empty() && std::all_of(text.begin(), text.end(),
                                        [](wchar_t c) { return c >= L'0' && c <= L'9'; });
}

inline long long digitsValue(std::wstring_view text) {
    long long value = 0;
    for (const wchar_t c : text) value = value * 10 + (c - L'0');
    return value;
}

// "12", "-3.5": the numbers ASS writes. Nothing for anything else.
inline std::optional<double> parseNumber(std::wstring_view text) {
    text = trim(text);
    bool negative = false;
    if (!text.empty() && (text.front() == L'-' || text.front() == L'+')) {
        negative = text.front() == L'-';
        text.remove_prefix(1);
    }
    const auto point = text.find(L'.');
    const std::wstring_view whole = text.substr(0, point);
    const std::wstring_view fraction =
        point == std::wstring_view::npos ? std::wstring_view{} : text.substr(point + 1);
    if ((whole.empty() && fraction.empty()) || (!whole.empty() && !digitsOnly(whole)) ||
        (!fraction.empty() && !digitsOnly(fraction)) || whole.size() > 15 || fraction.size() > 15) {
        return std::nullopt;
    }
    double value = whole.empty() ? 0.0 : static_cast<double>(digitsValue(whole));
    if (!fraction.empty()) {
        value += static_cast<double>(digitsValue(fraction)) / std::pow(10.0, static_cast<double>(fraction.size()));
    }
    return negative ? -value : value;
}

// "01:02:03,456", "1:02:03.45" (ASS centiseconds), "02:03.456" (VTT).
inline std::optional<double> parseTimestamp(std::wstring_view text) {
    text = trim(text);
    std::vector<std::wstring_view> parts;
    for (;;) {
        const auto colon = text.find(L':');
        if (colon == std::wstring_view::npos) break;
        parts.push_back(text.substr(0, colon));
        text.remove_prefix(colon + 1);
    }
    parts.push_back(text);
    if (parts.size() < 2 || parts.size() > 3) return std::nullopt;
    double total = 0.0;
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        const auto part = trim(parts[i]);
        // No time has a field of more digits than this; a longer run is
        // not one, and would not fit the number it is read into.
        if (!digitsOnly(part) || part.size() > 9) return std::nullopt;
        total = total * 60.0 + static_cast<double>(digitsValue(part));
    }
    auto seconds = trim(parts.back());
    auto separator = seconds.find(L',');
    if (separator == std::wstring_view::npos) separator = seconds.find(L'.');
    std::wstring_view whole = seconds;
    std::wstring_view fraction;
    if (separator != std::wstring_view::npos) {
        whole = seconds.substr(0, separator);
        fraction = seconds.substr(separator + 1);
    }
    if (!digitsOnly(whole) || (!fraction.empty() && !digitsOnly(fraction)) || whole.size() > 9 ||
        fraction.size() > 9) {
        return std::nullopt;
    }
    double value = static_cast<double>(digitsValue(whole));
    if (!fraction.empty()) {
        value += static_cast<double>(digitsValue(fraction)) / std::pow(10.0, static_cast<double>(fraction.size()));
    }
    return total * 60.0 + value;
}

inline void appendEntity(std::wstring& out, std::wstring_view entity) {
    if (entity == L"amp") out += L'&';
    else if (entity == L"lt") out += L'<';
    else if (entity == L"gt") out += L'>';
    else if (entity == L"quot") out += L'"';
    else if (entity == L"nbsp") out += L' ';
    else if (entity == L"#39" || entity == L"apos") out += L'\'';
    else { out += L'&'; out += entity; out += L';'; }
}

// SSA's own alignment numbers (\a, and the styles of a v4 script): 1-3 at
// the bottom, 5-7 at the top, 9-11 in the middle; left, centre, right. As
// \an counts them, 0 for a number that names no place.
inline int alignmentFromLegacy(int value) {
    const int horizontal = value & 3;
    if (horizontal == 0) return 0;
    if (value & 4) return 6 + horizontal;
    if (value & 8) return 3 + horizontal;
    return horizontal;
}

// A line without its markup, and what the markup said of its place.
struct Markup {
    std::wstring text;
    int alignment{};    // 1-9 as \an counts, 0 when the line does not say
    bool positioned{};  // \pos or \move names a point
    double positionY{};
};

// The tags of one {...} block. `drawing` follows \p: between \p1 and \p0
// the line's characters are the outline of a shape, not words.
inline void readOverrideBlock(std::wstring_view block, Markup& markup, bool& drawing) {
    std::size_t i = 0;
    while (i < block.size()) {
        if (block[i] != L'\\') { ++i; continue; }
        const std::size_t begin = ++i;
        // A tag runs to the next backslash outside parentheses: \t(...)
        // carries tags of its own.
        int depth = 0;
        while (i < block.size() && (block[i] != L'\\' || depth > 0)) {
            if (block[i] == L'(') ++depth;
            else if (block[i] == L')' && depth > 0) --depth;
            ++i;
        }
        const std::wstring_view tag = trim(block.substr(begin, i - begin));
        if (tag.size() >= 3 && startsWith(tag, L"an") && tag[2] >= L'1' && tag[2] <= L'9') {
            if (markup.alignment == 0) markup.alignment = tag[2] - L'0';
        } else if (tag.size() >= 2 && tag[0] == L'a' && digitsOnly(tag.substr(1))) {
            if (markup.alignment == 0) {
                markup.alignment = alignmentFromLegacy(static_cast<int>(digitsValue(tag.substr(1, 4))));
            }
        } else if (startsWith(tag, L"pos(") || startsWith(tag, L"move(")) {
            std::wstring_view arguments = tag.substr(tag.find(L'(') + 1);
            const auto first = arguments.find(L',');
            if (first != std::wstring_view::npos && !markup.positioned) {
                arguments.remove_prefix(first + 1);
                const auto end = arguments.find_first_of(L",)");
                if (const auto y = parseNumber(arguments.substr(0, end))) {
                    markup.positioned = true;
                    markup.positionY = *y;
                }
            }
        } else if (tag.size() >= 2 && tag[0] == L'p' && digitsOnly(tag.substr(1))) {
            drawing = digitsValue(tag.substr(1, 4)) > 0;
        }
    }
}

// "m 0 0 l 100 0 100 100 0 100": a shape's outline that a conversion to SRT
// left behind as if it were a line of speech.
inline bool looksLikeDrawing(std::wstring_view line) {
    if (line.size() < 7 || line[0] != L'm' || line[1] != L' ') return false;
    bool digit = false;
    for (const wchar_t c : line) {
        if (c >= L'0' && c <= L'9') { digit = true; continue; }
        if (c == L' ' || c == L'.' || c == L'-' || c == L'm' || c == L'n' || c == L'l' ||
            c == L'b' || c == L's' || c == L'p' || c == L'c') continue;
        return false;
    }
    return digit;
}

// Drops <i>/<font> style tags and ASS override blocks, turns the line-break
// escapes into real line breaks, and notes where the overrides place the
// line. Empty lines, stray spaces and drawings go.
inline Markup parseMarkup(std::wstring_view text, bool ass) {
    Markup markup;
    std::wstring out;
    out.reserve(text.size());
    bool drawing = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const wchar_t c = text[i];
        if (c == L'<') {
            const auto close = text.find(L'>', i + 1);
            // A tag, a closing tag, or WebVTT's inline <00:00:01.000> timing.
            if (close != std::wstring_view::npos && close > i + 1 &&
                (std::iswalpha(text[i + 1]) || text[i + 1] == L'/' || std::iswdigit(text[i + 1]))) {
                std::wstring_view tag = text.substr(i + 1, close - i - 1);
                if (startsWith(tag, L"br")) out += L'\n';
                i = close;
                continue;
            }
        } else if (c == L'{') {
            const auto close = text.find(L'}', i + 1);
            if (close != std::wstring_view::npos && (ass || (close > i + 1 && text[i + 1] == L'\\'))) {
                readOverrideBlock(text.substr(i + 1, close - i - 1), markup, drawing);
                i = close;
                continue;
            }
        } else if (c == L'\\' && ass && i + 1 < text.size()) {
            const wchar_t next = text[i + 1];
            if (next == L'N' || next == L'n') { out += L'\n'; ++i; continue; }
            if (next == L'h') { out += L' '; ++i; continue; }
        } else if (c == L'&') {
            const auto semicolon = text.find(L';', i + 1);
            if (semicolon != std::wstring_view::npos && semicolon - i <= 8) {
                if (!drawing) appendEntity(out, text.substr(i + 1, semicolon - i - 1));
                i = semicolon;
                continue;
            }
        }
        if (!drawing) out += c;
    }
    // Normalise the lines: trim each, drop the empty ones.
    std::wstring_view rest = out;
    while (!rest.empty()) {
        const auto newline = rest.find(L'\n');
        const auto line = trim(rest.substr(0, newline));
        if (!line.empty() && !looksLikeDrawing(line)) {
            if (!markup.text.empty()) markup.text += L'\n';
            markup.text += line;
        }
        if (newline == std::wstring_view::npos) break;
        rest.remove_prefix(newline + 1);
    }
    return markup;
}

inline std::wstring stripMarkup(std::wstring_view text, bool ass) {
    return parseMarkup(text, ass).text;
}

inline void addCue(std::vector<SubtitleCue>& cues, double start, double end, std::wstring text,
                   bool top = false) {
    if (text.empty() || start < 0.0) return;
    if (!(end > start)) end = start + 2.0;
    cues.push_back({start, end, std::move(text), top});
}

// Each cue of an SRT or WebVTT file: its times, and its text as written --
// tags and all -- with its lines joined by '\n'.
template <typename Take>
inline void forEachSrtCue(const std::vector<std::wstring>& lines, Take&& take) {
    std::size_t i = 0;
    while (i < lines.size()) {
        const std::wstring_view line = trim(lines[i]);
        const auto arrow = line.find(L"-->");
        if (arrow == std::wstring_view::npos) { ++i; continue; }
        std::wstring_view endText = trim(line.substr(arrow + 3));
        const auto settings = endText.find_first_of(L" \t");
        if (settings != std::wstring_view::npos) endText = endText.substr(0, settings);
        const auto start = parseTimestamp(line.substr(0, arrow));
        const auto end = parseTimestamp(endText);
        ++i;
        if (!start || !end) continue;
        std::wstring text;
        while (i < lines.size()) {
            const std::wstring_view next = trim(lines[i]);
            if (next.empty() || next.find(L"-->") != std::wstring_view::npos) break;
            if (!text.empty()) text += L'\n';
            text += next;
            ++i;
        }
        take(*start, *end, text);
    }
}

inline void parseSrtLike(const std::vector<std::wstring>& lines, std::vector<SubtitleCue>& cues) {
    forEachSrtCue(lines, [&](double start, double end, const std::wstring& text) {
        // An SRT may carry {\an8}: the one override players agreed to honour,
        // and what a server's conversion from ASS leaves of a line's place.
        auto markup = parseMarkup(text, false);
        addCue(cues, start, end, std::move(markup.text), markup.alignment >= 4);
    });
}

inline std::vector<std::wstring> commaList(std::wstring_view rest) {
    std::vector<std::wstring> fields;
    for (;;) {
        const auto comma = rest.find(L',');
        fields.emplace_back(trim(rest.substr(0, comma)));
        if (comma == std::wstring_view::npos) break;
        rest.remove_prefix(comma + 1);
    }
    return fields;
}

// Where an ASS script is being read.
struct AssReadState {
    enum class Section { Other, Info, Styles, Events } section{Section::Other};
    // A v4 (SSA) script counts alignment its own way.
    bool legacyStyles{};
    std::vector<std::wstring> styleFormat;
};

// One non-empty line of an ASS script outside its events: the section
// headers, the script's height and its styles. True for a section header.
inline bool readAssHeaderLine(std::wstring_view line, AssHeader& header, AssReadState& state) {
    if (line.front() == L'[') {
        const std::wstring section = lowered(line);
        if (section == L"[script info]") state.section = AssReadState::Section::Info;
        else if (section == L"[events]") state.section = AssReadState::Section::Events;
        else if (startsWith(section, L"[v4") && section.find(L"styles") != std::wstring::npos) {
            state.section = AssReadState::Section::Styles;
            state.legacyStyles = section.find(L'+') == std::wstring::npos;
            state.styleFormat.clear();
        } else {
            state.section = AssReadState::Section::Other;
        }
        return true;
    }
    if (state.section == AssReadState::Section::Info) {
        if (lowered(line.substr(0, 9)) == L"playresy:") {
            if (const auto value = parseNumber(line.substr(9)); value && *value > 0.0) header.playResY = *value;
        }
    } else if (state.section == AssReadState::Section::Styles) {
        if (startsWith(line, L"Format:")) {
            state.styleFormat = commaList(line.substr(7));
            for (auto& column : state.styleFormat) column = lowered(column);
        } else if (startsWith(line, L"Style:")) {
            const auto fields = commaList(line.substr(6));
            std::size_t nameColumn = 0;
            // Where the two scripts keep it when no Format line said.
            std::size_t alignmentColumn = state.legacyStyles ? 12 : 18;
            for (std::size_t column = 0; column < state.styleFormat.size(); ++column) {
                if (state.styleFormat[column] == L"name") nameColumn = column;
                else if (state.styleFormat[column] == L"alignment") alignmentColumn = column;
            }
            if (nameColumn < fields.size()) {
                int alignment = 2;
                if (alignmentColumn < fields.size() && digitsOnly(fields[alignmentColumn])) {
                    const int value = static_cast<int>(
                        digitsValue(std::wstring_view(fields[alignmentColumn]).substr(0, 4)));
                    alignment = state.legacyStyles ? alignmentFromLegacy(value) : value;
                    if (alignment < 1 || alignment > 9) alignment = 2;
                }
                header.styles.emplace_back(fields[nameColumn], alignment);
            }
        }
    }
    return false;
}

// An event's text as a cue: where its own overrides or, failing those, its
// style put it. Nothing for a line that is only markup or a drawing.
inline std::optional<SubtitleCue> assCue(const AssHeader& header, std::wstring_view style,
                                         std::wstring_view text, double start, double end) {
    auto markup = parseMarkup(text, true);
    if (markup.text.empty() || start < 0.0) return std::nullopt;
    if (!(end > start)) end = start + 2.0;
    const int alignment = markup.alignment != 0 ? markup.alignment : header.alignment(trim(style));
    const bool top = markup.positioned && header.playResY > 0.0
        ? markup.positionY < header.playResY * 0.5 : alignment >= 4;
    return SubtitleCue{start, end, std::move(markup.text), top};
}

inline void parseAss(const std::vector<std::wstring>& lines, std::vector<SubtitleCue>& cues) {
    std::vector<std::wstring> format{L"Layer", L"Start", L"End", L"Style", L"Name",
                                     L"MarginL", L"MarginR", L"MarginV", L"Effect", L"Text"};
    AssHeader header;
    AssReadState state;
    for (const auto& raw : lines) {
        const std::wstring_view line = trim(raw);
        if (line.empty()) continue;
        if (readAssHeaderLine(line, header, state)) continue;
        if (state.section != AssReadState::Section::Events) continue;
        if (startsWith(line, L"Format:")) {
            format = commaList(line.substr(7));
            continue;
        }
        if (!startsWith(line, L"Dialogue:")) continue;
        std::wstring_view rest = trim(line.substr(9));
        std::vector<std::wstring_view> fields;
        for (std::size_t k = 0; k + 1 < format.size(); ++k) {
            const auto comma = rest.find(L',');
            if (comma == std::wstring_view::npos) break;
            fields.push_back(rest.substr(0, comma));
            rest.remove_prefix(comma + 1);
        }
        fields.push_back(rest);
        std::optional<double> start, end;
        std::wstring_view text, style;
        for (std::size_t k = 0; k < fields.size() && k < format.size(); ++k) {
            if (format[k] == L"Start") start = parseTimestamp(fields[k]);
            else if (format[k] == L"End") end = parseTimestamp(fields[k]);
            else if (format[k] == L"Style") style = fields[k];
            else if (format[k] == L"Text") text = fields[k];
        }
        if (!start || !end) continue;
        if (auto cue = assCue(header, style, text, *start, *end)) cues.push_back(std::move(*cue));
    }
}

inline std::vector<std::wstring> splitLines(std::wstring_view rest) {
    std::vector<std::wstring> lines;
    while (!rest.empty()) {
        const auto newline = rest.find(L'\n');
        std::wstring_view line = rest.substr(0, newline);
        if (!line.empty() && line.back() == L'\r') line.remove_suffix(1);
        lines.emplace_back(line);
        if (newline == std::wstring_view::npos) break;
        rest.remove_prefix(newline + 1);
    }
    return lines;
}

}  // namespace subtitle_detail

// A subtitle file's bytes as UTF-8: UTF-16 with a byte-order mark, either
// endianness, is converted; anything else is returned as it came for the
// caller to judge.
inline std::string decodeSubtitleBytes(std::string bytes) {
    if (bytes.size() >= 2) {
        const auto first = static_cast<unsigned char>(bytes[0]);
        const auto second = static_cast<unsigned char>(bytes[1]);
        if ((first == 0xFF && second == 0xFE) || (first == 0xFE && second == 0xFF)) {
            const bool bigEndian = first == 0xFE;
            std::wstring text;
            text.reserve((bytes.size() - 2) / 2);
            for (std::size_t i = 2; i + 1 < bytes.size(); i += 2) {
                const unsigned low = static_cast<unsigned char>(bytes[bigEndian ? i + 1 : i]);
                const unsigned high = static_cast<unsigned char>(bytes[bigEndian ? i : i + 1]);
                text.push_back(static_cast<wchar_t>(low | (high << 8)));
            }
            return wideToUtf8Text(text);
        }
    }
    return bytes;
}

// Whether bytes are UTF-8 as they stand. A script in a legacy code page is
// not, and read as if it were its lines would be replacement characters.
inline bool isUtf8Text(std::string_view bytes) {
    return wideToUtf8Text(utf8ToWideText(bytes)) == bytes;
}

// Recognises the format from the content, not the file name: Emby hands
// converted subtitles over with a fixed name.
inline std::optional<SubtitleTrack> parseSubtitles(std::string_view utf8) {
    if (utf8.size() >= 3 && static_cast<unsigned char>(utf8[0]) == 0xEF &&
        static_cast<unsigned char>(utf8[1]) == 0xBB && static_cast<unsigned char>(utf8[2]) == 0xBF) {
        utf8.remove_prefix(3);
    }
    const std::wstring wide = utf8ToWideText(utf8);
    if (subtitle_detail::trim(wide).empty()) return std::nullopt;
    const std::vector<std::wstring> lines = subtitle_detail::splitLines(wide);
    bool ass = false;
    for (const auto& line : lines) {
        const auto trimmed = subtitle_detail::trim(line);
        if (trimmed.empty()) continue;
        if (subtitle_detail::startsWith(trimmed, L"WEBVTT")) break;
        if (subtitle_detail::startsWith(trimmed, L"[Script Info]") ||
            subtitle_detail::startsWith(trimmed, L"Dialogue:")) { ass = true; break; }
        if (trimmed.find(L"-->") != std::wstring_view::npos) break;
    }
    SubtitleTrack track;
    track.text.assign(utf8);
    track.ass = ass;
    if (ass) subtitle_detail::parseAss(lines, track.cues);
    else subtitle_detail::parseSrtLike(lines, track.cues);
    std::stable_sort(track.cues.begin(), track.cues.end(),
                     [](const SubtitleCue& a, const SubtitleCue& b) { return a.start < b.start; });
    track.rebuildLookupIndex();
    return track;
}

// The part of an ASS script before its events, as FFmpeg's subtitle decoders
// hand it over for a stream inside a container.
inline AssHeader parseAssHeader(std::string_view utf8) {
    AssHeader header;
    subtitle_detail::AssReadState state;
    for (const auto& raw : subtitle_detail::splitLines(utf8ToWideText(utf8))) {
        const std::wstring_view line = subtitle_detail::trim(raw);
        if (!line.empty()) subtitle_detail::readAssHeaderLine(line, header, state);
    }
    return header;
}

// One event as those decoders hand it over, without its times:
// "ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,Effect,Text".
inline std::optional<SubtitleCue> cueFromDecodedAss(const AssHeader& header, std::string_view utf8,
                                                    double start, double end) {
    const std::wstring wide = utf8ToWideText(utf8);
    std::wstring_view rest = wide;
    std::wstring_view style;
    for (int field = 0; field < 8; ++field) {
        const auto comma = rest.find(L',');
        if (comma == std::wstring_view::npos) return std::nullopt;
        if (field == 2) style = rest.substr(0, comma);
        rest.remove_prefix(comma + 1);
    }
    return subtitle_detail::assCue(header, style, rest, start, end);
}

// Adds a cue to a track still being read -- a stream inside the container,
// decoded as the video plays -- keeping the start order and the lookup
// index. False when the track already holds it: a seek back reads the same
// packets a second time.
inline bool insertSubtitleCue(SubtitleTrack& track, SubtitleCue cue) {
    auto& cues = track.cues;
    const auto after = std::upper_bound(cues.begin(), cues.end(), cue.start,
                                        [](double value, const SubtitleCue& held) { return value < held.start; });
    for (auto it = after; it != cues.begin();) {
        --it;
        if (it->start < cue.start - 0.0005) break;
        if (it->top == cue.top && it->text == cue.text) return false;
    }
    const auto index = static_cast<std::size_t>(after - cues.begin());
    const bool indexed = track.prefixMaxEnd.size() == cues.size();
    cues.insert(after, std::move(cue));
    if (!indexed) {
        track.rebuildLookupIndex();
        return true;
    }
    track.prefixMaxEnd.resize(cues.size());
    for (std::size_t i = index; i < cues.size(); ++i) {
        track.prefixMaxEnd[i] = i == 0 ? cues[i].end : std::max(track.prefixMaxEnd[i - 1], cues[i].end);
    }
    return true;
}

// The lines on screen at one moment, by where they sit.
struct SubtitleLines {
    std::wstring bottom;
    std::wstring top;
    bool empty() const { return bottom.empty() && top.empty(); }
};

namespace subtitle_detail {

// Calls `take` for every cue on screen at `seconds`, in start order, once
// for each distinct line: a fansub script layers the same words several
// times over for an outline or a glow, and drawn as plain text those are
// one line repeated.
template <typename Take>
inline void forEachCueAt(const SubtitleTrack& track, double seconds, Take&& take) {
    const auto& cues = track.cues;
    const auto after = std::upper_bound(cues.begin(), cues.end(), seconds,
                                        [](double value, const SubtitleCue& cue) { return value < cue.start; });
    // Skip only a prefix whose cues have ALL ended. A long-running sign may
    // overlap any number of shorter dialogue cues, even far later in the file.
    // Both searches are stateless, so backward seeks need no special reset.
    auto first = cues.begin();
    if (track.prefixMaxEnd.size() == cues.size()) {
        const auto end = track.prefixMaxEnd.begin() + (after - cues.begin());
        const auto firstLive = std::upper_bound(track.prefixMaxEnd.begin(), end, seconds);
        first += firstLive - track.prefixMaxEnd.begin();
    }
    std::vector<const SubtitleCue*> shown;
    for (auto it = first; it != after; ++it) {
        if (seconds < it->start || seconds >= it->end) continue;
        const bool repeated = std::any_of(shown.begin(), shown.end(), [&](const SubtitleCue* held) {
            return held->top == it->top && held->text == it->text;
        });
        if (repeated) continue;
        shown.push_back(&*it);
        take(*it);
    }
}

}  // namespace subtitle_detail

// Every cue on screen at `seconds`, in start order, one per line.
inline std::wstring subtitleTextAt(const SubtitleTrack& track, double seconds) {
    std::wstring text;
    subtitle_detail::forEachCueAt(track, seconds, [&](const SubtitleCue& cue) {
        if (!text.empty()) text += L'\n';
        text += cue.text;
    });
    return text;
}

// The same, told apart by where the lines sit.
inline SubtitleLines subtitleLinesAt(const SubtitleTrack& track, double seconds) {
    SubtitleLines lines;
    subtitle_detail::forEachCueAt(track, seconds, [&](const SubtitleCue& cue) {
        std::wstring& target = cue.top ? lines.top : lines.bottom;
        if (!target.empty()) target += L'\n';
        target += cue.text;
    });
    return lines;
}

// ---------------------------------------------------------------------------
// What libass is given

// One event of a subtitle stream inside a container, as FFmpeg's decoders
// hand it over and libass takes it: "ReadOrder,Layer,Style,Name,MarginL,
// MarginR,MarginV,Effect,Text", with its times in milliseconds of the video.
struct SubtitleEvent {
    std::string chunk;
    long long startMs{};
    long long durationMs{};
};

// A font a video brought with it: a Matroska attachment, named as it was.
// A fansub script asks for the faces its makers used, and ships them so.
struct SubtitleFont {
    std::string name;
    std::string data;
};

// How a subtitle that brings no look of its own -- an SRT, a WebVTT, a text
// stream converted from one -- is drawn: white with a black outline at the
// bottom of the picture, in the face the language wanted reads best in, and
// on a dark box when the viewer asks for one.
struct PlainSubtitleStyle {
    std::string font{"Segoe UI"};
    bool box{};
    friend bool operator==(const PlainSubtitleStyle&, const PlainSubtitleStyle&) = default;
};

// The face for plain subtitles in the most wanted language: one whose Han
// characters are drawn the way that language writes them, and that also has
// Latin letters. Windows ships each of these.
inline std::string plainSubtitleFont(const std::vector<std::string>& wanted) {
    const std::string first = wanted.empty() ? std::string() : wanted.front();
    if (first == "zh-Hant") return "Microsoft JhengHei";
    if (first == "zh-Hans" || first == "zh") return "Microsoft YaHei";
    if (first == "ja") return "Yu Gothic";
    if (first == "ko") return "Malgun Gothic";
    return "Segoe UI";
}

// The script a plain subtitle's events go under. 384x288 to measure in, as
// FFmpeg's own stand-in header: a stream inside a file comes with sizes its
// decoder wrote for that height (`<font size=20>` as \fs20). The size, the
// outline and the height above the bottom are those the lines were drawn
// with before libass, as shares of the picture (52, 3.6 and 65 of 1080).
// The box is libass's own border style 4, as far from the letters as the
// shadow is long (and then no shadow is drawn).
inline std::string plainSubtitleHeader(const PlainSubtitleStyle& style) {
    std::string font;
    for (const char c : style.font) {
        if (c != ',' && c != '\n' && c != '\r') font += c;
    }
    if (font.empty()) font = "Segoe UI";
    return "[Script Info]\n"
           "ScriptType: v4.00+\n"
           "PlayResX: 384\n"
           "PlayResY: 288\n"
           "ScaledBorderAndShadow: yes\n"
           "WrapStyle: 0\n"
           "YCbCr Matrix: None\n"
           "\n"
           "[V4+ Styles]\n"
           "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, "
           "Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, "
           "MarginL, MarginR, MarginV, Encoding\n"
           "Style: Default," + font + ",13.87,&H00FFFFFF,&H000000FF,&H00000000,&H8C000000,600,0,0,0,100,100,0,0," +
           (style.box ? "4,0.8,3.73" : "1,0.96,0") + ",2,19,19,17,1\n"
           "\n"
           "[Events]\n"
           "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";
}

namespace subtitle_detail {

// "h:mm:ss.cc", the centiseconds ASS counts in.
inline std::string assTimestamp(double seconds) {
    long long centiseconds = std::llround(std::max(0.0, seconds) * 100.0);
    const long long hours = centiseconds / 360000;
    centiseconds %= 360000;
    const long long minutes = centiseconds / 6000;
    centiseconds %= 6000;
    char text[32]{};
    std::snprintf(text, sizeof(text), "%lld:%02lld:%02lld.%02lld", hours, minutes, centiseconds / 100,
                  centiseconds % 100);
    return text;
}

// A colour as HTML writes it -- "#FF8000", "FF8000", "red" -- as ASS's
// "&HBBGGRR&"; nothing for anything else.
inline std::optional<std::wstring> assColour(std::wstring_view value) {
    while (!value.empty() && (value.front() == L'"' || value.front() == L'\'')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == L'"' || value.back() == L'\'')) value.remove_suffix(1);
    static constexpr std::pair<std::wstring_view, std::wstring_view> names[] = {
        {L"white", L"FFFFFF"}, {L"black", L"000000"}, {L"red", L"FF0000"}, {L"lime", L"00FF00"},
        {L"green", L"008000"}, {L"blue", L"0000FF"}, {L"yellow", L"FFFF00"}, {L"cyan", L"00FFFF"},
        {L"aqua", L"00FFFF"}, {L"magenta", L"FF00FF"}, {L"fuchsia", L"FF00FF"}, {L"gray", L"808080"},
        {L"grey", L"808080"}, {L"silver", L"C0C0C0"}, {L"orange", L"FFA500"}, {L"purple", L"800080"},
        {L"pink", L"FFC0CB"}};
    const std::wstring name = lowered(value);
    for (const auto& [known, hex] : names) {
        if (name == known) { value = hex; break; }
    }
    if (!value.empty() && value.front() == L'#') value.remove_prefix(1);
    if (value.size() != 6 || !std::all_of(value.begin(), value.end(), [](wchar_t c) { return std::iswxdigit(c); })) {
        return std::nullopt;
    }
    std::wstring hex(value);
    for (auto& c : hex) c = static_cast<wchar_t>(std::towupper(c));
    return L"&H" + hex.substr(4, 2) + hex.substr(2, 2) + hex.substr(0, 2) + L"&";
}

// The value of one attribute of an HTML tag: `color` of `font color="red"`.
inline std::wstring_view tagAttribute(std::wstring_view tag, std::wstring_view name) {
    std::size_t at = 0;
    while ((at = tag.find(name, at)) != std::wstring_view::npos) {
        std::size_t i = at + name.size();
        const bool word = at == 0 || tag[at - 1] == L' ';
        while (i < tag.size() && tag[i] == L' ') ++i;
        if (word && i < tag.size() && tag[i] == L'=') {
            ++i;
            while (i < tag.size() && tag[i] == L' ') ++i;
            std::size_t end = i;
            if (end < tag.size() && (tag[end] == L'"' || tag[end] == L'\'')) {
                const wchar_t quote = tag[end];
                end = tag.find(quote, end + 1);
                end = end == std::wstring_view::npos ? tag.size() : end + 1;
            } else {
                while (end < tag.size() && tag[end] != L' ') ++end;
            }
            return tag.substr(i, end - i);
        }
        at += name.size();
    }
    return {};
}

}  // namespace subtitle_detail

namespace subtitle_detail {

// Whether `<...>` in an SRT or WebVTT cue is one of their tags -- those
// they share with HTML, WebVTT's own, an inline time -- and not text that
// happens to hold a "<" ("a<b and c>d").
inline bool knownCueTag(std::wstring_view tag) {
    if (!tag.empty() && tag.front() == L'/') tag.remove_prefix(1);
    if (!tag.empty() && std::iswdigit(tag.front())) return true;
    // Those that take nothing after their name, as they are written.
    for (const std::wstring_view bare : {L"i", L"b", L"u", L"s", L"ruby", L"rt", L"br", L"br/", L"br /"}) {
        if (tag == bare) return true;
    }
    // Those that take attributes, a voice's name or a language; and
    // WebVTT's class tag, <c> or <c.yellow>.
    for (const std::wstring_view named : {L"font", L"v", L"lang"}) {
        if (tag == named || (tag.size() > named.size() && tag.substr(0, named.size()) == named &&
                             tag[named.size()] == L' ')) {
            return true;
        }
    }
    return tag == L"c" || (tag.size() > 2 && tag[0] == L'c' && tag[1] == L'.');
}

}  // namespace subtitle_detail

// An SRT or WebVTT cue's text as an ASS event's. The tags those formats
// share with HTML become overrides (<i>, <b>, <u>, <s>, <font color face>),
// a line break is \N, an override the file carries ({\an8}) stays, and a
// brace that opens none is escaped so that libass shows it. Other tags --
// WebVTT's voices, classes and inline times -- are dropped; a "<" that opens
// no tag is text. A backslash before n or h stays a backslash ("C:\new"):
// libass would read \n and \h as a break and a hard space. \N is taken as
// meant, the line break a conversion from ASS leaves.
inline std::wstring assTextFromSrt(std::wstring_view text) {
    using namespace subtitle_detail;
    std::wstring out;
    out.reserve(text.size() + 16);
    for (std::size_t i = 0; i < text.size(); ++i) {
        const wchar_t c = text[i];
        if (c == L'<') {
            const auto close = text.find(L'>', i + 1);
            if (close != std::wstring_view::npos && close > i + 1 &&
                knownCueTag(lowered(trim(text.substr(i + 1, close - i - 1))))) {
                const std::wstring tag = lowered(trim(text.substr(i + 1, close - i - 1)));
                const std::wstring_view name = std::wstring_view(tag).substr(0, tag.find(L' '));
                if (name == L"i") out += L"{\\i1}";
                else if (name == L"/i") out += L"{\\i}";
                else if (name == L"b") out += L"{\\b700}";
                else if (name == L"/b") out += L"{\\b}";
                else if (name == L"u") out += L"{\\u1}";
                else if (name == L"/u") out += L"{\\u}";
                else if (name == L"s") out += L"{\\s1}";
                else if (name == L"/s") out += L"{\\s}";
                else if (name == L"br" || name == L"br/") out += L"\\N";
                else if (name == L"/font") out += L"{\\c\\fn}";
                else if (name == L"font") {
                    std::wstring overrides;
                    if (const auto colour = assColour(tagAttribute(tag, L"color"))) overrides += L"\\c" + *colour;
                    std::wstring_view face = tagAttribute(text.substr(i + 1, close - i - 1), L"face");
                    if (face.empty()) face = tagAttribute(text.substr(i + 1, close - i - 1), L"FACE");
                    while (!face.empty() && (face.front() == L'"' || face.front() == L'\'')) face.remove_prefix(1);
                    while (!face.empty() && (face.back() == L'"' || face.back() == L'\'')) face.remove_suffix(1);
                    if (!face.empty() && face.find_first_of(L"{}\\") == std::wstring_view::npos) {
                        overrides += L"\\fn" + std::wstring(face);
                    }
                    if (!overrides.empty()) out += L"{" + overrides + L"}";
                }
                i = close;
                continue;
            }
        } else if (c == L'{') {
            const auto close = text.find(L'}', i + 1);
            if (close != std::wstring_view::npos && close > i + 1 && text[i + 1] == L'\\') {
                out += text.substr(i, close - i + 1);
                i = close;
                continue;
            }
            out += L"\\{";
            continue;
        } else if (c == L'}') {
            out += L"\\}";
            continue;
        } else if (c == L'&') {
            const auto semicolon = text.find(L';', i + 1);
            if (semicolon != std::wstring_view::npos && semicolon - i <= 8) {
                std::wstring entity;
                appendEntity(entity, text.substr(i + 1, semicolon - i - 1));
                for (const wchar_t e : entity) {
                    if (e == L'{') out += L"\\{";
                    else if (e == L'}') out += L"\\}";
                    else out += e;
                }
                i = semicolon;
                continue;
            }
        } else if (c == L'\n') {
            out += L"\\N";
            continue;
        } else if (c == L'\r') {
            continue;
        } else if (c == L'\\' && i + 1 < text.size() && (text[i + 1] == L'n' || text[i + 1] == L'h')) {
            // A word joiner between them: drawn as nothing, read as no escape.
            out += L"\\\x2060";
            continue;
        }
        out += c;
    }
    return out;
}

// An SRT or WebVTT file as an ASS script libass can draw: the plain header,
// and each cue an event of its style. Empty when the file holds no cue.
inline std::string plainSubtitleScript(std::string_view utf8, const PlainSubtitleStyle& style) {
    if (utf8.size() >= 3 && static_cast<unsigned char>(utf8[0]) == 0xEF &&
        static_cast<unsigned char>(utf8[1]) == 0xBB && static_cast<unsigned char>(utf8[2]) == 0xBF) {
        utf8.remove_prefix(3);
    }
    std::string events;
    subtitle_detail::forEachSrtCue(
        subtitle_detail::splitLines(utf8ToWideText(utf8)), [&](double start, double end, const std::wstring& text) {
            if (start < 0.0) return;
            if (!(end > start)) end = start + 2.0;
            const std::wstring line = assTextFromSrt(text);
            if (subtitle_detail::trim(line).empty()) return;
            events += "Dialogue: 0," + subtitle_detail::assTimestamp(start) + "," +
                      subtitle_detail::assTimestamp(end) + ",Default,,0,0,0,," + wideToUtf8Text(line) + "\n";
        });
    if (events.empty()) return {};
    return plainSubtitleHeader(style) + events;
}

// The matrix an ASS script's colours were picked against ("YCbCr Matrix:"),
// and the one the video is decoded with. VSFilter, which most scripts were
// timed and coloured in, turned a script's RGB into the video's YCbCr as if
// the video were BT.601; a sign coloured to match a wall in the picture only
// matches when its colour takes the same detour (libass's ass_types.h).
enum class SubtitleMatrix { None, Bt601Tv, Bt601Pc, Bt709Tv, Bt709Pc, Smpte240mTv, Smpte240mPc, FccTv, FccPc };

namespace subtitle_detail {

struct MatrixCoefficients {
    double kr{};
    double kb{};
    bool full{};
};

inline std::optional<MatrixCoefficients> matrixCoefficients(SubtitleMatrix matrix) {
    switch (matrix) {
    case SubtitleMatrix::Bt601Tv: return MatrixCoefficients{0.299, 0.114, false};
    case SubtitleMatrix::Bt601Pc: return MatrixCoefficients{0.299, 0.114, true};
    case SubtitleMatrix::Bt709Tv: return MatrixCoefficients{0.2126, 0.0722, false};
    case SubtitleMatrix::Bt709Pc: return MatrixCoefficients{0.2126, 0.0722, true};
    case SubtitleMatrix::Smpte240mTv: return MatrixCoefficients{0.212, 0.087, false};
    case SubtitleMatrix::Smpte240mPc: return MatrixCoefficients{0.212, 0.087, true};
    case SubtitleMatrix::FccTv: return MatrixCoefficients{0.30, 0.11, false};
    case SubtitleMatrix::FccPc: return MatrixCoefficients{0.30, 0.11, true};
    default: return std::nullopt;
    }
}

}  // namespace subtitle_detail

// A script colour (0xRRGGBB) as it shows on a video in `video`: into YCbCr
// by the script's matrix, back out by the video's. Unchanged when either
// says None or both are the same.
inline std::uint32_t subtitleColourForVideo(std::uint32_t rgb, SubtitleMatrix script, SubtitleMatrix video) {
    if (script == video) return rgb;
    const auto from = subtitle_detail::matrixCoefficients(script);
    const auto to = subtitle_detail::matrixCoefficients(video);
    if (!from || !to) return rgb;
    const double r = static_cast<double>((rgb >> 16) & 0xFF) / 255.0;
    const double g = static_cast<double>((rgb >> 8) & 0xFF) / 255.0;
    const double b = static_cast<double>(rgb & 0xFF) / 255.0;
    // Into Y'CbCr, as the script's matrix and range write it ...
    const double kg = 1.0 - from->kr - from->kb;
    const double luma = from->kr * r + kg * g + from->kb * b;
    const double blue = (b - luma) / (2.0 * (1.0 - from->kb));
    const double red = (r - luma) / (2.0 * (1.0 - from->kr));
    const double codeY = from->full ? luma * 255.0 : 16.0 + 219.0 * luma;
    const double codeB = 128.0 + (from->full ? 255.0 : 224.0) * blue;
    const double codeR = 128.0 + (from->full ? 255.0 : 224.0) * red;
    // ... and those codes read back as the video's.
    const double y = to->full ? codeY / 255.0 : (codeY - 16.0) / 219.0;
    const double cb = (codeB - 128.0) / (to->full ? 255.0 : 224.0);
    const double cr = (codeR - 128.0) / (to->full ? 255.0 : 224.0);
    const double outR = y + 2.0 * (1.0 - to->kr) * cr;
    const double outB = y + 2.0 * (1.0 - to->kb) * cb;
    const double outG = (y - to->kr * outR - to->kb * outB) / (1.0 - to->kr - to->kb);
    const auto channel = [](double value) {
        return static_cast<std::uint32_t>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
    };
    return (channel(outR) << 16) | (channel(outG) << 8) | channel(outB);
}

// ---------------------------------------------------------------------------
// Which of several subtitles to show

// The language a viewer asks for. Auto is what Windows says they read.
enum class SubtitleLanguage { Auto, ChineseSimplified, ChineseTraditional, English, Japanese, Korean };
inline constexpr int kSubtitleLanguageCount = 6;

inline SubtitleLanguage clampSubtitleLanguage(int value) {
    return static_cast<SubtitleLanguage>(std::clamp(value, 0, kSubtitleLanguageCount - 1));
}

// Each in its own writing, as a language picker shows them.
inline const wchar_t* subtitleLanguageName(SubtitleLanguage language) {
    switch (language) {
    case SubtitleLanguage::ChineseSimplified: return L"\x7B80\x4F53\x4E2D\x6587";
    case SubtitleLanguage::ChineseTraditional: return L"\x7E41\x9AD4\x4E2D\x6587";
    case SubtitleLanguage::English: return L"English";
    case SubtitleLanguage::Japanese: return L"\x65E5\x672C\x8A9E";
    case SubtitleLanguage::Korean: return L"\xD55C\xAD6D\xC5B4";
    default: return L"Auto";
    }
}

namespace subtitle_detail {

// What a title or a file name's suffix says of the language: "SC", "chs",
// "unibig5", "Chinese Simplified (SSA)", a name in its own script.
inline std::string languageFromHint(std::wstring_view hint) {
    const std::wstring text = lowered(hint);
    std::vector<std::wstring> tokens;
    std::wstring token;
    for (const wchar_t c : text) {
        if ((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9')) { token += c; continue; }
        if (!token.empty()) tokens.push_back(std::move(token));
        token.clear();
    }
    if (!token.empty()) tokens.push_back(std::move(token));
    const auto any = [&](std::initializer_list<std::wstring_view> wanted) {
        return std::any_of(wanted.begin(), wanted.end(), [&](std::wstring_view one) {
            return std::find(tokens.begin(), tokens.end(), one) != tokens.end();
        });
    };
    const auto holds = [&](std::wstring_view part) { return text.find(part) != std::wstring::npos; };
    if (any({L"chs", L"sc", L"gb", L"gbk", L"hans", L"jpsc", L"scjp", L"simplified"}) ||
        holds(L"\x7B80") || holds(L"\x7C21") || holds(L"gb2312")) return "zh-Hans";
    if (any({L"cht", L"tc", L"hant", L"jptc", L"tcjp", L"traditional"}) || holds(L"big5") ||
        holds(L"\x7E41")) return "zh-Hant";
    if (any({L"chi", L"zho", L"zh", L"cn", L"chinese", L"mandarin", L"cantonese"}) || holds(L"\x4E2D")) return "zh";
    if (any({L"eng", L"en", L"english"}) || holds(L"\x82F1")) return "en";
    if (any({L"jpn", L"ja", L"jp", L"japanese"}) || holds(L"\x65E5")) return "ja";
    if (any({L"kor", L"ko", L"korean"}) || holds(L"\x97E9") || holds(L"\x97D3") || holds(L"\xD55C")) return "ko";
    return {};
}

}  // namespace subtitle_detail

// A track's language as a short tag -- "zh-Hans", "zh-Hant", "zh" when the
// script is not said, "en", "ja", ... -- from the code a container or a
// server gives ("chi", "zh-CN", "eng", a Windows language name) and, where
// that leaves it open, from the track's title. Empty for unknown.
inline std::string subtitleLanguageTag(std::string_view code, std::wstring_view hint = {}) {
    std::string lower(code);
    for (auto& c : lower) {
        c = c == '_' ? '-' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    const std::string primary = lower.substr(0, lower.find('-'));
    const auto has = [&](std::string_view part) { return lower.find(part) != std::string::npos; };
    if (primary == "zh" || primary == "chi" || primary == "zho" || primary == "cmn" || primary == "chs" ||
        primary == "cht" || primary == "yue") {
        if (has("hant") || has("-tw") || has("-hk") || has("-mo") || primary == "cht") return "zh-Hant";
        if (has("hans") || has("-cn") || has("-sg") || primary == "chs") return "zh-Hans";
        const std::string hinted = subtitle_detail::languageFromHint(hint);
        return hinted == "zh-Hans" || hinted == "zh-Hant" ? hinted : "zh";
    }
    static constexpr std::pair<std::string_view, std::string_view> names[] = {
        {"eng", "en"}, {"jpn", "ja"}, {"kor", "ko"}, {"fre", "fr"}, {"fra", "fr"}, {"ger", "de"},
        {"deu", "de"}, {"spa", "es"}, {"ita", "it"}, {"rus", "ru"}, {"por", "pt"}, {"tha", "th"},
        {"vie", "vi"}, {"ara", "ar"}, {"ind", "id"}, {"may", "ms"}, {"msa", "ms"}, {"dut", "nl"},
        {"nld", "nl"}, {"pol", "pl"}, {"tur", "tr"}, {"swe", "sv"}, {"dan", "da"}, {"nor", "no"},
        {"nob", "no"}, {"fin", "fi"}, {"hin", "hi"}, {"ukr", "uk"}, {"cze", "cs"}, {"ces", "cs"},
        {"hun", "hu"}, {"gre", "el"}, {"ell", "el"}, {"heb", "he"}};
    for (const auto& [three, two] : names) {
        if (primary == three) return std::string(two);
    }
    if (primary.size() == 2) return primary;
    // "und", nothing at all, or a code that is not one: the title may say.
    return subtitle_detail::languageFromHint(hint);
}

// The tags to look for, most wanted first. An explicit choice comes before
// what Windows lists; of Windows' own list the languages other than English
// come first -- someone who added another language to an English Windows
// reads it, and English is on every list.
inline std::vector<std::string> subtitleLanguagePreference(SubtitleLanguage choice,
                                                            const std::vector<std::string>& systemLanguages) {
    std::vector<std::string> wanted;
    const auto add = [&](const std::string& tag) {
        if (!tag.empty() && std::find(wanted.begin(), wanted.end(), tag) == wanted.end()) wanted.push_back(tag);
    };
    switch (choice) {
    case SubtitleLanguage::ChineseSimplified: add("zh-Hans"); break;
    case SubtitleLanguage::ChineseTraditional: add("zh-Hant"); break;
    case SubtitleLanguage::English: add("en"); break;
    case SubtitleLanguage::Japanese: add("ja"); break;
    case SubtitleLanguage::Korean: add("ko"); break;
    default: break;
    }
    std::vector<std::string> system;
    for (const auto& language : systemLanguages) system.push_back(subtitleLanguageTag(language));
    for (const auto& tag : system) {
        if (tag != "en") add(tag);
    }
    for (const auto& tag : system) {
        if (tag == "en") add(tag);
    }
    if (wanted.empty()) add("en");
    return wanted;
}

// One of the subtitles a video offers, as far as choosing goes.
struct SubtitleCandidate {
    std::string language;  // the container's or the server's code
    std::wstring title;    // the track's title, or a file name's suffix
    bool isDefault{};
    bool forced{};         // only the lines the audio leaves untranslated
    bool external{};       // a file beside the video, not a stream inside it
};

// How well a candidate suits the wanted languages, 0 best. A wanted
// language ranks by its place in the list, the same language in another or
// an unsaid script just behind the exact one. A file beside the video that
// names no language was put there by the viewer and comes right after the
// first wanted language; a stream that names none comes after every wanted
// language, and a language nobody asked for last.
inline int subtitleLanguageRank(const SubtitleCandidate& candidate, const std::vector<std::string>& wanted) {
    const std::string tag = subtitleLanguageTag(candidate.language, candidate.title);
    const int last = static_cast<int>(wanted.size()) * 4;
    if (tag.empty()) return candidate.external ? 3 : last;
    const std::string language = tag.substr(0, tag.find('-'));
    for (std::size_t i = 0; i < wanted.size(); ++i) {
        const int base = static_cast<int>(i) * 4;
        if (wanted[i] == tag) return base;
        if (wanted[i].substr(0, wanted[i].find('-')) != language) continue;
        return tag == language || wanted[i] == language ? base + 1 : base + 2;
    }
    return last + 1;
}

// The candidates' places in their list, most suitable first: the best
// language, then a full track before a forced one, then the one marked
// default, then a file before a stream, then the order they came in.
inline std::vector<std::size_t> rankSubtitles(const std::vector<SubtitleCandidate>& candidates,
                                              const std::vector<std::string>& wanted) {
    std::vector<std::array<int, 4>> keys;
    keys.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        keys.push_back({subtitleLanguageRank(candidate, wanted), candidate.forced ? 1 : 0,
                        candidate.isDefault ? 0 : 1, candidate.external ? 0 : 1});
    }
    std::vector<std::size_t> order(candidates.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(),
                     [&](std::size_t left, std::size_t right) { return keys[left] < keys[right]; });
    return order;
}

// The candidate to show when the viewer has chosen none, -1 for an empty list.
inline int chooseSubtitle(const std::vector<SubtitleCandidate>& candidates, const std::vector<std::string>& wanted) {
    const auto order = rankSubtitles(candidates, wanted);
    return order.empty() ? -1 : static_cast<int>(order.front());
}

}  // namespace quaddeck
