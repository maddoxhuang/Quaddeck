#include "FileAssociations.hpp"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cwchar>
#include <vector>

namespace quaddeck {

namespace {

std::wstring describe(const wchar_t* what, const std::wstring& key, LSTATUS status) {
    return std::wstring(what) + L" " + key + L" failed (error " + std::to_wstring(status) + L")";
}

bool writeValue(const RegistryValue& value, std::wstring& error) {
    HKEY key = nullptr;
    LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, value.key.c_str(), 0, nullptr, 0, KEY_SET_VALUE,
                                     nullptr, &key, nullptr);
    if (status != ERROR_SUCCESS) {
        error = describe(L"Creating", value.key, status);
        return false;
    }
    const auto bytes = static_cast<DWORD>((value.data.size() + 1) * sizeof(wchar_t));
    status = RegSetValueExW(key, value.name.empty() ? nullptr : value.name.c_str(), 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(value.data.c_str()), bytes);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS) {
        error = describe(L"Writing", value.key, status);
        return false;
    }
    return true;
}

// One value under HKEY_CURRENT_USER: its text, empty for a value of another
// type, nothing when it is not there.
std::optional<std::wstring> readValue(const std::wstring& key, const std::wstring& name) {
    const wchar_t* valueName = name.empty() ? nullptr : name.c_str();
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), valueName, RRF_RT_ANY | RRF_NOEXPAND, &type, nullptr,
                     &bytes) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    if (type != REG_SZ && type != REG_EXPAND_SZ) return std::wstring();
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1);
    bytes = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), valueName, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
                     nullptr, buffer.data(), &bytes) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    return std::wstring(buffer.data());
}

void notifyShell() {
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

}  // namespace

std::wstring currentExecutablePath() {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) return {};
        if (length < buffer.size() - 1) return std::wstring(buffer.data(), length);
        if (buffer.size() > 32768) return {};
        buffer.resize(buffer.size() * 2);
    }
}

bool registerFileAssociations(const std::wstring& executable, std::wstring& error) {
    if (executable.empty()) {
        error = L"The player's own path is unknown";
        return false;
    }
    const AssociationPlan plan = fileAssociationPlan(executable);
    for (const auto& value : plan.values) {
        if (!writeValue(value, error)) return false;
    }
    notifyShell();
    return true;
}

bool unregisterFileAssociations(std::wstring& error) {
    const AssociationPlan plan = fileAssociationPlan(L"QuadDeck.exe");
    bool complete = true;
    for (const auto& value : plan.sharedValues) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, value.key.c_str(), 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
            continue;   // never written, or already gone
        }
        const LSTATUS status = RegDeleteValueW(key, value.name.c_str());
        RegCloseKey(key);
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
            error = describe(L"Removing from", value.key, status);
            complete = false;
        }
    }
    for (const auto& key : plan.ownedKeys) {
        const LSTATUS status = RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
            error = describe(L"Removing", key, status);
            complete = false;
        }
    }
    notifyShell();
    return complete;
}

AssociationStatus fileAssociationStatus(const std::wstring& executable) {
    return associationStatusFrom(executable, readValue);
}

void openDefaultAppsSettings() {
    // Windows 11 opens the page for this application; Windows 10 ignores the
    // argument and opens Default apps.
    ShellExecuteW(nullptr, L"open", L"ms-settings:defaultapps?registeredAppUser=QuadDeck", nullptr, nullptr,
                  SW_SHOWNORMAL);
}

}  // namespace quaddeck
