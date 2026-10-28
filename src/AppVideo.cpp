// App: the picture's way to the screen, PotPlayer's video output settings as
// far as one D3D11 swap chain for every pane allows. The renderer and the
// graphics card it runs on -- a change of either builds the device again, as
// a lost one is, and every video is opened again on it -- the display
// fullscreen goes to, the output's depth, deinterlacing, the scaler that
// enlarges the picture, the levels and the colour controls. The Video tab,
// the picture menu and keys all act through here; D3DRenderer does the work
// and VideoDevices lists the cards and displays.

#include "App.hpp"
#include "AppInternal.hpp"
#include "Diagnostics.hpp"
#include "FilePersistence.hpp"
#include "TextEncoding.hpp"
#include "VideoDevices.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace quaddeck {

using namespace app_internal;

// ---------------------------------------------------------------------------
// The settings

void App::primeVideoOutput() {
    // The rest of the settings are read once the window is there, but the
    // renderer is built with the window: its own part is read now, so it is
    // built as chosen rather than built and at once built again.
    const auto path = settingsPath();
    std::error_code missing;
    std::wstring text;
    if (path.empty() || !std::filesystem::exists(path, missing) || !readUtf8File(path, text)) return;
    std::wistringstream input(text);
    input.imbue(std::locale::classic());
    AppSettings state;
    if (!readAppSettings(input, state)) return;
    videoOutput_ = clampVideoOutputSettings(state.video);
    renderer_.setVideoOutput(videoOutput_);
}

void App::applyVideoOutput() {
    if (!renderer_.setVideoOutput(videoOutput_)) return;
    // Another renderer or card: the device is built again on the next frame,
    // by the lost device's recovery, with every video opened again where it
    // was. The backoff of an earlier failure does not hold it up.
    const auto choice = videoRendererChoice(videoOutput_);
    appendDiagnostic("Renderer: building again as " + wideToUtf8Text(videoRendererLabel(choice.renderer)) +
                     (choice.adapter.empty() ? std::string{} : " on " + wideToUtf8Text(choice.adapter)));
    renderer_.requestRebuild();
    nextDeviceRecovery_ = {};
}

void App::refreshVideoDevices() {
    videoAdapters_ = listVideoAdapters();
    videoDisplays_ = listVideoDisplays();
}

void App::reclaimVideoAdapter() {
    const auto status = renderer_.rendererStatus();
    if (!status.built || !status.adapterMissing) return;
    if (videoAdapterIndex(videoAdapters_, videoOutput_.adapter, videoOutput_.adapterIndex) < 0) return;
    appendDiagnostic("Renderer: the chosen graphics card is connected again");
    renderer_.requestRebuild();
    nextDeviceRecovery_ = {};
}

void App::noticeWarpFallback() {
    // Every pane is then drawn on the CPU and decoded in software, so it is
    // said on screen, not only in the Video tab: at the start, where the
    // window did not open before, and after a rebuild. Up for longer than a
    // choice's notice, as nobody asked for it and it is longer to read.
    const std::wstring fallback = videoRendererWarpFallback(videoOutput_, renderer_.rendererStatus());
    if (!fallback.empty()) showNotice(fallback, 6000);
}

void App::noticeWarpFallbackWhenClear() {
    // Shown at once, it was replaced at the start before the first frame
    // was drawn -- the first tick opens the files the launch gave and starts
    // them ("Playing"), and the settings, a session and a file that could
    // not be added have notices of their own -- and it hid a rebuild's own,
    // a shader file that cannot be read now. It waits for them
    // (expireNotice), and is said once, for the device there is then, when
    // a rebuild comes before the start's was said.
    warpFallbackNoticeDue_ = true;
}

void App::chooseVideoRenderer(VideoRenderer renderer) {
    if (videoOutput_.renderer == renderer) return;
    videoOutput_.renderer = renderer;
    applyVideoOutput();
    showNotice(std::wstring(L"Renderer: ") + videoRendererLabel(renderer));
    scheduleAppSettingsSave();
}

