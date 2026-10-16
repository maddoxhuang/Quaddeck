#pragma once

// Dark popup menus.
//
// The player is dark everywhere -- title bar, dock, settings -- except the
// right-click menu, which Windows drew as a white system menu over the video.
// Windows has rendered dark menus for Win32 applications since 10.0.18362,
// but only exposes the switch through two unnamed uxtheme exports, ordinals
// 135 and 136, which is how Explorer and Notepad turn theirs on. They have
// kept their numbers since 1809; anything older or a build where the lookup
// fails simply keeps the light menu, so the player never depends on them.

#include <windows.h>

namespace quaddeck {

inline void enableDarkPopupMenus() {
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto rtlGetVersion = ntdll
        ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
    if (!rtlGetVersion || rtlGetVersion(&version) != 0) return;
    if (version.dwMajorVersion < 10 || version.dwBuildNumber < 18362) return;

    const HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!uxtheme) return;
    // 135: SetPreferredAppMode(PreferredAppMode) -> previous mode. 2 = ForceDark.
    // 136: FlushMenuThemes().
    using SetPreferredAppModeFn = int(WINAPI*)(int);
    using FlushMenuThemesFn = void(WINAPI*)();
    const auto setPreferredAppMode = reinterpret_cast<SetPreferredAppModeFn>(
        GetProcAddress(uxtheme, MAKEINTRESOURCEA(135)));
    const auto flushMenuThemes = reinterpret_cast<FlushMenuThemesFn>(
        GetProcAddress(uxtheme, MAKEINTRESOURCEA(136)));
    if (setPreferredAppMode) setPreferredAppMode(2);
    if (flushMenuThemes) flushMenuThemes();
    // uxtheme stays loaded for the process lifetime; the theme state lives in it.
}

}  // namespace quaddeck
