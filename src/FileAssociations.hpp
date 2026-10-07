#pragma once

// Opening videos and their subtitles with QuadDeck from Explorer. The
// player registers itself for the current user as a program that can open
// its file types -- "Open with" and the Default apps page then offer it --
// and Windows keeps the choice of default to the user, which is why
// registering ends on that page rather than taking the types over.
// Everything is under HKCU: no elevation, nothing machine-wide.
//
// The plan is pure so it can be tested; applying it (FileAssociations.cpp)
// is only ever done at the user's request, never by a test.

#include <array>
#include <cwchar>
#include <optional>
#include <string>
#include <vector>

namespace quaddeck {

inline constexpr std::array<const wchar_t*, 9> kVideoExtensions{
    L".mp4", L".mkv", L".mov", L".avi", L".webm", L".m4v", L".ts", L".wmv", L".flv"};
// The text subtitles the player reads, beside a video or opened by hand.
inline constexpr std::array<const wchar_t*, 4> kSubtitleExtensions{L".ass", L".ssa", L".srt", L".vtt"};

inline constexpr const wchar_t* kVideoProgId = L"QuadDeck.Video";
inline constexpr const wchar_t* kSubtitleProgId = L"QuadDeck.Subtitle";
inline constexpr const wchar_t* kSessionProgId = L"QuadDeck.Session";
inline constexpr const wchar_t* kSessionExtension = L".qdeck";
inline constexpr const wchar_t* kRegisteredAppName = L"QuadDeck";
inline constexpr const wchar_t* kCapabilitiesKey = L"Software\\QuadDeck\\Capabilities";

// One string value under HKEY_CURRENT_USER; an empty name is the key's default.
struct RegistryValue {
    std::wstring key;
    std::wstring name;
    std::wstring data;
    friend bool operator==(const RegistryValue&, const RegistryValue&) = default;
};

struct AssociationPlan {
    // Written when registering.
    std::vector<RegistryValue> values;
    // Removed when unregistering: whole keys of ours, and the single values
    // we added to keys that belong to the file types.
    std::vector<std::wstring> ownedKeys;
    std::vector<RegistryValue> sharedValues;   // data unused
};

inline std::wstring openCommand(const std::wstring& executable) {
    return L"\"" + executable + L"\" \"%1\"";
}

inline AssociationPlan fileAssociationPlan(const std::wstring& executable) {
    AssociationPlan plan;
    const std::wstring classes = L"Software\\Classes\\";
    const std::wstring command = openCommand(executable);
    const std::wstring icon = L"\"" + executable + L"\",0";
    const auto progId = [&](const wchar_t* id, const wchar_t* description) {
        const std::wstring key = classes + id;
        plan.values.push_back({key, L"", description});
        plan.values.push_back({key + L"\\DefaultIcon", L"", icon});
        plan.values.push_back({key + L"\\shell\\open", L"FriendlyAppName", kRegisteredAppName});
        plan.values.push_back({key + L"\\shell\\open\\command", L"", command});
        plan.ownedKeys.push_back(key);
    };
    progId(kVideoProgId, L"Video");
    progId(kSubtitleProgId, L"Subtitles");
    progId(kSessionProgId, L"QuadDeck session");

    const std::wstring application = classes + L"Applications\\QuadDeck.exe";
    plan.values.push_back({application, L"FriendlyAppName", kRegisteredAppName});
    plan.values.push_back({application + L"\\shell\\open\\command", L"", command});
    plan.ownedKeys.push_back(application);

    const std::wstring capabilities = kCapabilitiesKey;
    plan.values.push_back({capabilities, L"ApplicationName", kRegisteredAppName});
    plan.values.push_back({capabilities, L"ApplicationDescription",
                           L"Synchronized multi-video player; plays one video with its folder as the list."});
    plan.ownedKeys.push_back(capabilities);

    const auto type = [&](const std::wstring& extension, const wchar_t* id) {
        plan.values.push_back({classes + extension + L"\\OpenWithProgids", id, L""});
        plan.sharedValues.push_back({classes + extension + L"\\OpenWithProgids", id, L""});
        plan.values.push_back({application + L"\\SupportedTypes", extension, L""});
        plan.values.push_back({capabilities + L"\\FileAssociations", extension, id});
    };
    for (const wchar_t* extension : kVideoExtensions) type(extension, kVideoProgId);
    for (const wchar_t* extension : kSubtitleExtensions) type(extension, kSubtitleProgId);
    type(kSessionExtension, kSessionProgId);

    plan.values.push_back({L"Software\\RegisteredApplications", kRegisteredAppName, capabilities});
    plan.sharedValues.push_back({L"Software\\RegisteredApplications", kRegisteredAppName, L""});
    return plan;
}

// Whether, and for which executable, the types are registered. Incomplete
// is a registration for this executable that lacks part of the plan --
// made by an older build, before the subtitle types were added.
enum class AssociationState { NotRegistered, Registered, RegisteredElsewhere, Incomplete };

struct AssociationStatus {
    AssociationState state{AssociationState::NotRegistered};
    // The command found in the registry when it names another executable.
    std::wstring command;
};

// The status from what the registry holds. `read(key, name)` is one value
// under HKEY_CURRENT_USER, empty for a value without text and nothing for
// one that is not there.
template <typename Read>
AssociationStatus associationStatusFrom(const std::wstring& executable, Read&& read) {
    AssociationStatus status;
    const std::optional<std::wstring> command =
        read(std::wstring(L"Software\\Classes\\") + kVideoProgId + L"\\shell\\open\\command", std::wstring());
    const std::optional<std::wstring> registered =
        read(std::wstring(L"Software\\RegisteredApplications"), std::wstring(kRegisteredAppName));
    if (!command || command->empty() || !registered || registered->empty()) return status;
    if (_wcsicmp(command->c_str(), openCommand(executable).c_str()) != 0) {
        status.state = AssociationState::RegisteredElsewhere;
        status.command = *command;
        return status;
    }
    for (const auto& value : fileAssociationPlan(executable).values) {
        const std::optional<std::wstring> found = read(value.key, value.name);
        if (!found || _wcsicmp(found->c_str(), value.data.c_str()) != 0) {
            status.state = AssociationState::Incomplete;
            return status;
        }
    }
    status.state = AssociationState::Registered;
    return status;
}

inline std::wstring associationNote(const AssociationStatus& status) {
    switch (status.state) {
    case AssociationState::Registered:
        return L"Registered. Choose QuadDeck per file type in Windows' Default apps.";
    case AssociationState::RegisteredElsewhere:
        return L"Registered for another copy of QuadDeck; Register points it here.";
    case AssociationState::Incomplete:
        return L"Registered by an older QuadDeck; Register adds subtitle files.";
    default:
        return L"Not registered: Explorer does not offer QuadDeck for videos.";
    }
}

// FileAssociations.cpp. `error` says what failed, in words for the notice.
bool registerFileAssociations(const std::wstring& executable, std::wstring& error);
bool unregisterFileAssociations(std::wstring& error);
AssociationStatus fileAssociationStatus(const std::wstring& executable);
// Windows' Default apps page for QuadDeck, where the user makes it the default.
void openDefaultAppsSettings();
std::wstring currentExecutablePath();

}  // namespace quaddeck