void App::chooseVideoAdapter(int index) {
    std::wstring name;
    int ordinal = 0;
    if (index >= 0 && static_cast<std::size_t>(index) < videoAdapters_.size()) {
        name = videoAdapters_[static_cast<std::size_t>(index)].name;
        ordinal = videoAdapterOrdinal(videoAdapters_, static_cast<std::size_t>(index));
    } else if (index >= 0) {
        return;
    }
    if (videoOutput_.adapter == name && videoOutput_.adapterIndex == ordinal) return;
    videoOutput_.adapter = name;
    videoOutput_.adapterIndex = ordinal;
    applyVideoOutput();
    showNotice(name.empty() ? std::wstring(L"Graphics card: automatic")
                            : L"Graphics card: " + videoAdapterLabel(videoAdapters_, static_cast<std::size_t>(index)));
    scheduleAppSettingsSave();
}

void App::chooseFullscreenDisplay(int index) {
    if (index < 0) {
        videoOutput_.display.clear();
        videoOutput_.displayName.clear();
    } else if (static_cast<std::size_t>(index) < videoDisplays_.size()) {
        const auto& display = videoDisplays_[static_cast<std::size_t>(index)];
        videoOutput_.display = display.device;
        videoOutput_.displayName = videoDisplayLabel(display);
    } else {
        return;
    }
    if (fullscreen_) placeFullscreen();
    showNotice(videoOutput_.display.empty() ? std::wstring(L"Fullscreen on the window's display")
                                            : L"Fullscreen on " + videoOutput_.displayName);
    scheduleAppSettingsSave();
}

void App::setVideoTenBit(bool tenBit) {
    if (videoOutput_.tenBit == tenBit) return;
    videoOutput_.tenBit = tenBit;
    applyVideoOutput();
    showNotice(tenBit ? L"Output: 10-bit" : L"Output: 8-bit");
    scheduleAppSettingsSave();
}

void App::setVideoDeinterlace(VideoDeinterlace mode) {
    if (videoOutput_.deinterlace == mode) return;
    videoOutput_.deinterlace = mode;
    applyVideoOutput();
    showNotice(std::wstring(L"Deinterlacing: ") + videoDeinterlaceLabel(mode));
    scheduleAppSettingsSave();
}

void App::setVideoScaling(VideoScaling scaling) {
    if (videoOutput_.scaling == scaling) return;
    videoOutput_.scaling = scaling;
    applyVideoOutput();
    showNotice(std::wstring(L"Enlarging: ") + videoScalingLabel(scaling));
    scheduleAppSettingsSave();
}

void App::setVideoLimitedRange(bool limited) {
    if (videoOutput_.limitedRange == limited) return;
    videoOutput_.limitedRange = limited;
    applyVideoOutput();
    showNotice(limited ? L"Output levels: TV  (16-235)" : L"Output levels: PC  (0-255)");
    scheduleAppSettingsSave();
}

void App::setVideoColor(VideoColorControl control, int value, bool notice) {
    const int before = videoColorValue(videoOutput_.color, control);
    setVideoColorValue(videoOutput_.color, control, value);
    const int after = videoColorValue(videoOutput_.color, control);
    // At the end of its range a key held on still says where it is.
    if (notice) showNotice(std::wstring(videoColorControlLabel(control)) + L"  " + videoColorValueText(control, after));
    if (after == before) return;
    applyVideoOutput();
    scheduleAppSettingsSave();
}

void App::stepVideoColor(VideoColorControl control, int steps) {
    setVideoColor(control, videoColorValue(videoOutput_.color, control) + steps, true);
}

void App::resetVideoColor() {
    showNotice(L"Colour controls back to 0");
    if (videoOutput_.color == VideoColorControls{}) return;
    videoOutput_.color = {};
    applyVideoOutput();
    scheduleAppSettingsSave();
}

HMONITOR App::fullscreenMonitor() const {
    if (HMONITOR chosen = monitorForDisplay(videoOutput_.display)) return chosen;
    return MonitorFromWindow(window_, MONITOR_DEFAULTTOPRIMARY);
}

// ---------------------------------------------------------------------------
// Menus and keys

