#include "App.hpp"
#include "SingleInstance.hpp"

#include <shellapi.h>
#include <windows.h>
#include <commctrl.h>

#include <string>
#include <vector>

namespace {
// Without this the process is DPI-unaware: on a 200% display Windows hands
// it a 1920x1080 client for a 3840x2160 screen and bitmap-stretches the
// result, so a 4K source is rendered at half resolution and scaled back up.
// Per-monitor v2 gives the swap chain the real pixels; App scales its own
// fonts and control sizes from the window's DPI. Set before any window is
// created; the manifest-free lookup keeps builds before 1703 running unaware.
void declareDpiAwareness() {
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    using SetContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    const auto setContext = user32 ? reinterpret_cast<SetContextFn>(
        GetProcAddress(user32, "SetProcessDpiAwarenessContext")) : nullptr;
    if (setContext && setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return;
    SetProcessDPIAware();
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    declareDpiAwareness();
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int argumentCount = 0;
    PWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (!arguments) {
        MessageBoxW(nullptr, L"QuadDeck could not read the launch arguments.",
                    L"QuadDeck", MB_OK | MB_ICONERROR);
        if (SUCCEEDED(comResult)) CoUninitialize();
        return 1;
    }
    // For an installer or a script: register or remove the file types and
    // leave, without a window.
    if (arguments && argumentCount == 2) {
        const std::wstring flag = arguments[1];
        if (flag == L"--register-file-types" || flag == L"--unregister-file-types") {
            std::wstring error;
            const bool done = flag == L"--register-file-types"
                ? quaddeck::registerFileAssociations(quaddeck::currentExecutablePath(), error)
                : quaddeck::unregisterFileAssociations(error);
            LocalFree(arguments);
            if (SUCCEEDED(comResult)) CoUninitialize();
            return done ? 0 : 1;
        }
    }
    std::vector<std::wstring> files;
    std::wstring skipped;
    std::size_t skippedCount = 0;
    if (arguments) {
        for (int index = 1; index < argumentCount; ++index) {
            // One item the handoff cannot carry (a URL, a switch, a device)
            // is left out rather than costing the whole launch its window.
            std::wstring normalized, reason;
            if (quaddeck::SingleInstance::normalizeLocalPath(arguments[index], normalized, reason)) {
                files.push_back(std::move(normalized));
            } else if (++skippedCount <= 5) {
                skipped += L"\n" + std::wstring(arguments[index]);
            }
        }
        LocalFree(arguments);
    }
    if (skippedCount > 0) {
        if (skippedCount > 5) skipped += L"\n(" + std::to_wstring(skippedCount - 5) + L" more)";
        const std::wstring message =
            L"QuadDeck opens files on a drive or a network share. These were left out:" + skipped;
        MessageBoxW(nullptr, message.c_str(), L"QuadDeck", MB_OK | MB_ICONWARNING);
    }
    quaddeck::SingleInstance::Request request;
    std::wstring error;
    quaddeck::SingleInstance singleInstance;
    if (!quaddeck::SingleInstance::prepareRequest(files, request, error)) {
        MessageBoxW(nullptr, error.c_str(), L"QuadDeck launch failed", MB_OK | MB_ICONERROR);
        if (SUCCEEDED(comResult)) CoUninitialize();
        return 1;
    }
    const auto launch = singleInstance.launch(request, error);
    if (launch != quaddeck::SingleInstance::LaunchResult::Primary) {
        if (launch == quaddeck::SingleInstance::LaunchResult::Error)
            MessageBoxW(nullptr, error.c_str(), L"QuadDeck launch failed", MB_OK | MB_ICONERROR);
        if (SUCCEEDED(comResult)) CoUninitialize();
        return launch == quaddeck::SingleInstance::LaunchResult::Forwarded ? 0 : 1;
    }
    quaddeck::App app;
    const int result = app.run(instance, showCommand, request.files, &singleInstance);
    singleInstance.stopAccepting();
    if (SUCCEEDED(comResult)) {
        CoUninitialize();
    }
    return result;
}
