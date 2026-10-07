#include "FilePersistence.hpp"
#include "Session.hpp"

#include <cassert>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>

#include <windows.h>

using namespace quaddeck;

namespace {

struct TemporaryDirectory {
    std::filesystem::path path;

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

std::string readBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

template <typename State, typename Reader>
State readWideDocument(const std::filesystem::path& path, Reader reader) {
    std::wstring wide;
    assert(readUtf8File(path, wide));
    std::wistringstream input(wide);
    input.imbue(std::locale::classic());
    State state;
    assert(reader(input, state));
    return state;
}

bool hasTransactionArtifact(const std::filesystem::path& directory) {
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        const auto name = entry.path().filename().wstring();
        if (name.find(L".tmp.") != std::wstring::npos ||
            name.find(L".rollback.") != std::wstring::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    TemporaryDirectory temporary{
        std::filesystem::temp_directory_path() /
        (L"QuadDeck-persistence-tests-" + std::to_wstring(nonce))};
    std::filesystem::create_directories(temporary.path);

    const auto textPath = temporary.path / L"atomic.txt";
    assert(writeUtf8FileAtomically(
        textPath, [](std::wostream& output) {
            output << L"first complete document\n";
            return output.good();
        }));
    assert(readBytes(textPath) == "first complete document\n");

    assert(writeUtf8FileAtomically(
        textPath, [](std::wostream& output) {
            output << L"replacement\n";
            return output.good();
        }));
    assert(readBytes(textPath) == "replacement\n");
    assert(!hasTransactionArtifact(temporary.path));

    assert(!writeUtf8FileAtomically(
        textPath, [](std::wostream& output) {
            output << L"partial document";
            return false;
        }));
    assert(readBytes(textPath) == "replacement\n");
    assert(!writeUtf8FileAtomically(
        textPath, [](std::wostream&) -> bool {
            throw std::runtime_error("synthetic serializer failure");
        }));
    assert(readBytes(textPath) == "replacement\n");
    assert(!hasTransactionArtifact(temporary.path));

    const auto unicodePath = temporary.path / L"保存-✓.txt";
    assert(writeUtf8FileAtomically(
        unicodePath, [](std::wostream& output) {
            output << L"QuadDeck 保存 ✓\n";
            return output.good();
        }));
    const std::u8string expectedUnicode = u8"QuadDeck 保存 ✓\n";
    assert(readBytes(unicodePath) == std::string(
        reinterpret_cast<const char*>(expectedUnicode.data()), expectedUnicode.size()));
    std::wstring loadedUnicode;
    assert(readUtf8File(unicodePath, loadedUnicode));
    assert(loadedUnicode == L"QuadDeck 保存 ✓\n");

    const auto invalidUtf8Path = temporary.path / L"invalid-utf8.qconfig";
    {
        std::ofstream invalid(invalidUtf8Path, std::ios::binary);
        invalid << "QCONFIG 5\n\xFF";
    }
    std::wstring rejectedText;
    assert(!readUtf8File(invalidUtf8Path, rejectedText));

    const auto bomPath = temporary.path / L"utf8-bom.qconfig";
    {
        std::ofstream bom(bomPath, std::ios::binary);
        const unsigned char marker[]{0xEFU, 0xBBU, 0xBFU};
        bom.write(reinterpret_cast<const char*>(marker), sizeof(marker));
        bom << "QCONFIG 5\n";
    }
    std::wstring bomText;
    assert(readUtf8File(bomPath, bomText));
    assert(bomText == L"QCONFIG 5\n");

    const auto oversizedPath = temporary.path / L"oversized.qconfig";
    {
        std::ofstream oversized(oversizedPath, std::ios::binary);
        oversized.seekp(16LL * 1024LL * 1024LL);
        oversized.put('x');
    }
    assert(!readUtf8File(oversizedPath, rejectedText));

    SessionState session;
    session.timeline = 12.5;
    session.playing = true;
    session.audioMask = 0x15U;
    session.panes[4].path = L"C:\\影片\\第五路.mp4";
    session.panes[4].repeat = true;
    session.panes[4].volume = 0.42F;
    const auto sessionPath = temporary.path / L"roundtrip.qdeck";
    assert(writeUtf8FileAtomically(
        sessionPath,
        [&](std::wostream& output) { return writeSession(output, session); }));
    const auto loadedSession = readWideDocument<SessionState>(sessionPath, readSession);
    assert(loadedSession.timeline == session.timeline);
    assert(loadedSession.audioMask == session.audioMask);
    assert(loadedSession.panes[4].path == session.panes[4].path);
    assert(loadedSession.panes[4].repeat);
    assert(std::abs(loadedSession.panes[4].volume - 0.42F) < 0.001F);

    StyleState style;
    style.layout = LayoutMode::LandscapePair;
    style.customShaderPath = L"C:\\着色器\\鲜艳.hlsl";
    style.paneViews[3].mode = ViewMode::Fill;
    const auto stylePath = temporary.path / L"roundtrip.qstyle";
    assert(writeUtf8FileAtomically(
        stylePath, [&](std::wostream& output) { return writeStyle(output, style); }));
    const auto loadedStyle = readWideDocument<StyleState>(stylePath, readStyle);
    assert(loadedStyle.layout == style.layout);
    assert(loadedStyle.customShaderPath == style.customShaderPath);
    assert(loadedStyle.paneViews[3].mode == ViewMode::Fill);

    AppSettings settings;
    settings.audioMask = kAllPaneMask;
    settings.paneRepeat[2] = true;
    settings.paneVolume[2] = 0.37F;
    settings.paneMuted[4] = true;
    const auto serverId = wideToUtf8Text(L"\u4F3A\u670D\u5668 \"one\" \\ \U0001F680");
    const auto libraryId = wideToUtf8Text(L"001 \u7535\u5F71 \"library\" \\ ");
    assert(setEmbyLibraryDetailsEnabled(settings.embyLibraries, serverId, libraryId, false));
    assert(setEmbyLibraryDetailsEnabled(settings.embyLibraries, "other-server", libraryId, true));
    const auto settingsPath = temporary.path / L"roundtrip.qconfig";
    assert(writeUtf8FileAtomically(
        settingsPath,
        [&](std::wostream& output) { return writeAppSettings(output, settings); }));
    const auto loadedSettings = readWideDocument<AppSettings>(settingsPath, readAppSettings);
    assert(loadedSettings.audioMask == kAllPaneMask);
    assert(loadedSettings.paneRepeat[2]);
    assert(std::abs(loadedSettings.paneVolume[2] - 0.37F) < 0.001F);
    assert(loadedSettings.paneMuted[4]);
    assert(loadedSettings.embyLibraries.entries.size() == 2);
    assert(!embyLibraryDetailsEnabled(loadedSettings.embyLibraries, serverId, libraryId, "movies"));
    assert(embyLibraryDetailsEnabled(loadedSettings.embyLibraries, "other-server", libraryId, "movies"));
    assert(readBytes(settingsPath).starts_with("QCONFIG 14\n"));
    const auto savedSettings = readBytes(settingsPath);
    auto invalidSettings = settings;
    invalidSettings.embyLibraries.entries.push_back(invalidSettings.embyLibraries.entries.front());
    assert(!writeUtf8FileAtomically(settingsPath,
        [&](std::wostream& output) { return writeAppSettings(output, invalidSettings); }));
    assert(readBytes(settingsPath) == savedSettings && !hasTransactionArtifact(temporary.path));

    // A reader that does not share DELETE makes ReplaceFileW fail
    // deterministically. The old document must survive byte-for-byte and the
    // temporary transaction file must be removed.
    const HANDLE lock = CreateFileW(
        textPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(lock != INVALID_HANDLE_VALUE);
    assert(!writeUtf8FileAtomically(
        textPath, [](std::wostream& output) {
            output << L"must not replace locked target\n";
            return output.good();
        }));
    CloseHandle(lock);
    assert(readBytes(textPath) == "replacement\n");
    assert(!hasTransactionArtifact(temporary.path));

    return 0;
}