// The picture's way to the screen, one line of the picture menu: the
// choices changed while watching. The card, the display, the depth and the
// levels stay in the Video tab.
void App::appendVideoMenu(PanelMenu& menu) const {
    PanelMenu renderers;
    for (int renderer = 0; renderer < kVideoRendererCount; ++renderer) {
        renderers.option(CmdVideoRendererFirst + static_cast<unsigned>(renderer),
                         videoRendererLabel(static_cast<VideoRenderer>(renderer)),
                         videoOutput_.renderer == static_cast<VideoRenderer>(renderer));
    }
    menu.submenu(L"Renderer", std::move(renderers));
    menu.check(CmdVideoDeinterlace, L"Deinterlace", videoOutput_.deinterlace == VideoDeinterlace::Automatic);
    PanelMenu scalers;
    for (int scaling = 0; scaling < kVideoScalingCount; ++scaling) {
        scalers.option(CmdVideoScalingFirst + static_cast<unsigned>(scaling),
                       videoScalingLabel(static_cast<VideoScaling>(scaling)),
                       videoOutput_.scaling == static_cast<VideoScaling>(scaling));
    }
    menu.submenu(L"Enlarging", std::move(scalers));
    menu.option(CmdVideoColorReset, L"Reset the colour controls", false, shortcut(KeyAction::ColorReset),
                videoOutput_.color != VideoColorControls{});
    menu.separator();
    menu.option(CmdVideoSettings, L"Video settings...");
}

bool App::handleVideoCommand(unsigned command) {
    if (command >= CmdVideoRendererFirst && command <= CmdVideoRendererLast) {
        chooseVideoRenderer(static_cast<VideoRenderer>(command - CmdVideoRendererFirst));
    } else if (command == CmdVideoDeinterlace) {
        setVideoDeinterlace(videoOutput_.deinterlace == VideoDeinterlace::Automatic ? VideoDeinterlace::Off
                                                                                     : VideoDeinterlace::Automatic);
    } else if (command >= CmdVideoScalingFirst && command <= CmdVideoScalingLast) {
        setVideoScaling(static_cast<VideoScaling>(command - CmdVideoScalingFirst));
    } else if (command == CmdVideoColorReset) {
        resetVideoColor();
    } else if (command == CmdVideoSettings) {
        openSettingsWindow();
        selectSettingsTab(SettingsTab::Video);
    } else {
        return false;
    }
    return true;
}

bool App::handleVideoKey(KeyAction action) {
    switch (action) {
    case KeyAction::BrightnessDown: stepVideoColor(VideoColorControl::Brightness, -1); return true;
    case KeyAction::BrightnessUp: stepVideoColor(VideoColorControl::Brightness, 1); return true;
    case KeyAction::ContrastDown: stepVideoColor(VideoColorControl::Contrast, -1); return true;
    case KeyAction::ContrastUp: stepVideoColor(VideoColorControl::Contrast, 1); return true;
    case KeyAction::SaturationDown: stepVideoColor(VideoColorControl::Saturation, -1); return true;
    case KeyAction::SaturationUp: stepVideoColor(VideoColorControl::Saturation, 1); return true;
    case KeyAction::HueDown: stepVideoColor(VideoColorControl::Hue, -1); return true;
    case KeyAction::HueUp: stepVideoColor(VideoColorControl::Hue, 1); return true;
    case KeyAction::ColorReset: resetVideoColor(); return true;
    default: return false;
    }
}

// ---------------------------------------------------------------------------
// The Video tab

void App::videoPanelAction(const PanelRow& row, int part) {
    if (part < 0) return;
    // A drop-down's last place may be a choice that is not connected: it is
    // already the choice, so choosing it again does nothing.
    switch (row.id) {
    case SettingId::VideoRenderer:
        if (part < kVideoRendererCount) chooseVideoRenderer(static_cast<VideoRenderer>(part));
        break;
    case SettingId::VideoAdapter:
        if (part == 0) chooseVideoAdapter(-1);
        else if (static_cast<std::size_t>(part) <= videoAdapters_.size()) chooseVideoAdapter(part - 1);
        break;
    case SettingId::VideoDisplay:
        if (part == 0) chooseFullscreenDisplay(-1);
        else if (static_cast<std::size_t>(part) <= videoDisplays_.size()) chooseFullscreenDisplay(part - 1);
        break;
    case SettingId::VideoDepth:
        setVideoTenBit(part == 1);
        break;
    case SettingId::VideoDeinterlace:
        if (part < kVideoDeinterlaceCount) setVideoDeinterlace(static_cast<VideoDeinterlace>(part));
        break;
    case SettingId::VideoScaling:
        if (part < kVideoScalingCount) setVideoScaling(static_cast<VideoScaling>(part));
        break;
    case SettingId::VideoLevels:
        setVideoLimitedRange(part == 1);
        break;
    case SettingId::VideoColorReset:
        resetVideoColor();
        break;
    default:
        break;
    }
}

void App::appendVideoSettingsRows(std::vector<PanelRow>& rows) const {
    const auto header = [&](const wchar_t* text) {
        PanelRow row;
        row.kind = PanelRowKind::Header;
        row.label = text;
        rows.push_back(std::move(row));
    };
    const auto note = [&](const std::wstring& text) {
        PanelRow row;
        row.kind = PanelRowKind::Note;
        row.wrapNote = true;
        row.label = text;
        rows.push_back(std::move(row));
    };
    // A few short options are segments beside the label; a long list, or
    // long names, a drop-down.
    const auto choice = [&](SettingId id, const std::wstring& label, std::vector<std::wstring> options, int selected,
                            bool dropDown = false) {
        PanelRow row;
        row.kind = PanelRowKind::Choice;
        row.id = id;
        row.label = label;
        row.options = std::move(options);
        row.selected = selected;
        row.dropDown = dropDown;
        rows.push_back(std::move(row));
    };

    header(L"Renderer");
    {
        std::vector<std::wstring> renderers;
        for (int renderer = 0; renderer < kVideoRendererCount; ++renderer) {
            renderers.emplace_back(videoRendererLabel(static_cast<VideoRenderer>(renderer)));
        }
        choice(SettingId::VideoRenderer, L"Video renderer", std::move(renderers),
               static_cast<int>(videoOutput_.renderer), true);
        rows.back().hint = L"Direct3D 11 hands frames to Windows with the flip model, which 10-bit and HDR output "
                           L"need. Bitblt is the older way, for capture tools and drivers the flip model troubles. "
                           L"WARP draws on the CPU, with no graphics card at all.";
    }
    {
        // D3D11's own pick first, then the cards as Windows lists them; one
        // chosen that is not connected stays the choice, last.
        std::vector<std::wstring> adapters{L"Automatic  (the main display's card)"};
        int selected = videoOutput_.adapter.empty() ? 0 : -1;
        for (std::size_t index = 0; index < videoAdapters_.size(); ++index) {
            adapters.push_back(videoAdapterLabel(videoAdapters_, index));
        }
        if (const int found = videoAdapterIndex(videoAdapters_, videoOutput_.adapter, videoOutput_.adapterIndex);
            found >= 0) {
            selected = found + 1;
        }
        if (selected < 0) {
            adapters.push_back(videoOutput_.adapter + L"  (not connected)");
            selected = static_cast<int>(adapters.size() - 1);
        }
        choice(SettingId::VideoAdapter, L"Graphics card", std::move(adapters), selected, true);
        rows.back().enabled = videoOutput_.renderer != VideoRenderer::Warp;
        rows.back().hint = L"The card that decodes and draws every video. On a laptop with two, the faster one "
                           L"may need choosing here as well as in Windows' graphics settings.";
    }
    note(L"Changing the renderer or the graphics card reopens every video at the same time.");
    note(videoRendererNote(videoOutput_, renderer_.rendererStatus()));
    {
        std::vector<std::wstring> displays{L"The window's display"};
        int selected = videoOutput_.display.empty() ? 0 : -1;
        for (std::size_t index = 0; index < videoDisplays_.size(); ++index) {
            displays.push_back(videoDisplayLabel(videoDisplays_[index]));
        }
        if (const int found = videoDisplayIndex(videoDisplays_, videoOutput_.display); found >= 0) {
            selected = found + 1;
        }
        if (selected < 0) {
            displays.push_back(videoOutput_.displayName + L"  (not connected)");
            selected = static_cast<int>(displays.size() - 1);
        }
        choice(SettingId::VideoDisplay, withShortcut(L"Fullscreen on", KeyAction::Fullscreen), std::move(displays),
               selected, true);
        rows.back().hint = L"The display fullscreen fills. While it is not connected, fullscreen fills the "
                           L"window's own.";
    }
    choice(SettingId::VideoDepth, L"Output", {L"8-bit", L"10-bit"}, videoOutput_.tenBit ? 1 : 0);
    rows.back().hint = L"10-bit shows smoother gradients on a display that takes it, and needs the flip model. "
                       L"RTX Video HDR's output is 10-bit either way.";

    header(L"Processing");
    {
        std::vector<std::wstring> modes;
        for (int mode = 0; mode < kVideoDeinterlaceCount; ++mode) {
            modes.emplace_back(videoDeinterlaceLabel(static_cast<VideoDeinterlace>(mode)));
        }
        choice(SettingId::VideoDeinterlace, L"Deinterlacing", std::move(modes),
               static_cast<int>(videoOutput_.deinterlace));
        rows.back().hint = L"Videos their decoder marks as interlaced, such as TV recordings and DVDs, are "
                           L"deinterlaced by the graphics card. Off shows their two fields woven together.";
        std::vector<std::wstring> scalers;
        for (int scaling = 0; scaling < kVideoScalingCount; ++scaling) {
            scalers.emplace_back(videoScalingLabel(static_cast<VideoScaling>(scaling)));
        }
        choice(SettingId::VideoScaling, L"Enlarging", std::move(scalers), static_cast<int>(videoOutput_.scaling),
               true);
        rows.back().hint = L"How a video shown larger than it is gets there: bicubic and Lanczos are sharper than "
                           L"bilinear, nearest neighbour keeps pixels square. A video shown smaller is bilinear.";
    }

    header(L"Colour");
    choice(SettingId::VideoLevels, L"Output levels", {L"PC  0\x2013" L"255", L"TV  16\x2013" L"235"},
           videoOutput_.limitedRange ? 1 : 0);
    rows.back().hint = L"TV levels for a television that expects video levels. The wrong one shows black as grey, "
                       L"or dark detail as black.";
    for (int index = 0; index < kVideoColorControlCount; ++index) {
        const auto control = static_cast<VideoColorControl>(index);
        const int range = videoColorControlRange(control);
        const int value = videoColorValue(videoOutput_.color, control);
        // Its keys, as PotPlayer has a pair for each.
        std::wstring keys;
        const auto pair = [&](KeyAction down, KeyAction up) {
            const std::wstring lower = shortcut(down);
            const std::wstring higher = shortcut(up);
            keys = lower.empty() ? higher : higher.empty() ? lower : lower + L" / " + higher;
        };
        if (control == VideoColorControl::Brightness) pair(KeyAction::BrightnessDown, KeyAction::BrightnessUp);
        else if (control == VideoColorControl::Contrast) pair(KeyAction::ContrastDown, KeyAction::ContrastUp);
        else if (control == VideoColorControl::Saturation) pair(KeyAction::SaturationDown, KeyAction::SaturationUp);
        else if (control == VideoColorControl::Hue) pair(KeyAction::HueDown, KeyAction::HueUp);
        PanelRow row;
        row.kind = PanelRowKind::Slider;
        row.id = SettingId::VideoColor;
        row.param = index;
        row.label = keys.empty() ? std::wstring(videoColorControlLabel(control))
                                 : videoColorControlLabel(control) + std::wstring(L"  (") + keys + L")";
        row.slider01 = std::clamp(static_cast<float>(value + range) / static_cast<float>(2 * range), 0.0F, 1.0F);
        row.value = videoColorValueText(control, value);
        rows.push_back(std::move(row));
    }
    {
        PanelRow row;
        row.kind = PanelRowKind::Buttons;
        row.id = SettingId::VideoColorReset;
        row.param = -1;
        const std::wstring key = shortcut(KeyAction::ColorReset);
        row.options = {key.empty() ? std::wstring(L"Reset the colour controls")
                                   : L"Reset the colour controls  " + key};
        row.enabled = videoOutput_.color != VideoColorControls{};
        rows.push_back(std::move(row));
    }
    note(L"A double-click sets a slider back to 0. The levels and the colour controls apply to every video after "
         L"its pixel shader; with RTX Video HDR's output they are left out.");
}

}  // namespace quaddeck
