#include "Core.hpp"
#include "OverlayLayout.hpp"
#include "Session.hpp"
#include "SettingsPanel.hpp"
#include "ShaderCompat.hpp"
#include "SyncState.hpp"

#include <cassert>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <sstream>
#include <thread>

using namespace quaddeck;

void overlayLayoutTests() {
    const auto inside = [](const OverlayRect& rect, float width, float height) {
        if (!rect.visible()) return;
        assert(rect.x >= -0.01F && rect.y >= -0.01F);
        assert(rect.right() <= width + 0.01F);
        assert(rect.bottom() <= height + 0.01F);
    };
    // A wide client shows every control; the rail takes what is left.
    const auto wide = transportBarLayout(1280.0F, 720.0F, 1.0F, true);
    for (std::size_t i = 0; i < kBarItemCount; ++i) {
        // Previous and Next are only there with one video, Layout only with
        // several (both checked below).
        if (i == static_cast<std::size_t>(BarItem::Previous) || i == static_cast<std::size_t>(BarItem::Next) ||
            i == static_cast<std::size_t>(BarItem::Layout)) continue;
        assert(wide.items[i].visible());
        inside(wide.items[i], 1280.0F, 720.0F);
    }
    assert(wide[BarItem::Play].x == 16.0F);
    assert(wide[BarItem::Play].width == 40.0F);
    assert(wide[BarItem::Seek].width > 400.0F);
    assert(wide.row.bottom() == 720.0F - 16.0F);
    // The volume is a popup standing on the speaker, centred on it and
    // reaching down to its top; opening it leaves the rail its length.
    const auto& speaker = wide[BarItem::Audio];
    const auto& popup = wide[BarItem::Volume];
    assert(popup.width == 28.0F && std::abs(popup.x + popup.width * 0.5F - (speaker.x + speaker.width * 0.5F)) < 0.01F);
    assert(std::abs(popup.bottom() - speaker.y) < 0.01F && popup.height == 120.0F && popup.y == speaker.y - 120.0F);
    const auto closed = transportBarLayout(1280.0F, 720.0F, 1.0F, false);
    assert(!closed[BarItem::Volume].visible());
    assert(closed[BarItem::Seek].width == wide[BarItem::Seek].width);
    assert(closed[BarItem::Audio].x == wide[BarItem::Audio].x);
    // Browser panes with independent clocks have no meaningful aggregate
    // time. The empty middle has no hit target, while both button groups
    // retain their positions and more controls fit at narrow widths.
    const auto noTimeline = transportBarLayout(1280.0F, 720.0F, 1.0F, false, false, {}, false);
    assert(!noTimeline[BarItem::Time].visible() && !noTimeline[BarItem::Seek].visible());
    assert(noTimeline[BarItem::Play].visible() && noTimeline[BarItem::Stop].visible() &&
           noTimeline[BarItem::Audio].visible() && noTimeline[BarItem::Settings].visible());
    assert(noTimeline[BarItem::Audio].x == wide[BarItem::Audio].x);
    std::array<bool, kBarItemCount> appearedWithoutTimeline{};
    for (int width = 1; width <= 1920; ++width) {
        const auto bar = transportBarLayout(static_cast<float>(width), 300.0F, 1.0F,
                                            false, false, {}, false);
        float previousRight = -1.0F;
        for (std::size_t i = 0; i < kBarItemCount; ++i) {
            const auto& rect = bar.items[i];
            inside(rect, static_cast<float>(width), 300.0F);
            if (appearedWithoutTimeline[i]) assert(rect.visible());
            if (!rect.visible()) continue;
            appearedWithoutTimeline[i] = true;
            assert(rect.x >= previousRight);
            previousRight = rect.right();
        }
        assert(!bar[BarItem::Time].visible() && !bar[BarItem::Seek].visible());
        assert(bar[BarItem::Play].visible());
    }
    // A short client keeps the popup on screen.
    const auto low = transportBarLayout(1280.0F, 100.0F, 1.0F, true);
    assert(low[BarItem::Volume].y == 0.0F && std::abs(low[BarItem::Volume].bottom() - low[BarItem::Audio].y) < 0.01F);
    // Its rail reads from the bottom up.
    assert(std::abs(verticalRailFraction(popup.bottom() - 10.0F, popup, 10.0F) - 0.0F) < 0.001F);
    assert(std::abs(verticalRailFraction(popup.y + 10.0F, popup, 10.0F) - 1.0F) < 0.001F);
    assert(std::abs(verticalRailFraction(popup.y + popup.height * 0.5F, popup, 10.0F) - 0.5F) < 0.001F);
    assert(verticalRailFraction(-100.0F, popup, 10.0F) == 1.0F && verticalRailFraction(10000.0F, popup, 10.0F) == 0.0F);
    // 2x scale of a 2x client doubles every coordinate.
    const auto doubled = transportBarLayout(2560.0F, 1440.0F, 2.0F, true);
    for (std::size_t i = 0; i < kBarItemCount; ++i) {
        assert(std::abs(doubled.items[i].x - wide.items[i].x * 2.0F) < 0.01F);
        assert(std::abs(doubled.items[i].width - wide.items[i].width * 2.0F) < 0.01F);
        assert(std::abs(doubled.items[i].height - wide.items[i].height * 2.0F) < 0.01F);
    }
    // Every width: inside, ordered, non-overlapping, monotonic appearance,
    // rail always present.
    std::array<bool, kBarItemCount> appeared{};
    for (int width = 1; width <= 1920; ++width) {
        const auto bar = transportBarLayout(static_cast<float>(width), 300.0F, 1.0F, true);
        float previousRight = -1.0F;
        for (std::size_t i = 0; i < kBarItemCount; ++i) {
            const auto& rect = bar.items[i];
            inside(rect, static_cast<float>(width), 300.0F);
            if (appeared[i]) assert(rect.visible());
            if (!rect.visible()) continue;
            appeared[i] = true;
            // The popup stands over the speaker, not in the row's order.
            if (i == static_cast<std::size_t>(BarItem::Volume)) continue;
            assert(rect.x >= previousRight);
            previousRight = rect.right();
        }
        assert(bar[BarItem::Seek].visible());
        assert(bar[BarItem::Volume].visible() == bar[BarItem::Audio].visible());
    }
    const auto narrow = transportBarLayout(260.0F, 300.0F, 1.0F, true);
    assert(narrow[BarItem::Play].visible() && narrow[BarItem::Seek].visible());
    assert(!narrow[BarItem::Settings].visible() && !narrow[BarItem::Menu].visible());
    // The subtitle button sits between the speaker and the menu, and goes
    // after the menu does and before Stop: its menu is also the pane's.
    assert(wide[BarItem::Subtitles].x == wide[BarItem::Audio].right() + 6.0F &&
           wide[BarItem::Menu].x == wide[BarItem::Subtitles].right() + 6.0F);
    assert(!narrow[BarItem::Subtitles].visible());
    bool subtitlesOutlastMenu = false;
    for (int width = 1; width <= 1920; ++width) {
        const auto bar = transportBarLayout(static_cast<float>(width), 300.0F, 1.0F, false);
        assert(!bar[BarItem::Menu].visible() || bar[BarItem::Subtitles].visible());
        assert(!bar[BarItem::Subtitles].visible() || bar[BarItem::Stop].visible());
        subtitlesOutlastMenu = subtitlesOutlastMenu ||
            (bar[BarItem::Subtitles].visible() && !bar[BarItem::Menu].visible());
    }
    assert(subtitlesOutlastMenu);
    const auto tiny = transportBarLayout(60.0F, 300.0F, 1.0F, true);
    assert(!tiny[BarItem::Play].visible() && tiny[BarItem::Seek].visible());
    // A very short client keeps the row on screen.
    const auto flat = transportBarLayout(800.0F, 30.0F, 1.0F, false);
    assert(flat.row.y >= 0.0F && flat.row.bottom() <= 30.0F);
    // One video: Previous and Next flank Play; several: they are not there.
    assert(!wide[BarItem::Previous].visible() && !wide[BarItem::Next].visible());
    const auto single = transportBarLayout(1280.0F, 720.0F, 1.0F, true, true);
    assert(single[BarItem::Previous].x == 16.0F && single[BarItem::Previous].width == 40.0F);
    assert(single[BarItem::Play].x == 62.0F && single[BarItem::Next].x == 108.0F);
    assert(single[BarItem::Stop].x == 154.0F);
    assert(single[BarItem::Seek].width < wide[BarItem::Seek].width);
    std::array<bool, kBarItemCount> appearedSingle{};
    for (int width = 1; width <= 1920; ++width) {
        const auto bar = transportBarLayout(static_cast<float>(width), 300.0F, 1.0F, true, true);
        float previousRight = -1.0F;
        for (std::size_t i = 0; i < kBarItemCount; ++i) {
            const auto& rect = bar.items[i];
            inside(rect, static_cast<float>(width), 300.0F);
            if (appearedSingle[i]) assert(rect.visible());
            if (!rect.visible()) continue;
            appearedSingle[i] = true;
            if (i == static_cast<std::size_t>(BarItem::Volume)) continue;
            assert(rect.x >= previousRight);
            previousRight = rect.right();
        }
        assert(bar[BarItem::Seek].visible());
    }
    // The step buttons go before Play does.
    const auto narrowSingle = transportBarLayout(260.0F, 300.0F, 1.0F, true, true);
    assert(narrowSingle[BarItem::Play].visible() && !narrowSingle[BarItem::Next].visible());
    // Several videos: the arrangement button sits between the subtitles and
    // the menu; with one video, or none, it is not there. It is the first
    // to go after Settings, since the menu and the sheet have the same choice.
    assert(!wide[BarItem::Layout].visible() && !single[BarItem::Layout].visible());
    const auto several = transportBarLayout(1280.0F, 720.0F, 1.0F, true, false, {}, true, true);
    assert(several[BarItem::Layout].visible() && several[BarItem::Layout].width == 40.0F);
    assert(several[BarItem::Layout].x == several[BarItem::Subtitles].right() + 6.0F &&
           several[BarItem::Menu].x == several[BarItem::Layout].right() + 6.0F);
    assert(several[BarItem::Seek].width == wide[BarItem::Seek].width - 46.0F);
    assert(several[BarItem::Settings].right() == wide[BarItem::Settings].right());
    std::array<bool, kBarItemCount> appearedSeveral{};
    for (int width = 1; width <= 1920; ++width) {
        const auto bar = transportBarLayout(static_cast<float>(width), 300.0F, 1.0F, true, false, {}, true, true);
        float previousRight = -1.0F;
        for (std::size_t i = 0; i < kBarItemCount; ++i) {
            const auto& rect = bar.items[i];
            inside(rect, static_cast<float>(width), 300.0F);
            if (appearedSeveral[i]) assert(rect.visible());
            if (!rect.visible()) continue;
            appearedSeveral[i] = true;
            if (i == static_cast<std::size_t>(BarItem::Volume)) continue;
            assert(rect.x >= previousRight);
            previousRight = rect.right();
        }
        assert(bar[BarItem::Seek].visible());
        assert(!bar[BarItem::Layout].visible() || bar[BarItem::Menu].visible());
        assert(!bar[BarItem::Settings].visible() || bar[BarItem::Layout].visible());
    }
    assert(appearedSeveral[static_cast<std::size_t>(BarItem::Layout)]);

    // Pane chrome: pill, chips only when expanded, rail along the bottom.
    const RectF cell{100.0F, 50.0F, 400.0F, 300.0F};
    const auto collapsed = paneChromeLayout(cell, 1.0F, 90.0F, false);
    assert(collapsed.pill.x == 112.0F && collapsed.pill.y == 62.0F);
    assert(collapsed.pill.width == 114.0F && collapsed.pill.height == 28.0F);
    for (const auto& chip : collapsed.chips) assert(!chip.visible());
    assert(collapsed.timeline.visible());
    assert(collapsed.timeline.bottom() == 350.0F && collapsed.timeline.x == 100.0F);
    assert(collapsed.timeLabel.visible() && collapsed.timeLabel.bottom() == collapsed.timeline.y);
    const auto expanded = paneChromeLayout(cell, 1.0F, 90.0F, true);
    for (std::size_t i = 0; i < kPaneChipCount; ++i) {
        assert(expanded.chips[i].visible());
        assert(expanded.chips[i].x >= expanded.pill.right());
        inside(expanded.chips[i], 500.0F, 350.0F);
    }
    assert(expanded.chips[1].x == expanded.chips[0].right() + 6.0F);
    // Chips drop from the right when the pane is narrow; the pill stays.
    const auto narrowPane = paneChromeLayout({0.0F, 0.0F, 200.0F, 300.0F}, 1.0F, 90.0F, true);
    assert(narrowPane.pill.visible());
    assert(narrowPane.chips[0].visible() && !narrowPane.chips[4].visible());
    // The transport bar covering the pane's bottom lifts the rail.
    const auto lifted = paneChromeLayout(cell, 1.0F, 90.0F, false, 60.0F);
    assert(lifted.timeline.bottom() == 290.0F);
    // A pane too short for a rail or a pill shows neither.
    const auto shortPane = paneChromeLayout({0.0F, 0.0F, 300.0F, 60.0F}, 1.0F, 90.0F, true);
    assert(shortPane.pill.visible() && !shortPane.timeline.visible());
    const auto stub = paneChromeLayout({0.0F, 0.0F, 300.0F, 30.0F}, 1.0F, 90.0F, true);
    assert(!stub.pill.visible() && !stub.timeline.visible());
    // Everything scales.
    const auto big = paneChromeLayout({200.0F, 100.0F, 800.0F, 600.0F}, 2.0F, 180.0F, true);
    assert(big.pill.x == 224.0F && big.pill.height == 56.0F && big.pill.width == 228.0F);
    assert(big.chips[0].width == 60.0F && big.timeline.height == 44.0F);

    assert(railFraction(50.0F, {0.0F, 0.0F, 100.0F, 10.0F}, 0.0F) == 0.5F);
    assert(railFraction(-5.0F, {0.0F, 0.0F, 100.0F, 10.0F}, 0.0F) == 0.0F);
    assert(railFraction(110.0F, {0.0F, 0.0F, 100.0F, 10.0F}, 10.0F) == 1.0F);

    // Hit testing: bar over panes, chips over pill over rail over video, and
    // nothing when the element is hidden.
    PaneArray<RectF> cells{};
    PaneArray<bool> active{};
    PaneArray<PaneChromeLayout> chrome{};
    PaneArray<bool> chromeActive{};
    cells[0] = {0.0F, 0.0F, 640.0F, 720.0F};
    cells[1] = {640.0F, 0.0F, 640.0F, 720.0F};
    active[0] = active[1] = true;
    chrome[0] = paneChromeLayout(cells[0], 1.0F, 90.0F, true, 56.0F);
    chrome[1] = paneChromeLayout(cells[1], 1.0F, 90.0F, false, 56.0F);
    chromeActive[0] = true;
    const auto play = wide[BarItem::Play];
    auto hit = overlayHitTest(play.x + 1.0F, play.y + 1.0F, wide, true, chrome, chromeActive, cells, active);
    assert(hit.kind == OverlayHitKind::BarItem && hit.item == static_cast<int>(BarItem::Play));
    hit = overlayHitTest(play.x + 1.0F, play.y + 1.0F, wide, false, chrome, chromeActive, cells, active);
    assert(hit.kind == OverlayHitKind::Video && hit.pane == 0);
    hit = overlayHitTest(chrome[0].chips[2].x + 1.0F, chrome[0].chips[2].y + 1.0F, wide, true,
                         chrome, chromeActive, cells, active);
    assert(hit.kind == OverlayHitKind::PaneChip && hit.pane == 0 && hit.item == 2);
    hit = overlayHitTest(chrome[0].pill.x + 1.0F, chrome[0].pill.y + 1.0F, wide, true,
                         chrome, chromeActive, cells, active);
    assert(hit.kind == OverlayHitKind::PanePill && hit.pane == 0);
    hit = overlayHitTest(300.0F, chrome[0].timeline.y + 1.0F, wide, true,
                         chrome, chromeActive, cells, active);
    assert(hit.kind == OverlayHitKind::PaneTimeline && hit.pane == 0);
    hit = overlayHitTest(900.0F, chrome[1].timeline.y + 1.0F, wide, true,
                         chrome, chromeActive, cells, active);
    assert(hit.kind == OverlayHitKind::Video && hit.pane == 1);
    hit = overlayHitTest(900.0F, 300.0F, wide, true, chrome, chromeActive, cells, active);
    assert(hit.kind == OverlayHitKind::Video && hit.pane == 1);
    active[1] = false;
    hit = overlayHitTest(900.0F, 300.0F, wide, true, chrome, chromeActive, cells, active);
    assert(hit.kind == OverlayHitKind::None);

    // The window caption: Windows 11's 46 x 32 buttons at the right, Close
    // outermost, the title before them, the scrim reaching further down.
    const auto caption = captionLayout(1280.0F, 720.0F, 1.0F);
    assert(caption.row.x == 0.0F && caption.row.y == 0.0F && caption.row.width == 1280.0F &&
           caption.row.height == 32.0F);
    assert(caption.band.height == 72.0F && caption.band.width == 1280.0F);
    assert(caption[CaptionItem::Close].x == 1234.0F && caption[CaptionItem::Close].width == 46.0F);
    assert(caption[CaptionItem::Maximize].right() == caption[CaptionItem::Close].x);
    assert(caption[CaptionItem::Minimize].right() == caption[CaptionItem::Maximize].x);
    assert(caption.title.x == 14.0F && caption.title.right() == caption[CaptionItem::Minimize].x - 14.0F);
    const auto captionDoubled = captionLayout(2560.0F, 1440.0F, 2.0F);
    for (std::size_t i = 0; i < kCaptionItemCount; ++i) {
        assert(captionDoubled.items[i].x == caption.items[i].x * 2.0F &&
               captionDoubled.items[i].width == caption.items[i].width * 2.0F &&
               captionDoubled.items[i].height == 64.0F);
    }
    // Every width: inside the window, in order, never overlapping, and
    // dropped from the left -- Minimize first, Close last.
    for (int width = 1; width <= 1920; ++width) {
        const auto layout = captionLayout(static_cast<float>(width), 300.0F, 1.0F);
        const auto& minimize = layout[CaptionItem::Minimize];
        const auto& maximize = layout[CaptionItem::Maximize];
        const auto& close = layout[CaptionItem::Close];
        for (const auto& item : layout.items) inside(item, static_cast<float>(width), 300.0F);
        inside(layout.title, static_cast<float>(width), 300.0F);
        assert(close.visible() == (width >= 46));
        assert(maximize.visible() == (width >= 92));
        assert(minimize.visible() == (width >= 138));
        if (minimize.visible()) assert(minimize.right() <= maximize.x);
        if (maximize.visible()) assert(maximize.right() <= close.x);
        const float firstButton = minimize.visible() ? minimize.x : maximize.visible() ? maximize.x
                                : close.visible() ? close.x : static_cast<float>(width);
        if (layout.title.visible()) assert(layout.title.right() <= firstButton);
    }
    assert(!captionLayout(0.0F, 300.0F, 1.0F).row.visible());
    assert(!captionLayout(800.0F, 300.0F, 0.0F).row.visible());

    // What the window manager is told: the top band resizes (corners
    // diagonally), the rest of the strip drags, the buttons and everything
    // below are the client's.
    const auto closeBox = caption[CaptionItem::Close];
    assert(windowFrameHitTest(600.0F, 2.0F, 1280.0F, 8.0F, 16.0F, caption, true) == WindowFrameHit::Top);
    assert(windowFrameHitTest(5.0F, 2.0F, 1280.0F, 8.0F, 16.0F, caption, true) == WindowFrameHit::TopLeft);
    assert(windowFrameHitTest(1275.0F, 2.0F, 1280.0F, 8.0F, 16.0F, caption, true) == WindowFrameHit::TopRight);
    assert(windowFrameHitTest(600.0F, 20.0F, 1280.0F, 8.0F, 16.0F, caption, true) == WindowFrameHit::Caption);
    assert(windowFrameHitTest(closeBox.x + 5.0F, 20.0F, 1280.0F, 8.0F, 16.0F, caption, true) ==
           WindowFrameHit::Client);
    assert(windowFrameHitTest(600.0F, 40.0F, 1280.0F, 8.0F, 16.0F, caption, true) == WindowFrameHit::Client);
    // A hidden caption is video, but the top edge still resizes; a
    // maximized window has no band, so its strip reaches the screen's edge.
    assert(windowFrameHitTest(600.0F, 20.0F, 1280.0F, 8.0F, 16.0F, caption, false) == WindowFrameHit::Client);
    assert(windowFrameHitTest(600.0F, 2.0F, 1280.0F, 8.0F, 16.0F, caption, false) == WindowFrameHit::Top);
    assert(windowFrameHitTest(600.0F, 2.0F, 1280.0F, 0.0F, 16.0F, caption, true) == WindowFrameHit::Caption);
    assert(windowFrameHitTest(-1.0F, 2.0F, 1280.0F, 8.0F, 16.0F, caption, true) == WindowFrameHit::Client);
    assert(captionItemAt(closeBox.x + 1.0F, closeBox.y + 1.0F, caption, true) ==
           static_cast<int>(CaptionItem::Close));
    assert(captionItemAt(closeBox.x + 1.0F, closeBox.y + 1.0F, caption, false) == -1);
    assert(captionItemAt(caption.title.x + 1.0F, 10.0F, caption, true) == -1);

    // A pane under the caption keeps its pill below the strip; the rail
    // stays where it was, and a pane with no room under the strip shows
    // no pill.
    const RectF topCell{0.0F, 0.0F, 640.0F, 360.0F};
    const auto covered = paneChromeLayout(topCell, 1.0F, 90.0F, true, 0.0F, false, {}, 32.0F);
    const auto uncovered = paneChromeLayout(topCell, 1.0F, 90.0F, true);
    assert(uncovered.pill.y == 12.0F && covered.pill.y == 44.0F && covered.pill.x == uncovered.pill.x);
    assert(covered.chips[0].y == uncovered.chips[0].y + 32.0F);
    assert(covered.timeline.y == uncovered.timeline.y);
    const auto squeezed = paneChromeLayout({0.0F, 0.0F, 640.0F, 70.0F}, 1.0F, 90.0F, true, 0.0F, false, {}, 32.0F);
    assert(!squeezed.pill.visible());
}

void settingsPanelTests() {
    std::vector<PanelRow> rows;
    PanelRow header; header.kind = PanelRowKind::Header; header.label = L"Audio";
    PanelRow toggle; toggle.kind = PanelRowKind::Toggle; toggle.id = SettingId::Mute; toggle.on = true;
    PanelRow slider; slider.kind = PanelRowKind::Slider; slider.id = SettingId::MasterVolume;
    slider.slider01 = 0.5F; slider.trailingToggle = true;
    PanelRow choice; choice.kind = PanelRowKind::Choice; choice.id = SettingId::Layout;
    choice.options = {L"A", L"B", L"C", L"D", L"E", L"F"}; choice.selected = 1;
    PanelRow buttons; buttons.kind = PanelRowKind::Buttons; buttons.id = SettingId::ZoomAll;
    buttons.options = {L"-", L"1", L"+"};
    PanelRow disabled = toggle; disabled.enabled = false; disabled.id = SettingId::PinBar;
    PanelRow item; item.kind = PanelRowKind::Item; item.id = SettingId::EmbyItem; item.param = 7;
    item.label = L"Camera Roll"; item.value = L"\x203A";
    PanelRow disabledItem = item; disabledItem.enabled = false;
    rows = {header, toggle, slider, choice, buttons, disabled, item, disabledItem};

    const auto layout = settingsPanelLayout(1280.0F, 720.0F, 1.0F, rows, 0.0F);
    assert(layout.sheet.width == 420.0F && layout.sheet.x == 860.0F && layout.sheet.height == 720.0F);
    assert(layout.header.height == 56.0F && layout.close.width == 32.0F);
    assert(layout.close.right() == layout.sheet.right() - 20.0F);
    assert(layout.rows.size() == rows.size());
    // Rows stack from the top of the content, each inside the sheet's padding.
    float y = layout.content.y;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        assert(layout.rows[i].row.y == y);
        assert(layout.rows[i].row.x == layout.sheet.x + 20.0F);
        assert(layout.rows[i].row.right() == layout.sheet.right() - 20.0F);
        y += panelRowHeight(rows[i], 1.0F);
    }
    // The switch sits at the right; the slider leaves room for its trailing toggle.
    assert(layout.rows[1].control.right() == layout.rows[1].row.right());
    assert(layout.rows[2].trailing.visible());
    assert(layout.rows[2].control.right() < layout.rows[2].trailing.x);
    // Six options wrap onto two lines of three equal segments.
    assert(layout.rows[3].parts.size() == 6);
    assert(layout.rows[3].parts[0].width == layout.rows[3].parts[5].width);
    assert(layout.rows[3].parts[3].y > layout.rows[3].parts[2].y);
    assert(layout.rows[3].parts[3].x == layout.rows[3].parts[0].x);
    assert(panelChoiceLines(6, 3) == 2 && panelChoiceLines(3, 3) == 1 && panelChoiceLines(4, 3) == 2);
    assert(layout.rows[4].parts.size() == 3);
    assert(std::abs(layout.rows[4].parts[2].right() - layout.rows[4].row.right()) < 0.01F);
    // Under the window caption the column still reaches the top edge, and
    // the header and the rows start below the strip.
    const auto captioned = settingsPanelLayout(1280.0F, 720.0F, 1.0F, rows, 0.0F, 1.0F, {}, 0.0F, 32.0F);
    assert(captioned.sheet.y == 0.0F && captioned.sheet.height == 720.0F);
    assert(captioned.header.y == 32.0F && captioned.header.height == 56.0F);
    assert(captioned.close.y == 44.0F && captioned.content.y == 88.0F && captioned.content.bottom() == 720.0F);
    assert(captioned.rows[0].row.y == layout.rows[0].row.y + 32.0F);
    assert(settingsPanelHitTest(captioned.close.x + 1.0F, captioned.close.y + 1.0F, captioned, rows).kind ==
           PanelHitKind::Close);
    // 2x scale doubles the sheet.
    const auto doubled = settingsPanelLayout(2560.0F, 1440.0F, 2.0F, rows, 0.0F);
    assert(doubled.sheet.width == 840.0F && doubled.rows[1].control.width == 88.0F);
    assert(doubled.rows[3].parts[0].height == 68.0F);
    // A narrow client shrinks the sheet rather than pushing it off screen.
    const auto narrow = settingsPanelLayout(300.0F, 400.0F, 1.0F, rows, 0.0F);
    assert(narrow.sheet.x == 16.0F && narrow.sheet.right() == 300.0F);
    // Scrolling: content taller than the sheet clamps and moves rows up.
    std::vector<PanelRow> many;
    for (int i = 0; i < 40; ++i) many.push_back(toggle);
    const auto tall = settingsPanelLayout(1280.0F, 400.0F, 1.0F, many, 0.0F);
    assert(tall.maxScroll > 0.0F);
    const auto scrolled = settingsPanelLayout(1280.0F, 400.0F, 1.0F, many, 100.0F);
    assert(scrolled.rows[0].row.y == tall.rows[0].row.y - 100.0F);
    const auto over = settingsPanelLayout(1280.0F, 400.0F, 1.0F, many, 1.0e6F);
    assert(std::abs(over.rows.back().row.bottom() + 24.0F - over.content.bottom()) < 0.01F);
    // The scrollbar: a strip at the right of the content whose thumb is
    // the visible share, at the top unscrolled and at the bottom fully.
    assert(!layout.scrollbar.visible() && !panelScrollThumb(layout, 1.0F).visible());
    assert(tall.scrollbar.visible() && tall.scrollbar.right() == tall.sheet.right() &&
           tall.scrollbar.width == 14.0F && tall.scrollbar.y == tall.content.y);
    const auto thumbTop = panelScrollThumb(tall, 1.0F);
    assert(thumbTop.visible() && thumbTop.y == tall.scrollbar.y && thumbTop.height >= 24.0F &&
           thumbTop.height < tall.scrollbar.height);
    assert(std::abs(thumbTop.height - tall.scrollbar.height * tall.content.height / tall.contentHeight) < 0.01F);
    assert(scrolled.scroll == 100.0F && panelScrollThumb(scrolled, 1.0F).y > thumbTop.y);
    const auto thumbEnd = panelScrollThumb(over, 1.0F);
    assert(std::abs(thumbEnd.bottom() - over.scrollbar.bottom()) < 0.01F && over.scroll == over.maxScroll);
    // Dragging the thumb maps its top back to the scroll, clamped at the ends.
    assert(std::abs(panelScrollForThumbTop(tall, 1.0F, tall.scrollbar.y) - 0.0F) < 0.01F);
    assert(std::abs(panelScrollForThumbTop(tall, 1.0F, tall.scrollbar.bottom()) - tall.maxScroll) < 0.01F);
    assert(std::abs(panelScrollForThumbTop(scrolled, 1.0F, panelScrollThumb(scrolled, 1.0F).y) - 100.0F) < 0.01F);
    auto barHit = settingsPanelHitTest(tall.scrollbar.x + 1.0F, tall.scrollbar.y + 1.0F, tall, many);
    assert(barHit.kind == PanelHitKind::Scrollbar);
    barHit = settingsPanelHitTest(layout.sheet.right() - 1.0F, layout.content.y + 1.0F, layout, rows);
    assert(barHit.kind == PanelHitKind::Sheet);   // nothing to scroll: no bar

    // Hit testing.
    auto hit = settingsPanelHitTest(10.0F, 10.0F, layout, rows);
    assert(hit.kind == PanelHitKind::Outside);
    hit = settingsPanelHitTest(layout.close.x + 1.0F, layout.close.y + 1.0F, layout, rows);
    assert(hit.kind == PanelHitKind::Close);
    hit = settingsPanelHitTest(layout.rows[1].row.x + 1.0F, layout.rows[1].row.y + 1.0F, layout, rows);
    assert(hit.kind == PanelHitKind::Toggle && hit.row == 1);
    // A list item is hit anywhere on its row; a disabled one swallows the click.
    assert(panelRowHeight(rows[6], 1.0F) == 40.0F && panelRowHeight(rows[6], 2.0F) == 80.0F);
    hit = settingsPanelHitTest(layout.rows[6].row.right() - 1.0F, layout.rows[6].row.y + 1.0F, layout, rows);
    assert(hit.kind == PanelHitKind::Item && hit.row == 6);
    hit = settingsPanelHitTest(layout.rows[7].row.x + 1.0F, layout.rows[7].row.y + 1.0F, layout, rows);
    assert(hit.kind == PanelHitKind::Sheet);
    hit = settingsPanelHitTest(layout.rows[2].control.x + layout.rows[2].control.width * 0.5F,
                               layout.rows[2].control.y + 1.0F, layout, rows, 0.0F);
    assert(hit.kind == PanelHitKind::Slider && hit.row == 2 && std::abs(hit.fraction - 0.5F) < 0.01F);
    hit = settingsPanelHitTest(layout.rows[2].trailing.x + 1.0F, layout.rows[2].trailing.y + 1.0F, layout, rows);
    assert(hit.kind == PanelHitKind::Trailing && hit.row == 2);
    hit = settingsPanelHitTest(layout.rows[3].parts[4].x + 1.0F, layout.rows[3].parts[4].y + 1.0F, layout, rows);
    assert(hit.kind == PanelHitKind::Segment && hit.row == 3 && hit.part == 4);
    hit = settingsPanelHitTest(layout.rows[4].parts[1].x + 1.0F, layout.rows[4].parts[1].y + 1.0F, layout, rows);
    assert(hit.kind == PanelHitKind::Button && hit.row == 4 && hit.part == 1);
    hit = settingsPanelHitTest(layout.rows[5].row.x + 1.0F, layout.rows[5].row.y + 1.0F, layout, rows);
    assert(hit.kind == PanelHitKind::Sheet);
    hit = settingsPanelHitTest(layout.rows[0].row.x + 1.0F, layout.rows[0].row.y + 1.0F, layout, rows);
    assert(hit.kind == PanelHitKind::Sheet);

    // Tabs: a strip under the header that the content starts below and
    // scrolls beneath, one rectangle per tab across the inner width, hit by
    // index. The strip's own padding is the sheet.
    assert(!layout.tabStrip.visible() && layout.tabs.empty());
    const auto tabbed = settingsPanelLayout(1280.0F, 720.0F, 1.0F, rows, 0.0F, 1.0F, {}, 0.0F, 0.0F, 6);
    assert(tabbed.tabStrip.visible() && tabbed.tabs.size() == 6);
    assert(tabbed.tabStrip.y == tabbed.header.bottom() && tabbed.tabStrip.height == 46.0F);
    assert(tabbed.content.y == tabbed.tabStrip.bottom() && tabbed.content.height == layout.content.height - 46.0F);
    assert(tabbed.rows[0].row.y == tabbed.content.y);
    assert(tabbed.tabs[0].x == tabbed.sheet.x + 20.0F && tabbed.tabs[0].y == tabbed.tabStrip.y + 8.0F);
    assert(tabbed.tabs[0].height == 30.0F && tabbed.tabs[0].width == 60.0F);
    assert(tabbed.tabs[1].x == tabbed.tabs[0].right() + 4.0F && tabbed.tabs[5].y == tabbed.tabs[0].y);
    assert(std::abs(tabbed.tabs[5].right() - (tabbed.sheet.right() - 20.0F)) < 0.01F);
    hit = settingsPanelHitTest(tabbed.tabs[3].x + 1.0F, tabbed.tabs[3].y + 1.0F, tabbed, rows);
    assert(hit.kind == PanelHitKind::Tab && hit.part == 3 && hit.row == -1);
    hit = settingsPanelHitTest(tabbed.tabStrip.x + 1.0F, tabbed.tabStrip.y + 1.0F, tabbed, rows);
    assert(hit.kind == PanelHitKind::Sheet);
    hit = settingsPanelHitTest(tabbed.rows[1].row.x + 1.0F, tabbed.rows[1].row.y + 1.0F, tabbed, rows);
    assert(hit.kind == PanelHitKind::Toggle && hit.row == 1);
    // A narrow sheet puts the tabs on two lines rather than squeezing them.
    const auto narrowTabs = settingsPanelLayout(300.0F, 400.0F, 1.0F, rows, 0.0F, 1.0F, {}, 0.0F, 0.0F, 6);
    assert(narrowTabs.tabs.size() == 6 && narrowTabs.tabs[3].y == narrowTabs.tabs[0].y &&
           narrowTabs.tabs[4].y == narrowTabs.tabs[0].y + 34.0F && narrowTabs.tabs[4].x == narrowTabs.tabs[0].x);
    assert(narrowTabs.tabStrip.height == 80.0F && narrowTabs.content.y == narrowTabs.tabStrip.bottom());
    // Under the caption the strip follows the header down; 2x doubles it.
    const auto tabbedCaptioned = settingsPanelLayout(1280.0F, 720.0F, 1.0F, rows, 0.0F, 1.0F, {}, 0.0F, 32.0F, 6);
    assert(tabbedCaptioned.tabStrip.y == 88.0F && tabbedCaptioned.content.y == 134.0F);
    const auto tabbedDoubled = settingsPanelLayout(2560.0F, 1440.0F, 2.0F, rows, 0.0F, 1.0F, {}, 0.0F, 0.0F, 6);
    assert(tabbedDoubled.tabStrip.height == 92.0F && tabbedDoubled.tabs[0].width == 120.0F &&
           tabbedDoubled.tabs[5].x == tabbed.tabs[5].x * 2.0F);

    // The header's switch: a button left of the close glyph, only when asked
    // for, hit as Switch; the tabs and rows are where they were.
    assert(!tabbed.sheetSwitch.visible());
    const auto switched = settingsPanelLayout(1280.0F, 720.0F, 1.0F, rows, 0.0F, 1.0F, {}, 0.0F, 0.0F, 6, true);
    assert(switched.sheetSwitch.visible() && switched.sheetSwitch.width == 88.0F &&
           switched.sheetSwitch.height == switched.close.height && switched.sheetSwitch.y == switched.close.y);
    assert(std::abs(switched.sheetSwitch.right() - (switched.close.x - 8.0F)) < 0.01F);
    assert(switched.tabs[0].x == tabbed.tabs[0].x && switched.rows[0].row.y == tabbed.rows[0].row.y);
    hit = settingsPanelHitTest(switched.sheetSwitch.x + 1.0F, switched.sheetSwitch.y + 1.0F, switched, rows);
    assert(hit.kind == PanelHitKind::Switch);
    hit = settingsPanelHitTest(switched.close.x + 1.0F, switched.close.y + 1.0F, switched, rows);
    assert(hit.kind == PanelHitKind::Close);
    // A switch wider than the room beside the title is cut to that room.
    PanelMetrics wideSwitch;
    wideSwitch.sheetSwitchWidth = 10000.0F;
    const auto cutSwitch = settingsPanelLayout(1280.0F, 720.0F, 1.0F, rows, 0.0F, 1.0F, wideSwitch, 0.0F, 0.0F, 0, true);
    assert(std::abs(cutSwitch.sheetSwitch.x - (cutSwitch.sheet.x + 20.0F)) < 0.01F &&
           std::abs(cutSwitch.sheetSwitch.right() - (cutSwitch.close.x - 8.0F)) < 0.01F);

    // --- the browser's grid ---------------------------------------------
    // A wide sheet takes the whole client; tiles fill lines of it.
    assert(panelTilesPerLine(1240.0F, 220.0F, 1.0F) == 5);   // 1252 / 232 = 5.4: five, stretched
    assert(panelTilesPerLine(1240.0F, 150.0F, 1.0F) == 8);   // 1252 / 162 = 7.7: eight, shrunk
    assert(panelTilesPerLine(1240.0F, 220.0F, 2.0F) == 3);   // 1264 / 464 = 2.7: three
    assert(panelTilesPerLine(100.0F, 220.0F, 1.0F) == 1);    // never zero
    // A docked sheet: one thumbnail across, or two posters.
    assert(panelTilesPerLine(280.0F, 220.0F, 1.0F) == 1 && panelTilesPerLine(280.0F, 150.0F, 1.0F) == 2);
    assert(std::abs(panelTileWidthFor(280.0F, 1, 1.0F) - 280.0F) < 0.01F);
    assert(std::abs(panelTileWidthFor(280.0F, 2, 1.0F) - 134.0F) < 0.01F);
    assert(std::abs(panelTileWidthFor(1240.0F, 5, 1.0F) - 238.4F) < 0.01F);
    assert(std::abs(panelTileWidthFor(1240.0F, 5, 2.0F) - 114.4F) < 0.01F);   // (1240 - 4 * 24) / 5 px, in 96-DPI units
    assert(panelTileWidthFor(280.0F, 0, 1.0F) == 0.0F);
    // The sort row's seven segments wrap on a narrow sheet.
    assert(panelSegmentsPerLine(1240.0F, 1.0F, 7) == 7 && panelSegmentsPerLine(280.0F, 1.0F, 7) == 4);
    assert(panelSegmentsPerLine(100.0F, 1.0F, 7) == 3 && panelSegmentsPerLine(280.0F, 1.0F, 3) == 3);
    assert(panelSegmentsPerLine(280.0F, 2.0F, 7) == 3);
    // A playlist's eight fit a wide sheet's line and wrap evenly on a narrow one.
    assert(panelSegmentsPerLine(1240.0F, 1.0F, 8) == 8 && panelSegmentsPerLine(280.0F, 1.0F, 8) == 4);
    assert(panelSegmentsPerLine(1240.0F, 1.0F, 9) == 9 && panelSegmentsPerLine(1240.0F, 1.0F, 12) == 9);
    PanelRow tiles;
    tiles.kind = PanelRowKind::Tiles;
    tiles.id = SettingId::EmbyItem;
    tiles.options = {L"A", L"B", L"C"};
    tiles.tileKeys = {"1|t|440", "", "3|t|440"};
    tiles.tileParams = {4, 5, 6};
    tiles.tileWidth = 220.0F;
    tiles.tileAspect = 16.0F / 9.0F;
    PanelRow posters = tiles;
    posters.tileWidth = 150.0F;
    posters.tileAspect = 2.0F / 3.0F;
    PanelRow sortRow; sortRow.kind = PanelRowKind::Choice; sortRow.id = SettingId::EmbySort;
    sortRow.options = {L"1", L"2", L"3", L"4", L"5", L"6"}; sortRow.segmentsPerLine = 6;
    const std::vector<PanelRow> grid{header, tiles, posters, sortRow};
    const auto wideSheet = settingsPanelLayout(1280.0F, 720.0F, 1.0F, grid, 0.0F, 1.0F, PanelMetrics{}, 1280.0F);
    assert(wideSheet.sheet.x == 0.0F && wideSheet.sheet.width == 1280.0F);
    // Docked beside a video: the right part of the client, never wider than it.
    const auto docked = settingsPanelLayout(1280.0F, 720.0F, 1.0F, grid, 0.0F, 1.0F, PanelMetrics{}, 640.0F);
    assert(docked.sheet.x == 640.0F && docked.sheet.width == 640.0F && docked.rows[1].parts.size() == 3);
    const auto tooWide = settingsPanelLayout(500.0F, 720.0F, 1.0F, grid, 0.0F, 1.0F, PanelMetrics{}, 640.0F);
    assert(tooWide.sheet.x == 0.0F && tooWide.sheet.width == 500.0F);
    assert(std::abs(panelRowHeight(tiles, 1.0F) - (220.0F * 9.0F / 16.0F + 40.0F + 12.0F)) < 0.01F);
    assert(std::abs(panelRowHeight(posters, 1.0F) - (150.0F * 3.0F / 2.0F + 40.0F + 12.0F)) < 0.01F);
    assert(wideSheet.rows[1].parts.size() == 3);
    assert(wideSheet.rows[1].parts[0].x == wideSheet.rows[1].row.x && wideSheet.rows[1].parts[0].width == 220.0F);
    assert(std::abs(wideSheet.rows[1].parts[1].x - (wideSheet.rows[1].row.x + 232.0F)) < 0.01F);
    assert(std::abs(wideSheet.rows[1].parts[0].height - (220.0F * 9.0F / 16.0F + 40.0F)) < 0.01F);
    // Six sort segments on one line when the row asks for six per line.
    assert(wideSheet.rows[3].parts.size() == 6 && wideSheet.rows[3].parts[5].y == wideSheet.rows[3].parts[0].y);
    assert(std::abs(panelRowHeight(sortRow, 1.0F) - (24.0F + 34.0F + 8.0F)) < 0.01F);
    // A hit names the tile, and the tile names its item; the gap is the sheet.
    hit = settingsPanelHitTest(wideSheet.rows[1].parts[2].x + 1.0F, wideSheet.rows[1].parts[2].y + 1.0F, wideSheet, grid);
    assert(hit.kind == PanelHitKind::Tile && hit.row == 1 && hit.part == 2);
    assert(panelRowParam(grid[1], hit.part) == 6);
    assert(panelRowParam(grid[1], 7) == -1 && panelRowParam(grid[0], 3) == grid[0].param);
    hit = settingsPanelHitTest(wideSheet.rows[1].parts[0].right() + 3.0F, wideSheet.rows[1].parts[0].y + 1.0F, wideSheet, grid);
    assert(hit.kind == PanelHitKind::Sheet);
    // The same rows in the normal sheet keep its width.
    const auto normal = settingsPanelLayout(1280.0F, 720.0F, 1.0F, grid, 0.0F, 1.0F, PanelMetrics{}, 0.0F);
    assert(normal.sheet.width == 420.0F && normal.sheet.x == 860.0F);

    // --- the playback order at the end of the only video -----------------
    assert(playOrderNextIndex(PlayOrder::PlayOne, 1, 3, 7) == -1);
    assert(playOrderNextIndex(PlayOrder::RepeatOne, 1, 3, 7) == 1);
    assert(playOrderNextIndex(PlayOrder::InOrder, 1, 3, 7) == 2);
    assert(playOrderNextIndex(PlayOrder::InOrder, 2, 3, 7) == -1);
    assert(playOrderNextIndex(PlayOrder::RepeatList, 2, 3, 7) == 0);
    assert(playOrderNextIndex(PlayOrder::RepeatList, 0, 1, 7) == 0);
    assert(playOrderNextIndex(PlayOrder::Shuffle, 0, 1, 7) == 0);
    for (unsigned random = 0; random < 20; ++random) {
        const int pick = playOrderNextIndex(PlayOrder::Shuffle, 1, 4, random);
        assert(pick >= 0 && pick < 4 && pick != 1);   // another one, never the same
    }
    assert(playOrderNextIndex(PlayOrder::Shuffle, 3, 4, 2) == 2 && playOrderNextIndex(PlayOrder::Shuffle, 0, 4, 0) == 1);
    // Not in the list at all: nothing to go to.
    assert(playOrderNextIndex(PlayOrder::InOrder, -1, 3, 7) == -1);
    assert(playOrderNextIndex(PlayOrder::RepeatList, 5, 3, 7) == -1);
    assert(playOrderNextIndex(PlayOrder::RepeatOne, 0, 0, 7) == -1);
    assert(clampPlayOrder(-4) == PlayOrder::PlayOne && clampPlayOrder(99) == PlayOrder::Shuffle);
}

void fileAssociationTests() {
    const std::wstring exe = L"C:\\Program Files\\QuadDeck\\QuadDeck.exe";
    const AssociationPlan plan = fileAssociationPlan(exe);
    const auto value = [&](const std::wstring& key, const std::wstring& name) -> const RegistryValue* {
        for (const auto& candidate : plan.values) {
            if (candidate.key == key && candidate.name == name) return &candidate;
        }
        return nullptr;
    };
    // The command quotes the path and the file, or a space breaks either.
    assert(openCommand(exe) == L"\"C:\\Program Files\\QuadDeck\\QuadDeck.exe\" \"%1\"");
    const auto* command = value(L"Software\\Classes\\QuadDeck.Video\\shell\\open\\command", L"");
    assert(command && command->data == openCommand(exe));
    const auto* session = value(L"Software\\Classes\\QuadDeck.Session\\shell\\open\\command", L"");
    assert(session && session->data == openCommand(exe));
    const auto* icon = value(L"Software\\Classes\\QuadDeck.Video\\DefaultIcon", L"");
    assert(icon && icon->data == L"\"" + exe + L"\",0");
    // Every type the player opens is offered, and only under HKCU keys of
    // the type's "open with" list -- never the type's own default.
    for (const wchar_t* extension : kVideoExtensions) {
        const std::wstring type = std::wstring(L"Software\\Classes\\") + extension;
        assert(value(type + L"\\OpenWithProgids", L"QuadDeck.Video"));
        assert(!value(type, L""));
        const auto* capability = value(L"Software\\QuadDeck\\Capabilities\\FileAssociations", extension);
        assert(capability && capability->data == L"QuadDeck.Video");
        assert(isMediaExtension(extension));
        assert(!isSubtitleExtension(extension));
    }
    // Subtitle files are offered the same way, to their own ProgID, and
    // are exactly the files the player reads.
    const auto* subtitleCommand = value(L"Software\\Classes\\QuadDeck.Subtitle\\shell\\open\\command", L"");
    assert(subtitleCommand && subtitleCommand->data == openCommand(exe));
    for (const wchar_t* extension : kSubtitleExtensions) {
        const std::wstring type = std::wstring(L"Software\\Classes\\") + extension;
        assert(value(type + L"\\OpenWithProgids", L"QuadDeck.Subtitle"));
        assert(!value(type, L""));
        const auto* capability = value(L"Software\\QuadDeck\\Capabilities\\FileAssociations", extension);
        assert(capability && capability->data == L"QuadDeck.Subtitle");
        assert(value(L"Software\\Classes\\Applications\\QuadDeck.exe\\SupportedTypes", extension));
        assert(isSubtitleExtension(extension) && !isMediaExtension(extension));
    }
    assert(isSubtitleExtension(L".ASS") && isSubtitleExtension(L".Srt") && !isSubtitleExtension(L".sup") &&
           !isSubtitleExtension(L".txt") && !isSubtitleExtension(L""));
    assert(value(L"Software\\Classes\\.qdeck\\OpenWithProgids", L"QuadDeck.Session"));
    const auto* registered = value(L"Software\\RegisteredApplications", L"QuadDeck");
    assert(registered && registered->data == L"Software\\QuadDeck\\Capabilities");
    for (const auto& written : plan.values) {
        assert(written.key.rfind(L"Software\\", 0) == 0);   // relative to HKEY_CURRENT_USER
    }
    // Removing takes our keys whole and, from keys that are not ours, only
    // the values we added.
    assert(plan.ownedKeys.size() == 5);
    for (const auto& key : plan.ownedKeys) {
        assert(key.find(L"QuadDeck") != std::wstring::npos);
        assert(key != L"Software\\Classes" && key != L"Software\\RegisteredApplications");
    }
    assert(plan.sharedValues.size() == kVideoExtensions.size() + kSubtitleExtensions.size() + 2);
    for (const auto& shared : plan.sharedValues) {
        assert(!shared.name.empty());
        assert(shared.name == L"QuadDeck.Video" || shared.name == L"QuadDeck.Subtitle" ||
               shared.name == L"QuadDeck.Session" || shared.name == L"QuadDeck");
    }
    // The status, from a registry made of a plan's values.
    using Registry = std::vector<RegistryValue>;
    const auto status = [&](const Registry& registry) {
        return associationStatusFrom(exe, [&](const std::wstring& key, const std::wstring& name) {
            for (const auto& held : registry) {
                if (_wcsicmp(held.key.c_str(), key.c_str()) == 0 && _wcsicmp(held.name.c_str(), name.c_str()) == 0)
                    return std::optional<std::wstring>(held.data);
            }
            return std::optional<std::wstring>();
        });
    };
    assert(status({}).state == AssociationState::NotRegistered);
    assert(status(plan.values).state == AssociationState::Registered);
    // Case is not a difference; another executable is.
    Registry spelled = plan.values;
    for (auto& held : spelled) {
        for (auto& character : held.data) character = static_cast<wchar_t>(std::towupper(character));
    }
    assert(status(spelled).state == AssociationState::Registered);
    const std::wstring elsewhere = L"D:\\Old\\QuadDeck.exe";
    const auto moved = associationStatusFrom(exe, [&](const std::wstring& key, const std::wstring& name) {
        for (const auto& held : fileAssociationPlan(elsewhere).values) {
            if (held.key == key && held.name == name) return std::optional<std::wstring>(held.data);
        }
        return std::optional<std::wstring>();
    });
    assert(moved.state == AssociationState::RegisteredElsewhere && moved.command == openCommand(elsewhere));
    // A registration made before subtitle files were offered: everything
    // but them. So is one that lost a single value.
    Registry older;
    for (const auto& held : plan.values) {
        if (held.key.find(L"QuadDeck.Subtitle") != std::wstring::npos || held.name == L"QuadDeck.Subtitle" ||
            std::find_if(kSubtitleExtensions.begin(), kSubtitleExtensions.end(),
                         [&](const wchar_t* extension) { return held.name == extension; }) !=
                kSubtitleExtensions.end()) continue;
        older.push_back(held);
    }
    assert(older.size() < plan.values.size());
    assert(status(older).state == AssociationState::Incomplete);
    Registry missingOne;
    for (const auto& held : plan.values) {
        if (held.key != L"Software\\Classes\\QuadDeck.Video\\DefaultIcon") missingOne.push_back(held);
    }
    assert(missingOne.size() + 1 == plan.values.size());
    assert(status(missingOne).state == AssociationState::Incomplete);
    // Without the RegisteredApplications entry Windows lists nothing.
    Registry unlisted;
    for (const auto& held : plan.values) {
        if (held.key != L"Software\\RegisteredApplications") unlisted.push_back(held);
    }
    assert(status(unlisted).state == AssociationState::NotRegistered);
    // The status line.
    AssociationStatus note;
    assert(associationNote(note).find(L"Not registered") != std::wstring::npos);
    note.state = AssociationState::Registered;
    assert(associationNote(note).find(L"Default apps") != std::wstring::npos);
    note.state = AssociationState::RegisteredElsewhere;
    assert(associationNote(note).find(L"another copy") != std::wstring::npos);
    assert(associationNote(note).size() <= 72);
    note.state = AssociationState::Incomplete;
    assert(associationNote(note).find(L"Register") != std::wstring::npos);
    assert(associationNote(note).size() <= 72);
}

void embyLibraryPrefsTests() {
    EmbyLibraryPrefs prefs;
    assert(validateEmbyLibraryPrefs(prefs));
    assert(embyLibraryDetailsEnabled(prefs, "server-a", "001", "movies"));
    assert(embyLibraryDetailsEnabled(prefs, "server-a", "001", "tvshows"));
    for (const char* type : {"", "unknown", "homevideos", "photos", "boxsets", "playlists", "Movie", "Series"}) {
        assert(!embyLibraryDetailsEnabled(prefs, "server-a", "001", type));
    }
    assert(!embyLibraryDetailsEnabled(prefs, "", "001", "movies"));
    assert(!embyLibraryDetailsEnabled(prefs, "server-a", "", "tvshows"));
    assert(setEmbyLibraryDetailsEnabled(prefs, "server-a", "001", false));
    assert(!embyLibraryDetailsEnabled(prefs, "server-a", "001", "movies"));
    assert(embyLibraryDetailsEnabled(prefs, "server-b", "001", "movies"));
    assert(embyLibraryDetailsEnabled(prefs, "server-a", "1", "movies"));
    assert(setEmbyLibraryDetailsEnabled(prefs, "server-b", "001", false));
    assert(setEmbyLibraryDetailsEnabled(prefs, "server-a", "002", false));
    assert(setEmbyLibraryDetailsEnabled(prefs, "server-a", "001", true));
    assert(prefs.entries.size() == 3 && embyLibraryDetailsEnabled(prefs, "server-a", "001", "movies"));
    assert(!embyLibraryDetailsEnabled(prefs, "server-b", "001", "movies"));
    assert(!embyLibraryDetailsEnabled(prefs, "server-a", "002", "tvshows"));
    // An explicit true is still gated by the currently confirmed library type.
    assert(!embyLibraryDetailsEnabled(prefs, "server-a", "001", "homevideos"));
    assert(!embyLibraryDetailsEnabled(prefs, "server-a", "001", ""));
    const auto unicode = wideToUtf8Text(L"\u4F3A\u670D\u5668 \"one\" \\ \U0001F680");
    assert(validEmbyLibraryPreferenceId(unicode));
    assert(setEmbyLibraryDetailsEnabled(prefs, unicode, " quoted \\\" id ", false));
    const std::string maximumId(kMaxEmbyLibraryIdBytes, 'x');
    assert(validEmbyLibraryPreferenceId(maximumId));
    assert(!validEmbyLibraryPreferenceId(maximumId + 'x'));
    const auto multiByte = wideToUtf8Text(L"\u5E93");
    std::string maximumUnicodeId;
    for (int index = 0; index < 170; ++index) maximumUnicodeId += multiByte;
    maximumUnicodeId += "ab";
    assert(maximumUnicodeId.size() == kMaxEmbyLibraryIdBytes && validEmbyLibraryPreferenceId(maximumUnicodeId));
    assert(!validEmbyLibraryPreferenceId(maximumUnicodeId + 'c'));
    for (const auto& invalid : {std::string(), std::string("a\nb"), std::string("a\rb"), std::string("a\tb"),
                               std::string("a\0b", 3), std::string("a\x7f"), std::string("\xc2\x85"),
                               std::string("\xc0\x80"), std::string("\xed\xa0\x80"), std::string("\xf4\x90\x80\x80"),
                               std::string("\xe2\x82"), wideToUtf8Text(L"a\u2028b"), wideToUtf8Text(L"a\u2029b")}) {
        assert(!validEmbyLibraryPreferenceId(invalid));
        const auto before = prefs;
        assert(!setEmbyLibraryDetailsEnabled(prefs, invalid, "valid", true));
        assert(!setEmbyLibraryDetailsEnabled(prefs, "valid", invalid, true));
        assert(prefs == before);
    }
    EmbyLibraryPrefs full;
    for (std::size_t index = 0; index < kMaxEmbyLibraryPreferences; ++index) {
        assert(setEmbyLibraryDetailsEnabled(full, "server", std::to_string(index), false));
    }
    assert(validateEmbyLibraryPrefs(full));
    const auto beforeFull = full;
    assert(!setEmbyLibraryDetailsEnabled(full, "server", "another", true) && full == beforeFull);
    assert(setEmbyLibraryDetailsEnabled(full, "server", "0", true));
    assert(full.entries.size() == kMaxEmbyLibraryPreferences && embyLibraryDetailsEnabled(full, "server", "0", "movies"));
    auto duplicate = prefs;
    duplicate.entries.push_back(duplicate.entries.front());
    assert(!validateEmbyLibraryPrefs(duplicate));
    assert(!setEmbyLibraryDetailsEnabled(duplicate, "other", "library", true));
    assert(!embyLibraryDetailsEnabled(duplicate, "server-a", "001", "movies"));
    full.entries.push_back({"server", "overflow", true});
    assert(!validateEmbyLibraryPrefs(full));
    assert(!embyLibraryDetailsEnabled(full, "server", "0", "movies"));
}

void embyLibrarySettingsFormatTests() {
    AppSettings state;
    const auto unicodeServer = wideToUtf8Text(L"\u4F3A\u670D\u5668 \"A\" \\ \U0001F680");
    const auto unicodeLibrary = wideToUtf8Text(L"001 \u7535\u5F71 \\ \"library\"");
    assert(setEmbyLibraryDetailsEnabled(state.embyLibraries, "server-b", "001", true));
    assert(setEmbyLibraryDetailsEnabled(state.embyLibraries, "server-a", "001", false));
    assert(setEmbyLibraryDetailsEnabled(state.embyLibraries, unicodeServer, unicodeLibrary, false));
    state.style.customShaderPath = L"C:\\Shaders\\saved.hlsl";
    std::wstringstream serialized;
    assert(writeAppSettings(serialized, state));
    assert(serialized.str().starts_with(L"QCONFIG 14\n"));
    AppSettings restored;
    assert(readAppSettings(serialized, restored));
    assert(restored.embyLibraries.entries.size() == 3 && restored.style.customShaderPath == state.style.customShaderPath);
    assert(!embyLibraryDetailsEnabled(restored.embyLibraries, "server-a", "001", "movies"));
    assert(embyLibraryDetailsEnabled(restored.embyLibraries, "server-b", "001", "movies"));
    assert(!embyLibraryDetailsEnabled(restored.embyLibraries, unicodeServer, unicodeLibrary, "tvshows"));
    std::wstringstream writtenAgain;
    assert(writeAppSettings(writtenAgain, restored) && writtenAgain.str() == serialized.str());
    assert(restored.embyLibraries.entries[0].serverId == "server-a");
    assert(restored.embyLibraries.entries[1].serverId == "server-b");
    std::wstringstream emptyText;
    assert(writeAppSettings(emptyText, AppSettings{}));
    const std::wstring emptyBlock = L"embylibrarydetails 0\n";
    const auto blockAt = emptyText.str().find(emptyBlock);
    assert(blockAt != std::wstring::npos);
    for (const int version : {0, 15}) {
        auto unsupported = emptyText.str();
        unsupported.replace(0, std::wstring(L"QCONFIG 14").size(), L"QCONFIG " + std::to_wstring(version));
        std::wistringstream input(unsupported);
        AppSettings loaded;
        assert(!readAppSettings(input, loaded));
    }
    const auto withBlock = [&](const std::wstring& block) {
        auto text = emptyText.str();
        text.replace(blockAt, emptyBlock.size(), block);
        return text;
    };
    const auto rejected = [&](const std::wstring& block) {
        std::wistringstream input(withBlock(block));
        AppSettings loaded;
        assert(!readAppSettings(input, loaded));
    };
    for (const auto& block : {
        std::wstring(), std::wstring(L"embylibrarydetails -1\n"), std::wstring(L"embylibrarydetails 257\n"),
        std::wstring(L"embylibrarydetails 18446744073709551615\n"), std::wstring(L"embylibrarydetails 1.5\n"),
        std::wstring(L"embylibrarydetails 1\n"), std::wstring(L"embylibrarydetails 1\nwrong \"s\" \"l\" 1\n"),
        std::wstring(L"embylibrarydetails 1\nembylibrarydetail \"\" \"l\" 1\n"),
        std::wstring(L"embylibrarydetails 1\nembylibrarydetail \"s\" \"\" 1\n"),
        std::wstring(L"embylibrarydetails 1\nembylibrarydetail s \"l\" 1\n"),
        std::wstring(L"embylibrarydetails 1\nembylibrarydetail \"s\" l 1\n"),
        std::wstring(L"embylibrarydetails 1\nembylibrarydetail \"s\" \"l\" 2\n"),
        std::wstring(L"embylibrarydetails 1\nembylibrarydetail \"s\" \"l\" -1\n"),
        std::wstring(L"embylibrarydetails 1\nembylibrarydetail \"s\" \"l\" true\n"),
        std::wstring(L"embylibrarydetails 1\nembylibrarydetail \"s\" \"l\"\n"),
        std::wstring(L"embylibrarydetails 2\nembylibrarydetail \"s\" \"l\" 1\n"),
        std::wstring(L"embylibrarydetails 2\nembylibrarydetail \"s\" \"l\" 1\nembylibrarydetail \"s\" \"l\" 0\n"),
        std::wstring(L"embylibrarydetails 0\nembylibrarydetail \"s\" \"l\" 1\n"),
        std::wstring(L"embylibrarydetails 1\nembylibrarydetail \"s\nline\" \"l\" 1\n"),
        std::wstring(L"embylibrarydetails 1\nembylibrarydetail \"s\" \"l\u0085\" 1\n")}) rejected(block);
    std::wstring malformedWide = L"embylibrarydetails 1\nembylibrarydetail \"s";
    malformedWide.push_back(static_cast<wchar_t>(0xd800));
    malformedWide += L"\" \"l\" 1\n";
    rejected(malformedWide);
    const std::wstring maximumId(kMaxEmbyLibraryIdBytes, L'x');
    const auto maximumBlock = L"embylibrarydetails 1\nembylibrarydetail \"s\" \"" + maximumId + L"\" 0\n";
    std::wistringstream maximumInput(withBlock(maximumBlock));
    AppSettings maximumLoaded;
    assert(readAppSettings(maximumInput, maximumLoaded));
    assert(maximumLoaded.embyLibraries.entries[0].libraryId.size() == kMaxEmbyLibraryIdBytes);
    rejected(L"embylibrarydetails 1\nembylibrarydetail \"s\" \"" + maximumId + L"x\" 0\n");
    AppSettings maximumCount;
    for (std::size_t index = 0; index < kMaxEmbyLibraryPreferences; ++index) {
        maximumCount.embyLibraries.entries.push_back({"server", std::to_string(index), index % 2 == 0});
    }
    std::wstringstream maximumCountText;
    assert(writeAppSettings(maximumCountText, maximumCount));
    AppSettings maximumCountLoaded;
    assert(readAppSettings(maximumCountText, maximumCountLoaded));
    assert(maximumCountLoaded.embyLibraries.entries.size() == kMaxEmbyLibraryPreferences);
    // Exercise count and duplicate validation before any document is emitted.
    auto tooMany = maximumCount;
    tooMany.embyLibraries.entries.push_back({"server", "overflow", true});
    std::wstringstream tooManyOutput;
    assert(!writeAppSettings(tooManyOutput, tooMany) && tooManyOutput.str().empty());
    auto duplicate = state;
    duplicate.embyLibraries.entries.push_back(duplicate.embyLibraries.entries.front());
    std::wstringstream duplicateOutput;
    assert(!writeAppSettings(duplicateOutput, duplicate) && duplicateOutput.str().empty());
    AppSettings invalidId = state;
    invalidId.embyLibraries.entries[0].serverId = std::string("bad\0id", 6);
    std::wstringstream invalidOutput;
    assert(!writeAppSettings(invalidOutput, invalidId) && invalidOutput.str().empty());
    invalidId.embyLibraries.entries[0].serverId.clear();
    assert(!writeAppSettings(invalidOutput, invalidId));
    invalidId.embyLibraries.entries[0].serverId = "\xc0\x80";
    assert(!writeAppSettings(invalidOutput, invalidId));
    invalidId.embyLibraries.entries[0].serverId.assign(kMaxEmbyLibraryIdBytes + 1, 'x');
    assert(!writeAppSettings(invalidOutput, invalidId));
    // Every prior version remains readable and clears newer overrides when
    // the same destination object is reused. Style/session formats are separate.
    for (int version = 1; version <= 12; ++version) {
        const std::size_t panes = version >= 5 ? kMaxPanes : kLegacyPaneCount;
        std::wstringstream legacy;
        legacy << L"QCONFIG " << version << L"\nplayback 0 0 1 1 0.5 0\nwindow 0 0 1440 900 0\n";
        if (version >= 2) {
            legacy << L"repeatpane";
            for (std::size_t index = 0; index < panes; ++index) legacy << L" 0";
            legacy << L'\n';
        }
        if (version >= 4) {
            legacy << L"panevolume";
            for (std::size_t index = 0; index < panes; ++index) legacy << L" 1 0";
            legacy << L'\n';
        }
        if (version >= 6) legacy << L"nascache 1\n";
        if (version >= 7) legacy << L"rtxvideo 0 0\n";
        if (version >= 8) legacy << L"embybrowser 2 0 0 0 0\n";
        if (version >= 9) legacy << L"playorder 2\n";
        if (version >= 10) legacy << L"keyframeseek 1\n";
        if (version >= 11) legacy << L"subtitles 1 0 1 0" << (version >= 12 ? L" 0\n" : L"\n");
        legacy << L"QSTYLE " << (version >= 5 ? 3 : 1) << L"\nsettings 0 -1 0 0\nshaderpath \"\"\n";
        if (version >= 5) legacy << L"smartvibrance 1.5 0.5 0.003 45\n";
        for (std::size_t index = 0; index < panes; ++index) legacy << L"pane " << index << L" 0 1\n";
        AppSettings loaded = state;
        assert(readAppSettings(legacy, loaded) && loaded.embyLibraries.entries.empty());
        assert(embyLibraryDetailsEnabled(loaded.embyLibraries, "server", "library", "movies"));
        assert(!embyLibraryDetailsEnabled(loaded.embyLibraries, "server", "library", "homevideos"));
    }
    std::wstringstream style;
    assert(writeStyle(style, state.style) && style.str().find(L"embylibrary") == std::wstring::npos);
    std::wstringstream session;
    assert(writeSession(session, SessionState{}) && session.str().find(L"embylibrary") == std::wstring::npos);
}

void deckPolicyTests() {
    using O = PaneOrigin;
    const PaneOrigins empty{};
    assert(deckTimeline(empty) == DeckTimeline::Shared && loadedPaneCount(empty) == 0);
    assert(effectiveSeekMode(empty, SeekMode::Independent) == SeekMode::Independent);
    assert(effectiveSeekMode(empty, SeekMode::Linked) == SeekMode::Linked);

    // One manual video is the timeline whatever was saved; several use it.
    const PaneOrigins one{O::Manual, O::Empty, O::Empty, O::Empty, O::Empty};
    assert(effectiveSeekMode(one, SeekMode::Independent) == SeekMode::Linked);
    const PaneOrigins manualPair{O::Manual, O::Empty, O::Manual, O::Empty, O::Empty};
    assert(deckTimeline(manualPair) == DeckTimeline::Shared);
    assert(effectiveSeekMode(manualPair, SeekMode::Independent) == SeekMode::Independent);
    assert(effectiveSeekMode(manualPair, SeekMode::Linked) == SeekMode::Linked);
    assert(paneRepeatFlagInForce(manualPair, 0, SeekMode::Independent));

    // Any browser pane gives every pane its own timeline; a photo does not.
    for (const auto browser : {O::LocalBrowser, O::EmbyVideo}) {
        const PaneOrigins deck{O::Manual, browser, O::Empty, O::Empty, O::Empty};
        assert(deckTimeline(deck) == DeckTimeline::PerPane);
        assert(effectiveSeekMode(deck, SeekMode::Linked) == SeekMode::Independent);
        const PaneOrigins alone{browser, O::Empty, O::Empty, O::Empty, O::Empty};
        assert(effectiveSeekMode(alone, SeekMode::Linked) == SeekMode::Independent);
    }
    const PaneOrigins photo{O::EmbyPhoto, O::Manual, O::Empty, O::Empty, O::Empty};
    assert(deckTimeline(photo) == DeckTimeline::Shared && loadedPaneCount(photo) == 2);

    // A lone manual pane beside browser panes keeps its Linked-era rule, and
    // so do several of them under a saved Linked mode.
    const PaneOrigins mixed{O::Manual, O::LocalBrowser, O::Empty, O::Empty, O::Empty};
    assert(!paneRepeatFlagInForce(mixed, 0, SeekMode::Independent));
    assert(paneRepeatFlagInForce(mixed, 1, SeekMode::Independent));
    const PaneOrigins mixedPair{O::Manual, O::LocalBrowser, O::Manual, O::Empty, O::Empty};
    assert(paneRepeatFlagInForce(mixedPair, 0, SeekMode::Independent));
    assert(!paneRepeatFlagInForce(mixedPair, 0, SeekMode::Linked));
    assert(!paneRepeatFlagInForce(mixedPair, 9, SeekMode::Independent));

    // The only browser video follows "When the only video ends", like the
    // only manual one; its flag counts once another pane joins.
    for (const auto browser : {O::LocalBrowser, O::EmbyVideo}) {
        const PaneOrigins alone{O::Empty, browser, O::Empty, O::Empty, O::Empty};
        assert(!paneRepeatFlagInForce(alone, 1, SeekMode::Independent));
        const PaneOrigins joined{O::Empty, browser, O::Empty, O::LocalBrowser, O::Empty};
        assert(paneRepeatFlagInForce(joined, 1, SeekMode::Independent));
    }

    // The bar shows the only browser video's own time: the master clock
    // under it kept counting through what that pane played before.
    assert(barTimelinePane(empty) == -1);
    assert(barTimelinePane(one) == -1 && barTimelinePane(manualPair) == -1);
    for (const auto own : {O::LocalBrowser, O::EmbyVideo, O::EmbyPhoto}) {
        const PaneOrigins alone{O::Empty, O::Empty, O::Empty, own, O::Empty};
        assert(barTimelinePane(alone) == 3);
        const PaneOrigins joined{O::Manual, O::Empty, O::Empty, own, O::Empty};
        assert(barTimelinePane(joined) == -1);
    }
    const PaneOrigins browserPair{O::EmbyVideo, O::LocalBrowser, O::Empty, O::Empty, O::Empty};
    assert(barTimelinePane(browserPair) == -1);
}

void seekBarrierDetachTests() {
    PaneTimings panes{};
    for (const auto index : {0u, 4u}) {
        panes[index].loaded = panes[index].ready = true;
        panes[index].duration = 120.0;
    }
    panes[4].adjustment = 10.0;
    SeekBarrier barrier;
    assert(barrier.begin(25.0, true, panes, 0b10001, SeekMode::Linked));
    barrier.setGeneration(0, 41);
    barrier.setGeneration(4, 45);
    barrier.setAudioGeneration(0, 51);
    barrier.setAudioGeneration(4, 55);
    assert(barrier.observeFrame(4, true, 45, false).firstFrame);
    PaneArray<SeekBarrier::PaneStatus> status{};
    status[0].loaded = status[4].loaded = true;
    status[4].audioGeneration = 55;
    status[4].audioState = SeekBarrier::AudioState::Primed;
    assert(!barrier.ready(0, status));

    barrier.detachPane(0);
    barrier.detachPane(kMaxPanes); // out of range is harmless
    assert(barrier.active() && barrier.resume() && !barrier.timedOut() && barrier.target() == 25.0);
    assert(!barrier.waiting(0) && !barrier.audioWaiting(0) && barrier.paneTarget(0) == 0.0);
    assert(barrier.waiting(4) && barrier.audioWaiting(4) && barrier.paneTarget(4) == 35.0);
    const auto detachedFrame = barrier.observeFrame(0, true, 41, true);
    assert(!detachedFrame.firstFrame && !detachedFrame.exactFrame);
    // V5 still owns its original frame/audio generations and logging state.
    const auto staleFrame = barrier.observeFrame(4, true, 44, true);
    assert(!staleFrame.firstFrame && !staleFrame.exactFrame && !barrier.ready(0, status));
    const auto exactFrame = barrier.observeFrame(4, true, 45, true);
    assert(!exactFrame.firstFrame && exactFrame.exactFrame);
    status[4].audioGeneration = 54;
    assert(!barrier.ready(0, status));
    status[4].audioGeneration = 55;
    status[4].audioState = SeekBarrier::AudioState::Unknown;
    assert(!barrier.ready(0, status));
    status[4].audioState = SeekBarrier::AudioState::Primed;
    assert(barrier.ready(0, status));
    assert(!barrier.observeFrame(4, true, 45, true).exactFrame);

    // Removing the last participant lets the caller complete this barrier;
    // it does not reset its global playback intent or timeout information.
    barrier.noteElapsed(SeekBarrier::kTimeoutMs);
    barrier.detachPane(4);
    assert(barrier.ready(0, status) && barrier.active() && barrier.resume() && barrier.timedOut());
    assert(barrier.target() == 25.0 && !barrier.waiting(4) && !barrier.audioWaiting(4));
    SeekBarrier inactive;
    inactive.detachPane(0);
    assert(!inactive.active() && !inactive.resume() && inactive.target() == 0.0);
}

void wrappedPanelNoteTests() {
    PanelRow note;
    note.label = L"Replace one pane with a long movie title that must stay complete in a narrow sheet "
                 L"(resume saved progress, muted).\n" + std::wstring(90, L'W');
    for (const float scale : {1.0F, 2.0F}) {
        for (const float width : {300.0F, 320.0F, 340.0F}) {
            const float innerWidth = (width - 40.0F) * scale;
            assert(panelRowHeight(note, scale, {}, innerWidth) == 30.0F * scale);
            auto wrapped = note;
            wrapped.wrapNote = true;
            const float fallbackHeight = panelRowHeight(wrapped, scale, {}, innerWidth);
            assert(fallbackHeight > 30.0F * scale);
            assert(std::abs(fallbackHeight / scale - panelRowHeight(wrapped, 1.0F, {}, width - 40.0F)) < 0.01F);

            wrapped.measuredNoteHeight = 80.0F * scale;
            wrapped.measuredNoteWidth = innerWidth;
            wrapped.measuredNoteScale = scale;
            const std::vector<PanelRow> rows{wrapped};
            const auto layout = settingsPanelLayout(width * scale, 1000.0F * scale, scale, rows,
                                                   0.0F, 1.0F, {}, width * scale);
            const auto& row = layout.rows[0].row;
            const auto text = panelWrappedNoteTextBox(row, scale);
            assert(row.height == 88.0F * scale && text.height == 80.0F * scale);
            assert(text.y == row.y + 2.0F * scale && text.bottom() == row.bottom() - 6.0F * scale);
            assert(layout.contentHeight == row.height + 24.0F * scale);
            assert(settingsPanelHitTest(row.x + 1.0F, row.bottom() - 1.0F, layout, rows).kind == PanelHitKind::Sheet);

            // A resize or DPI change must invalidate the old measurement,
            // even if its old font/width would have yielded a shorter row.
            auto unmeasured = wrapped;
            unmeasured.measuredNoteHeight = 0.0F;
            assert(panelRowHeight(wrapped, scale, {}, innerWidth + 0.25F) ==
                   panelRowHeight(unmeasured, scale, {}, innerWidth + 0.25F));
            assert(panelRowHeight(wrapped, scale + 0.25F, {}, innerWidth) ==
                   panelRowHeight(unmeasured, scale + 0.25F, {}, innerWidth));
              wrapped.measuredNoteHeight = -1.0F;
              assert(panelRowHeight(wrapped, scale, {}, innerWidth) == fallbackHeight);
              for (const float invalid : {std::numeric_limits<float>::quiet_NaN(),
                                          std::numeric_limits<float>::infinity()}) {
                  wrapped.measuredNoteHeight = invalid;
                  assert(panelRowHeight(wrapped, scale, {}, innerWidth) == fallbackHeight);
              }

              wrapped.measuredNoteHeight = 80.0F * scale;
              PanelRow cancel;
              cancel.kind = PanelRowKind::Buttons;
              cancel.id = SettingId::LocalReplaceCancel;
              cancel.options = {L"Cancel"};
              const std::vector<PanelRow> replacement{wrapped, cancel};
              const auto actions = settingsPanelLayout(width * scale, 1000.0F * scale, scale, replacement,
                                                      0.0F, 1.0F, {}, width * scale);
              assert(actions.rows[1].row.y >= actions.rows[0].row.bottom());
              const auto& button = actions.rows[1].parts[0];
              const auto hit = settingsPanelHitTest(button.x + button.width * 0.5F,
                                                   button.y + button.height * 0.5F, actions, replacement);
              assert(hit.kind == PanelHitKind::Button && hit.row == 1 && hit.part == 0);
        }
    }
}

void panelAddActionTests() {
    PanelRow detail;
    detail.kind = PanelRowKind::MediaDetail;
    detail.mediaDetail.title = L"Movie";
    detail.mediaDetail.overview = L"Overview";
    detail.options = {L"Play", L"From beginning", L"More", L"Add", L"Add from beginning"};
    const std::vector<PanelRow> rows{detail};
    const auto wide = settingsPanelLayout(1280.0F, 1400.0F, 1.0F, rows, 0.0F, 1.0F, PanelMetrics{}, 1280.0F);
    const auto& geometry = wide.rows[0];
    assert(geometry.parts.size() == 5 && !geometry.mediaDetail.stacked);
    for (std::size_t i = 0; i < geometry.parts.size(); ++i) {
        const auto& part = geometry.parts[i];
        assert(part.visible() && part.x >= geometry.row.x && part.right() <= geometry.row.right() + 0.01F);
        const auto hit = settingsPanelHitTest(part.x + 1.0F, part.y + 1.0F, wide, rows);
        assert(hit.kind == PanelHitKind::Button && hit.row == 0 && hit.part == static_cast<int>(i));
    }
    assert(geometry.parts[0].y == geometry.parts[1].y && geometry.parts[3].y == geometry.parts[4].y);
    assert(geometry.parts[2].y > geometry.parts[4].bottom());

    auto hidden = rows;
    hidden[0].options = {L"Play", L"", L"More", L"", L""};
    const auto hiddenLayout = settingsPanelLayout(1280.0F, 1400.0F, 1.0F, hidden, 0.0F, 1.0F, PanelMetrics{}, 1280.0F);
    assert(hiddenLayout.rows[0].parts.size() == 5);
    for (const auto slot : {1u, 3u, 4u}) {
        assert(!hiddenLayout.rows[0].parts[slot].visible());
        const auto& oldPart = geometry.parts[slot];
        const auto hit = settingsPanelHitTest(oldPart.x + 1.0F, oldPart.y + 1.0F, hiddenLayout, hidden);
        // Visible buttons may grow into freed space, keeping their own slot.
        assert(hit.kind != PanelHitKind::Button || (hit.part >= 0 && !hidden[0].options[hit.part].empty()));
    }
    const auto narrow = settingsPanelLayout(300.0F, 1400.0F, 1.0F, rows, 0.0F, 1.0F, PanelMetrics{}, 300.0F);
    const auto& narrowGeometry = narrow.rows[0];
    assert(narrowGeometry.mediaDetail.stacked && narrowGeometry.parts.size() == 5);
    assert(narrowGeometry.parts[0].y < narrowGeometry.parts[1].y &&
           narrowGeometry.parts[1].y < narrowGeometry.parts[3].y &&
           narrowGeometry.parts[3].y < narrowGeometry.parts[4].y);
    for (const auto& part : narrowGeometry.parts) {
        assert(part.visible() && part.x >= narrowGeometry.row.x && part.right() <= narrowGeometry.row.right() + 0.01F);
    }

    PanelRow item;
    item.kind = PanelRowKind::Item;
    item.addAvailable = true;
    const std::vector<PanelRow> itemRows{item};
    const auto itemLayout = settingsPanelLayout(600.0F, 400.0F, 1.0F, itemRows, 0.0F);
    const auto& add = itemLayout.rows[0].addParts[0];
    auto hit = settingsPanelHitTest(add.x + 1.0F, add.y + 1.0F, itemLayout, itemRows);
    assert(hit.kind == PanelHitKind::Add && hit.row == 0 && hit.part == 0);
    hit = settingsPanelHitTest(itemLayout.rows[0].row.x + 1.0F, add.y + 1.0F, itemLayout, itemRows);
    assert(hit.kind == PanelHitKind::Item);

    PanelRow tiles;
    tiles.kind = PanelRowKind::Tiles;
    tiles.options = {L"One", L"Two"};
    tiles.tileParams = {7, 8};
    tiles.tileAddAvailable = {true, false};
    const std::vector<PanelRow> tileRows{tiles};
    const auto tileLayout = settingsPanelLayout(800.0F, 600.0F, 1.0F, tileRows, 0.0F, 1.0F, PanelMetrics{}, 800.0F);
    assert(tileLayout.rows[0].addParts.size() == 2 && tileLayout.rows[0].addParts[0].visible() &&
           !tileLayout.rows[0].addParts[1].visible());
    const auto& tileAdd = tileLayout.rows[0].addParts[0];
    hit = settingsPanelHitTest(tileAdd.x + 1.0F, tileAdd.y + 1.0F, tileLayout, tileRows);
    assert(hit.kind == PanelHitKind::Add && hit.part == 0 && panelRowParam(tileRows[0], hit.part) == 7);
    const auto& second = tileLayout.rows[0].parts[1];
    hit = settingsPanelHitTest(second.right() - 10.0F, second.y + 10.0F, tileLayout, tileRows);
    assert(hit.kind == PanelHitKind::Tile && hit.part == 1);

    PanelRow buttons;
    buttons.kind = PanelRowKind::Buttons;
    buttons.options = {L"One", L"", L"Two", L"Three", L"Four", L"Five"};
    buttons.buttonMinWidth = 120.0F;
    const std::vector<PanelRow> buttonRows{buttons};
    const auto wrapped = settingsPanelLayout(320.0F, 600.0F, 1.0F, buttonRows, 0.0F, 1.0F, PanelMetrics{}, 320.0F);
    const auto& parts = wrapped.rows[0].parts;
    assert(parts.size() == buttons.options.size() && !parts[1].visible());
    assert(parts[0].y == parts[2].y && parts[3].y == parts[4].y && parts[5].y > parts[4].y);
    assert(parts[3].y > parts[2].y && parts[3].x == parts[0].x);
    for (const auto slot : {0u, 2u, 3u, 4u, 5u}) {
        hit = settingsPanelHitTest(parts[slot].x + 1.0F, parts[slot].y + 1.0F, wrapped, buttonRows);
        assert(hit.kind == PanelHitKind::Button && hit.part == static_cast<int>(slot));
    }
}

int main() {
    embyLibraryPrefsTests();
    embyLibrarySettingsFormatTests();
    overlayLayoutTests();
    settingsPanelTests();
    wrappedPanelNoteTests();
    panelAddActionTests();
    seekBarrierDetachTests();
    deckPolicyTests();
    fileAssociationTests();
    assert(std::abs(advanceOverlayAnimation(0.0F, true, 90.0F) - 0.5F) < 0.001F);
    assert(advanceOverlayAnimation(0.9F, true, 90.0F) == 1.0F);
    assert(std::abs(advanceOverlayAnimation(1.0F, false, 45.0F) - 0.75F) < 0.001F);
    assert(!shouldShowPaneTimeline(true, true, false, true, false));
    assert(!shouldShowPaneTimeline(true, true, true, false, false));
    assert(!shouldShowPaneTimeline(true, false, true, true, false));
    assert(shouldShowPaneTimeline(true, false, false, false, true));
    assert(!shouldShowPaneTimeline(false, true, true, true, true));

    assert(dockInteractiveHeight(0.0F, 46, 22) == 22);
    assert(dockInteractiveHeight(0.5F, 46, 22) == 23);
    assert(dockInteractiveHeight(1.0F, 46, 22) == 46);
    assert(pointerInDockInteractiveBand(77, 100, 0.5F, 46, 22));
    assert(!pointerInDockInteractiveBand(76, 100, 0.5F, 46, 22));
    assert(pointerInDockInteractiveBand(78, 100, 0.0F, 46, 22));
    assert(!pointerInDockInteractiveBand(77, 100, 0.0F, 46, 22));

    assert(incomingDropTarget(true, 1) == -1);
    assert(incomingDropTarget(false, 1) == 1);
    assert(centeredWindowStart(100, 1100, 820) == 190);
    assert(centeredWindowStart(100, 700, 820) == 100);

    const auto cells = quadCells(1920.0F, 1080.0F);
    assert(cells[0].width == 960.0F && cells[0].height == 540.0F);
    assert(cells[3].x == 960.0F && cells[3].y == 540.0F);
    assert(paneAt(10, 10, 1920, 1080) == 0);
    assert(paneAt(1000, 10, 1920, 1080) == 1);
    assert(paneAt(10, 600, 1920, 1080) == 2);
    assert(paneAt(1000, 600, 1920, 1080) == 3);

    const auto side = layoutCells(1000, 500, LayoutMode::SideBySide);
    assert(side[0].width == 500 && side[0].height == 500);
    assert(side[1].x == 500 && side[2].width == 0);
    assert(paneAt(750, 250, 1000, 500, LayoutMode::SideBySide) == 1);

    const auto row = layoutCells(1000, 400, LayoutMode::Row4);
    assert(row[3].x == 750 && row[3].width == 250);
    assert(paneAt(630, 200, 1000, 400, LayoutMode::Row4) == 2);

    const PaneArray<bool> threeActive{true, false, true, true, false};
    assert(supportsExpandedPaneCount(3));
    assert(supportsExpandedPaneCount(5));
    assert(!supportsExpandedPaneCount(2));
    assert(!supportsExpandedPaneCount(4));
    assert(retainedExpandedPane(2, threeActive) == 2);
    assert(retainedExpandedPane(1, threeActive) == -1);
    const auto dynamicRow = activeLayoutCells(1200, 600, threeActive, LayoutMode::SideBySide);
    assert(dynamicRow[0].width == 400 && dynamicRow[2].x == 400 && dynamicRow[3].x == 800);
    assert(dynamicRow[1].width == 0);
    const auto dynamicGrid = activeLayoutCells(1200, 600, threeActive, LayoutMode::Row4);
    assert(dynamicGrid[3].width == 1200 && dynamicGrid[3].y == 300);
    const auto expandedGrid = activeLayoutCells(1200, 600, threeActive, LayoutMode::Row4, 2);
    assert(expandedGrid[2].width == 600 && expandedGrid[2].height == 600);
    assert(expandedGrid[0].x == 600 && expandedGrid[3].x == 600);
    assert(activePaneAt(900, 450, 1200, 600, threeActive, LayoutMode::Row4, 2) == 3);

    const PaneArray<float> portraitAndLandscapes{9.0F / 16.0F, 0.0F,
                                                 16.0F / 9.0F, 16.0F / 9.0F, 0.0F};
    const auto portraitAuto = activeLayoutCells(
        1200, 600, threeActive, LayoutMode::Grid2x2, -1, -1,
        portraitAndLandscapes);
    assert(std::abs(portraitAuto[0].width - 504.0F) < 0.001F);
    assert(portraitAuto[0].height == 600 && portraitAuto[2].x == portraitAuto[0].width);
    assert(portraitAuto[2].height == 300 && portraitAuto[3].y == 300);
    assert(activePaneAt(900, 450, 1200, 600, threeActive, LayoutMode::Grid2x2,
                        -1, -1, portraitAndLandscapes) == 3);

    // Auto may choose a source whose storage slot is not first, but V numbers
    // follow visible positions: the large left cell is V1, then the two cells
    // on the right. Locking that focus keeps replacement metadata from moving
    // the panes after the user chose a physical cell to replace.
    const PaneArray<bool> firstThreeActive{true, true, true, false, false};
    const PaneArray<float> portraitLast{
        16.0F / 9.0F, 16.0F / 9.0F, 9.0F / 16.0F, 0.0F, 0.0F};
    const auto portraitLastCells = activeLayoutCells(
        1200, 600, firstThreeActive, LayoutMode::Grid2x2, -1, -1, portraitLast);
    const auto portraitLastOrder = panesByDisplayPosition(portraitLastCells);
    assert(portraitLastOrder[0] == 2 && portraitLastOrder[1] == 0 &&
           portraitLastOrder[2] == 1 && portraitLastOrder[3] == 3);
    const PaneArray<float> replacementLandscapes{
        16.0F / 9.0F, 16.0F / 9.0F, 16.0F / 9.0F, 0.0F, 0.0F};
    const auto lockedReplacement = activeLayoutCells(
        1200, 600, firstThreeActive, LayoutMode::Grid2x2, -1, -1,
        replacementLandscapes, AutoLayoutFocus::Tall, 2);
    assert(lockedReplacement[2].x == 0.0F && lockedReplacement[2].height == 600.0F);
    assert(lockedReplacement[0].x == lockedReplacement[2].width &&
           lockedReplacement[1].y == 300.0F);
    const auto lockedRegularGrid = activeLayoutCells(
        1200, 600, firstThreeActive, LayoutMode::Grid2x2, -1, -1,
        portraitLast, AutoLayoutFocus::None, -1);
    assert(lockedRegularGrid[0].x == 0.0F && lockedRegularGrid[0].y == 0.0F);
    assert(lockedRegularGrid[1].x == 600.0F && lockedRegularGrid[1].y == 0.0F);
    assert(lockedRegularGrid[2].x == 0.0F && lockedRegularGrid[2].y == 300.0F);

    const auto column = activeLayoutCells(
        1200, 600, threeActive, LayoutMode::Column4);
    assert(column[0].height == 200 && column[2].y == 200 && column[3].y == 400);

    const PaneArray<float> landscapeAndPortraits{16.0F / 9.0F, 0.0F,
                                                 9.0F / 16.0F, 9.0F / 16.0F, 0.0F};
    const auto landscapePair = activeLayoutCells(
        1200, 600, threeActive, LayoutMode::LandscapePair, -1, -1,
        landscapeAndPortraits);
    assert(std::abs(landscapePair[0].height - 348.0F) < 0.001F);
    assert(landscapePair[2].y == landscapePair[0].height && landscapePair[3].x == 600);

    const PaneArray<bool> fiveActive{true, true, true, true, true};
    const PaneArray<bool> fourActive{true, true, true, true, false};
    assert(retainedExpandedPane(4, fiveActive) == 4);
    assert(retainedExpandedPane(2, fourActive) == -1);
    const auto fiveExpanded = activeLayoutCells(
        1500, 900, fiveActive, LayoutMode::PortraitStack, 4);
    assert(fiveExpanded[4].x == 0.0F && fiveExpanded[4].y == 0.0F);
    assert(fiveExpanded[4].width == 750.0F && fiveExpanded[4].height == 900.0F);
    assert(fiveExpanded[0].x == 750.0F && fiveExpanded[0].y == 0.0F);
    assert(fiveExpanded[0].width == 375.0F && fiveExpanded[0].height == 450.0F);
    assert(fiveExpanded[1].x == 1125.0F && fiveExpanded[1].y == 0.0F);
    assert(fiveExpanded[2].x == 750.0F && fiveExpanded[2].y == 450.0F);
    assert(fiveExpanded[3].x == 1125.0F && fiveExpanded[3].y == 450.0F);
    // A stale expanded-pane value loaded with an unsupported source count is
    // ignored by geometry just as it is disabled in the menu.
    const auto staleFourExpanded = activeLayoutCells(
        1500, 900, fourActive, LayoutMode::Grid2x2, 2);
    assert(staleFourExpanded[0].x == 0.0F && staleFourExpanded[0].y == 0.0F);
    assert(staleFourExpanded[0].width == 750.0F && staleFourExpanded[0].height == 450.0F);
    assert(staleFourExpanded[2].x == 0.0F && staleFourExpanded[2].y == 450.0F);
    const PaneArray<float> onePortrait{
        16.0F / 9.0F, 16.0F / 9.0F, 16.0F / 9.0F, 16.0F / 9.0F, 9.0F / 16.0F};
    const auto portraitWithFour = activeLayoutCells(
        1500, 900, fiveActive, LayoutMode::Grid2x2, -1, -1, onePortrait);
    assert(portraitWithFour[4].x == 0.0F && portraitWithFour[4].height == 900.0F);
    assert(portraitWithFour[0].x == portraitWithFour[4].width);
    assert(portraitWithFour[1].x > portraitWithFour[0].x);
    assert(portraitWithFour[2].y == 450.0F && portraitWithFour[3].y == 450.0F);
    const auto portraitWithFourOrder = panesByDisplayPosition(portraitWithFour);
    assert(portraitWithFourOrder[0] == 4 && portraitWithFourOrder[1] == 0 &&
           portraitWithFourOrder[2] == 1 && portraitWithFourOrder[3] == 2 &&
           portraitWithFourOrder[4] == 3);

    const PaneArray<float> oneLandscape{
        9.0F / 16.0F, 9.0F / 16.0F, 9.0F / 16.0F, 9.0F / 16.0F, 16.0F / 9.0F};
    const auto landscapeWithFour = activeLayoutCells(
        1500, 900, fiveActive, LayoutMode::Grid2x2, -1, -1, oneLandscape);
    assert(landscapeWithFour[4].x == 0.0F && landscapeWithFour[4].width == 1500.0F);
    assert(std::abs(landscapeWithFour[4].height - 378.0F) < 0.001F);
    for (std::size_t pane = 0; pane < kLegacyPaneCount; ++pane) {
        assert(landscapeWithFour[pane].y == landscapeWithFour[4].height);
        assert(landscapeWithFour[pane].width == 375.0F);
    }

    const PaneArray<float> mixedFive{
        9.0F / 16.0F, 16.0F / 9.0F, 1.0F, 21.0F / 9.0F, 4.0F / 5.0F};
    const auto mixedFiveCells = activeLayoutCells(
        1500, 900, fiveActive, LayoutMode::Grid2x2, -1, -1,
        mixedFive, AutoLayoutFocus::None);
    assert(mixedFiveCells[0].y == 0.0F && mixedFiveCells[2].y == 0.0F);
    assert(mixedFiveCells[3].y > 0.0F && mixedFiveCells[4].y == mixedFiveCells[3].y);
    assert(mixedFiveCells[0].width < mixedFiveCells[1].width);
    assert(mixedFiveCells[3].width > mixedFiveCells[4].width);
    assert(std::abs(mixedFiveCells[0].width + mixedFiveCells[1].width +
                    mixedFiveCells[2].width - 1500.0F) < 0.01F);
    assert(activePaneAt(1499, 899, 1500, 900, fiveActive, LayoutMode::Grid2x2,
                        -1, -1, mixedFive, AutoLayoutFocus::None) == 4);

    assert(!sourceHasStarted(9.9, 10.0));
    assert(sourceHasStarted(10.0, 10.0));
    assert(std::abs(sourceTime(12.5, 10.0) - 2.5) < 0.001);
    assert(sourceTime(5.0, 10.0) == 0.0);
    assert(timelineDuration(60.0, 10.0) == 70.0);
    assert(std::abs(sourceTime(20.0, 10.0, 2.5) - 12.5) < 0.001);
    assert(std::abs(sourceTime(20.0, 10.0, -2.5) - 7.5) < 0.001);
    assert(std::abs(timelineDuration(60.0, 10.0, 2.5) - 67.5) < 0.001);
    assert(std::abs(timelineDuration(60.0, 10.0, -2.5) - 72.5) < 0.001);
    const double linkedTarget = timelineForSourceTime(25.0, 10.0, 2.5);
    assert(std::abs(linkedTarget - 32.5) < 0.001);
    assert(std::abs(sourceTime(linkedTarget, 10.0, 2.5) - 25.0) < 0.001);
    assert(linkedTimelineCanReachSourceTime(25.0, 2.5));
    assert(linkedTimelineCanReachSourceTime(0.0, -2.5));
    assert(linkedTimelineCanReachSourceTime(12.5, 12.5));
    assert(!linkedTimelineCanReachSourceTime(0.0, 12.5));
    assert(!timelineMappingChanged(12.5, 12.5004));
    assert(timelineMappingChanged(12.5, 12.501));
    assert(timelineMappingChanged(std::nan(""), 12.5));
    assert(std::abs(sourceTime(20.0, 10.0, 2.5, 2.0) - 22.5) < 0.001);
    assert(std::abs(timelineForSourceTime(22.5, 10.0, 2.5, 2.0) - 20.0) < 0.001);
    assert(std::abs(timelineDuration(60.0, 10.0, 0.0, 2.0) - 40.0) < 0.001);
    assert(std::abs(effectiveTimelineOffset(10.0, 2.5, 1.0) + 7.5) < 0.001);
    assert(!sourceIsActive(5.0, 10.0, 0.0));
    assert(sourceIsActive(0.0, 10.0, 12.0));
    assert(std::abs(sourceTime(0.0, 0.0, 15.0) - 15.0) < 0.001);
    assert(!sourceIsActive(5.0, 0.0, -10.0));
    assert(sourceIsActive(10.0, 0.0, -10.0));
    // Replacing the longest pane cannot shrink the master duration while its
    // metadata is still opening. A settled (or failed) replacement resumes
    // the normal ready-source duration and end policy.
    assert(stableTimelineDuration(300.0, 60.0, true) == 300.0);
    assert(stableTimelineDuration(0.0, 60.0, true) == 60.0);
    assert(stableTimelineDuration(60.0, 400.0, true) == 400.0);
    assert(stableTimelineDuration(300.0, 60.0, false) == 60.0);
    assert(masterTimelineEndAction(
               120.0, 60.0, true, true, false, false, false) ==
           MasterTimelineEndAction::None);
    assert(masterTimelineEndAction(
               120.0, 60.0, true, false, false, false, false) ==
           MasterTimelineEndAction::Pause);
    assert(masterTimelineEndAction(
               120.0, 60.0, true, false, false, false, true) ==
           MasterTimelineEndAction::Restart);
    assert(masterTimelineEndAction(
               120.0, 60.0, true, false, true, false, false) ==
           MasterTimelineEndAction::None);
    assert(masterTimelineEndAction(
               60.0, 60.0, true, false, false, true, false) ==
           MasterTimelineEndAction::None);
    assert(nextEligiblePane(0, {false, false, true, false, false}) == 2);
    assert(nextEligiblePane(3, {false, true, false, false, false}) == 1);
    assert(nextEligiblePane(1, {false, false, false, false, false}) == -1);
    assert(seekPreviewEligible(95.0, 100.0));
    assert(!seekPreviewEligible(70.0, 100.0));
    assert(!seekPreviewEligible(100.001, 100.0));
    assert(!seekPreviewEligible(101.0, 100.0));
    assert(seekKeyframePreviewEligible(true, 95.0, 100.0));
    assert(!seekKeyframePreviewEligible(false, 95.0, 100.0));
    assert(!seekKeyframePreviewEligible(true, 70.0, 100.0));

    // --- Exact seek frame intervals --------------------------------------
    // Stream ticks avoid a fixed seconds window. At this target, the frame
    // accepted by the old target-50ms rule for 60fps has already ended, while
    // a 24fps frame with the same starting PTS still covers the target.
    constexpr std::int64_t seekTargetPts = 10100;
    assert(frameEndsAtOrBeforeSeekTarget(10050, 17, seekTargetPts));
    assert(frameEndsAtOrBeforeSeekTarget(10083, 17, seekTargetPts));
    assert(!frameEndsAtOrBeforeSeekTarget(10100, 17, seekTargetPts));
    assert(!frameEndsAtOrBeforeSeekTarget(10083, 42, seekTargetPts));
    // A guessed 30fps duration in a millisecond time base is 33.333 ticks.
    // The decoder bridge rounds that up: 34 still covers target 1000, while
    // truncating it to 33 would incorrectly discard this frame.
    assert(frameEndsAtOrBeforeSeekTarget(967, 33, 1000));
    assert(!frameEndsAtOrBeforeSeekTarget(967, 34, 1000));
    // Variable-frame-rate intervals are authoritative even when much longer
    // than the nominal stream rate.
    assert(!frameEndsAtOrBeforeSeekTarget(10000, 120, seekTargetPts));
    // Signed stream origins retain exact interval arithmetic without a
    // potentially overflowing signed subtraction.
    assert(frameEndsAtOrBeforeSeekTarget(-5, 10, 5));
    assert(!frameEndsAtOrBeforeSeekTarget(-5, 11, 5));
    // When neither the frame nor stream supplies a duration, discard every
    // earlier PTS and accept the first timestamp at or after the target.
    assert(frameEndsAtOrBeforeSeekTarget(10099, 0, seekTargetPts));
    assert(!frameEndsAtOrBeforeSeekTarget(10100, 0, seekTargetPts));
    assert(!frameEndsAtOrBeforeSeekTarget(10150, 0, seekTargetPts));

    // Seconds are rounded to the nearest stream tick, not truncated. Binary
    // representation puts this exact millisecond boundary infinitesimally
    // below 2001 after division on common implementations.
    assert(secondsToStreamPts(2.001, 1, 1000) == 2001);
    assert(streamTimestampForSeconds(2.001, 9000, 1, 1000) == 11001);
    constexpr double ntscFrameSeconds = 1001.0 / 30000.0;
    assert(secondsToStreamPts(ntscFrameSeconds * 123.0, 1001, 30000) == 123);
    assert(secondsToStreamPts(0.00049, 1, 1000) == 0);
    assert(secondsToStreamPts(0.00051, 1, 1000) == 1);
    assert(secondsToStreamPts(std::numeric_limits<double>::quiet_NaN(), 1, 1000) == 0);
    assert(secondsToStreamPts(std::numeric_limits<double>::infinity(), 1, 1000) == 0);
    assert(secondsToStreamPts(1.0, 0, 1000) == 0);
    assert(secondsToStreamPts(1.0, 1, 0) == 0);
    assert(secondsToStreamPts(1.0e300, 1, 1000) == INT64_MAX);
    assert(secondsToStreamPts(-1.0e300, 1, 1000) == INT64_MIN);
    assert(streamTimestampForSeconds(
               std::numeric_limits<double>::quiet_NaN(), 1234, 1, 1000) == 1234);
    assert(streamTimestampForSeconds(
               std::numeric_limits<double>::infinity(), -1234, 1, 1000) == -1234);
    assert(streamTimestampForSeconds(1.0, 1234, -1, 1000) == 1234);
    assert(streamTimestampForSeconds(1.0, INT64_MAX - 5, 1, 1) == INT64_MAX - 4);
    assert(streamTimestampForSeconds(10.0, INT64_MAX - 5, 1, 1) == INT64_MAX);
    assert(streamTimestampForSeconds(-10.0, INT64_MIN + 5, 1, 1) == INT64_MIN);

    // Container duration may extend well past the video stream. Demuxing and
    // the 750ms full-quality window target the final video tick, while a normal
    // in-stream seek remains unchanged.
    assert(videoSeekDecodeTargetPts(120000, 0, 100000) == 99999);
    assert(videoSeekDecodeTargetPts(50000, 0, 100000) == 50000);
    assert(videoSeekDecodeTargetPts(11000, 9000, 1000) == 9999);
    assert(videoSeekDecodeTargetPts(-7000, -9000, 1000) == -8001);
    assert(videoSeekDecodeTargetPts(9000, 9000, 1) == 9000);
    // Unknown duration preserves the request, and an overflowing declared end
    // saturates rather than wrapping behind the seek target.
    assert(videoSeekDecodeTargetPts(120000, 0, 0) == 120000);
    assert(videoSeekDecodeTargetPts(120000, 0, INT64_MIN) == 120000);
    assert(videoSeekDecodeTargetPts(
               INT64_MAX, INT64_MAX - 5, 10) == INT64_MAX);
    const auto paddedDecodeTarget = videoSeekDecodeTargetPts(120000, 0, 100000);
    assert(std::abs(videoSeekFastDecodeUntilSeconds(
               paddedDecodeTarget, 0, 0.001) - 99.249) < 0.000001);
    assert(std::abs(videoSeekFastDecodeUntilSeconds(
               50000, 0, 0.001) - 49.250) < 0.000001);
    assert(std::abs(videoSeekFastDecodeUntilSeconds(
               -8001, -9000, 0.001) - 0.249) < 0.000001);
    assert(videoSeekFastDecodeUntilSeconds(500, 0, 0.001) < 0.0);
    assert(validLoop(5.0, 10.0));
    assert(std::abs(loopedTime(12.5, 5.0, 10.0, true) - 7.5) < 0.001);
    assert(loopedTime(4.0, 5.0, 10.0, true) == 4.0);
    // A alone loops to the end; a B before A counts as unset; no end, no loop.
    assert(loopEnd(5.0, 10.0, 30.0) == 10.0);
    assert(loopEnd(5.0, 0.0, 30.0) == 30.0);
    assert(loopEnd(5.0, 3.0, 30.0) == 30.0);
    assert(loopArmed(5.0, 0.0, 30.0) && !loopArmed(5.0, 0.0, 0.0) && !loopArmed(5.0, 0.0, 5.0));
    assert(loopArmed(0.0, 0.0, 30.0));
    assert(std::abs(loopedTime(32.0, 5.0, loopEnd(5.0, 0.0, 30.0), true) - 7.0) < 0.001);
    {
        PaneTiming openEnded;
        openEnded.loaded = openEnded.ready = true;
        openEnded.duration = 30.0;
        openEnded.loopEnabled = true;
        openEnded.loopA = 5.0;
        openEnded.loopB = 0.0;
        assert(std::abs(mappedPaneTime(openEnded, 32.0, SeekMode::Linked) - 7.0) < 0.001);
        assert(paneBeforeEnd(openEnded, 32.0, SeekMode::Linked));
        // Without a known duration the open-ended loop cannot wrap and must
        // not pretend to repeat, or the audio handoff would never happen.
        openEnded.duration = 0.0;
        assert(mappedPaneTime(openEnded, 32.0, SeekMode::Linked) == 32.0);
        assert(!paneRepeats(openEnded, SeekMode::Linked));
    }
    const auto clampedVibrance = clampSmartVibranceSettings({4.0F, 0.1F, 0.02F, 100.0F});
    assert(clampedVibrance.intensity == 3.0F);
    assert(clampedVibrance.saturationPivot == 0.2F);
    assert(clampedVibrance.grayPivot == 0.01F);
    assert(clampedVibrance.graySharpness == 80.0F);

    SessionState saved;
    saved.timeline = 42.25;
    saved.layout = LayoutMode::PortraitStack;
    saved.masterLoopEnabled = true;
    saved.masterLoopA = 10.0;
    saved.masterLoopB = 20.0;
    saved.repeatAll = false;
    saved.expandedPane = 1;
    saved.audioMask = 0b11011;
    saved.shader = ShaderPreset::SmartVibrancePlus;
    saved.smartVibrance = {2.25F, 0.72F, 0.0042F, 61.0F};
    saved.panes[1].path = L"C:\\Videos\\two.mp4";
    saved.panes[1].rate = 1.5;
    saved.panes[1].repeat = true;
    saved.panes[1].view = {ViewMode::Fill, 1.25F};
    saved.panes[1].volume = 0.35F;
    saved.panes[2].muted = true;
    saved.panes[4].path = L"C:\\Videos\\five.mp4";
    saved.panes[4].repeat = true;
    saved.panes[4].volume = 0.55F;
    // An open-ended loop (A only) survives the round trip enabled; a loop
    // with a negative A does not.
    saved.panes[4].loopEnabled = true;
    saved.panes[4].loopA = 12.0;
    saved.panes[4].loopB = 0.0;
    saved.masterLoopEnabled = true;
    saved.masterLoopA = 3.0;
    saved.masterLoopB = 0.0;
    std::wstringstream sessionText;
    assert(writeSession(sessionText, saved));
    SessionState loaded;
    assert(readSession(sessionText, loaded));
    assert(loaded.panes[4].loopEnabled && loaded.panes[4].loopA == 12.0);
    assert(loaded.masterLoopEnabled && loaded.masterLoopA == 3.0);
    assert(std::abs(loaded.timeline - 42.25) < 0.001);
    assert(loaded.layout == LayoutMode::PortraitStack);
    assert(loaded.panes[1].path == saved.panes[1].path);
    assert(std::abs(loaded.panes[1].rate - 1.5) < 0.001);
    assert(loaded.panes[1].view.mode == ViewMode::Fill);
    assert(loaded.audioMask == 0b11011 && loaded.panes[1].repeat);
    assert(!loaded.repeatAll && loaded.expandedPane == 1);
    assert(loaded.shader == ShaderPreset::SmartVibrancePlus);
    assert(std::abs(loaded.smartVibrance.intensity - 2.25F) < 0.001F);
    assert(std::abs(loaded.smartVibrance.grayPivot - 0.0042F) < 0.00001F);
    assert(std::abs(loaded.panes[1].volume - 0.35F) < 0.001F);
    assert(loaded.panes[2].muted && !loaded.panes[1].muted);
    assert(std::abs(loaded.panes[0].volume - 1.0F) < 0.001F);
    assert(loaded.panes[4].path == saved.panes[4].path && loaded.panes[4].repeat);
    assert(std::abs(loaded.panes[4].volume - 0.55F) < 0.001F);

    StyleState visual;
    visual.layout = LayoutMode::LandscapePair;
    visual.expandedPane = 2;
    visual.shader = ShaderPreset::SmartVibrancePlus;
    visual.smartVibrance = {1.75F, 0.64F, 0.0025F, 52.0F};
    visual.controlsPinned = true;
    visual.customShaderPath = L"C:\\Shaders\\clean.txt";
    visual.paneViews[0] = {ViewMode::Fill, 1.5F};
    std::wstringstream styleText;
    assert(writeStyle(styleText, visual));
    StyleState visualLoaded;
    assert(readStyle(styleText, visualLoaded));
    assert(visualLoaded.layout == LayoutMode::LandscapePair);
    assert(visualLoaded.expandedPane == 2);
    assert(visualLoaded.shader == ShaderPreset::SmartVibrancePlus);
    assert(std::abs(visualLoaded.smartVibrance.saturationPivot - 0.64F) < 0.001F);
    assert(std::abs(visualLoaded.smartVibrance.graySharpness - 52.0F) < 0.001F);
    assert(visualLoaded.controlsPinned);
    assert(visualLoaded.customShaderPath == visual.customShaderPath);
    assert(visualLoaded.paneViews[0].mode == ViewMode::Fill);
    assert(std::abs(visualLoaded.paneViews[0].zoom - 1.5F) < 0.001F);

    AppSettings preferences;
    preferences.nasCache = false;
    preferences.rtxVideo.superResolution = true;
    preferences.rtxVideo.rtxHdr = true;
    preferences.embyBrowser.view = 1;
    preferences.embyBrowser.sort = 3;
    preferences.embyBrowser.descending = true;
    preferences.embyBrowser.unplayed = true;
    preferences.embyBrowser.flat = false;
    preferences.playOrder = PlayOrder::Shuffle;
    preferences.keyframeSeek = false;
    preferences.subtitles = {false, 2, 1.5F, 0.25F, true};
    preferences.style = visual;
    preferences.decode = DecodeMode::Software;
    preferences.seek = SeekMode::Independent;
    preferences.repeatAll = false;
    preferences.audioMask = 0b11101;
    preferences.paneRepeat = {true, false, true, false, true};
    preferences.volume = 0.42F;
    preferences.muted = true;
    preferences.paneVolume = {1.0F, 0.25F, 0.8F, 0.0F, 0.6F};
    preferences.paneMuted = {false, false, true, false, true};
    preferences.windowX = -1200;
    preferences.windowY = 80;
    preferences.windowWidth = 1600;
    preferences.windowHeight = 1000;
    preferences.maximized = true;
    std::wstringstream settingsText;
    assert(writeAppSettings(settingsText, preferences));
    AppSettings preferencesLoaded;
    assert(readAppSettings(settingsText, preferencesLoaded));
    assert(!preferencesLoaded.nasCache);
    assert(preferencesLoaded.rtxVideo.superResolution && preferencesLoaded.rtxVideo.rtxHdr);
    assert(preferencesLoaded.embyBrowser == preferences.embyBrowser);
    assert(preferencesLoaded.embyLibraries.entries.empty());
    assert(preferencesLoaded.playOrder == PlayOrder::Shuffle);
    assert(!preferencesLoaded.keyframeSeek);
    assert(preferencesLoaded.subtitles == preferences.subtitles);
    // Before QCONFIG 14 the position was every bottom line's height above
    // the pane's edge, not how far they are raised above their script's own
    // place: an old value is dropped, the rest of the line kept.
    auto configV13 = settingsText.str();
    assert(configV13.rfind(L"QCONFIG 14\n", 0) == 0);
    configV13.replace(0, std::wstring(L"QCONFIG 14").size(), L"QCONFIG 13");
    {
        std::wistringstream configV13Input(configV13);
        AppSettings v13Settings;
        assert(readAppSettings(configV13Input, v13Settings));
        assert(v13Settings.subtitles == (SubtitleSettings{false, 2, 1.5F, 0.0F, true}));
        assert(SubtitleSettings{}.position == 0.0F);
    }
    // QCONFIG 10 had no subtitle line: they are shown, in the language
    // Windows names, at the default size and place.
    auto configV12 = configV13;
    configV12.replace(0, std::wstring(L"QCONFIG 13").size(), L"QCONFIG 12");
    const std::wstring libraryDetailsLine = L"embylibrarydetails 0\n";
    assert(configV12.find(libraryDetailsLine) != std::wstring::npos);
    configV12.erase(configV12.find(libraryDetailsLine), libraryDetailsLine.size());
    auto configV10 = configV12;
    assert(configV10.rfind(L"QCONFIG 12\n", 0) == 0);
    configV10.replace(0, std::wstring(L"QCONFIG 12").size(), L"QCONFIG 10");
    const std::wstring subtitleLine = L"subtitles 0 2 1.5 0.25 1\n";
    assert(configV10.find(subtitleLine) != std::wstring::npos);
    configV10.erase(configV10.find(subtitleLine), subtitleLine.size());
    {
        // QCONFIG 11 had the line without its last word, and drew the box
        // behind every line: it is read, and as without the box. A 12
        // that lacks the word is rejected.
        auto configV11 = configV12;
        configV11.replace(0, std::wstring(L"QCONFIG 12").size(), L"QCONFIG 11");
        configV11.replace(configV11.find(subtitleLine), subtitleLine.size(), L"subtitles 0 2 1.5 0.25\n");
        std::wistringstream configV11Input(configV11);
        AppSettings v11Settings;
        v11Settings.subtitles.background = true;
        assert(readAppSettings(configV11Input, v11Settings));
        assert(v11Settings.subtitles == (SubtitleSettings{false, 2, 1.5F, 0.0F, false}));
        // What follows the line is read from where it ends.
        assert(v11Settings.style.customShaderPath == visual.customShaderPath);
        configV11.replace(0, std::wstring(L"QCONFIG 11").size(), L"QCONFIG 12");
        std::wistringstream shortLineInput(configV11);
        assert(!readAppSettings(shortLineInput, v11Settings));
        assert(!SubtitleSettings{}.background);
    }
    {
        std::wistringstream configV10Input(configV10);
        AppSettings v10Settings;
        v10Settings.subtitles = {false, 3, 2.0F, 0.5F};
        assert(readAppSettings(configV10Input, v10Settings));
        assert(v10Settings.subtitles == SubtitleSettings{} && !v10Settings.keyframeSeek);
        // Values out of range are clamped; a missing line in a 12 is rejected.
        auto oddSubtitles = settingsText.str();
        oddSubtitles.replace(oddSubtitles.find(subtitleLine), subtitleLine.size(), L"subtitles 1 9 40 -3 0\n");
        std::wistringstream oddSubtitlesInput(oddSubtitles);
        AppSettings clampedSubtitles;
        assert(readAppSettings(oddSubtitlesInput, clampedSubtitles));
        assert(clampedSubtitles.subtitles.show && clampedSubtitles.subtitles.language == 5 &&
               clampedSubtitles.subtitles.size == kSubtitleSizeMaximum &&
               clampedSubtitles.subtitles.position == 0.0F);
        auto missingSubtitles = settingsText.str();
        missingSubtitles.erase(missingSubtitles.find(subtitleLine), subtitleLine.size());
        std::wistringstream missingSubtitlesInput(missingSubtitles);
        assert(!readAppSettings(missingSubtitlesInput, clampedSubtitles));
        const SubtitleSettings odd = clampSubtitleSettings({true, -4, 0.01F, 3.0F});
        assert(odd.language == 0 && odd.size == kSubtitleSizeMinimum && odd.position == kSubtitlePositionMaximum);
    }
    // QCONFIG 9 had no keyframe-seek line: one video seeks to keyframes.
    auto configV9 = configV10;
    assert(configV9.rfind(L"QCONFIG 10\n", 0) == 0);
    configV9.replace(0, std::wstring(L"QCONFIG 10").size(), L"QCONFIG 9");
    const auto keyframeLine = configV9.find(L"keyframeseek 0\n");
    assert(keyframeLine != std::wstring::npos);
    configV9.erase(keyframeLine, std::wstring(L"keyframeseek 0\n").size());
    {
        std::wistringstream configV9Input(configV9);
        AppSettings v9Settings;
        v9Settings.keyframeSeek = false;
        assert(readAppSettings(configV9Input, v9Settings));
        assert(v9Settings.keyframeSeek && v9Settings.playOrder == PlayOrder::Shuffle);
        auto missingKeyframe = settingsText.str();
        missingKeyframe.erase(missingKeyframe.find(L"keyframeseek 0\n"), std::wstring(L"keyframeseek 0\n").size());
        std::wistringstream missingKeyframeInput(missingKeyframe);
        assert(!readAppSettings(missingKeyframeInput, v9Settings));
    }
    // QCONFIG 8 had no playback order: the only video plays on in order.
    auto configV8 = configV9;
    configV8.replace(0, std::wstring(L"QCONFIG 9").size(), L"QCONFIG 8");
    const auto orderLine = configV8.find(L"playorder 4\n");
    assert(orderLine != std::wstring::npos);
    configV8.erase(orderLine, std::wstring(L"playorder 4\n").size());
    {
        std::wistringstream configV8Input(configV8);
        AppSettings v8Settings;
        v8Settings.playOrder = PlayOrder::PlayOne;
        assert(readAppSettings(configV8Input, v8Settings));
        assert(v8Settings.playOrder == PlayOrder::InOrder && v8Settings.embyBrowser == preferences.embyBrowser);
        // An order out of range is clamped; a missing line in a 9 is rejected.
        auto oddOrder = settingsText.str();
        oddOrder.replace(oddOrder.find(L"playorder 4"), 11, L"playorder 9");
        std::wistringstream oddOrderInput(oddOrder);
        AppSettings clampedOrder;
        assert(readAppSettings(oddOrderInput, clampedOrder) && clampedOrder.playOrder == PlayOrder::Shuffle);
        auto missingOrder = settingsText.str();
        missingOrder.erase(missingOrder.find(L"playorder 4\n"), std::wstring(L"playorder 4\n").size());
        std::wistringstream missingOrderInput(missingOrder);
        assert(!readAppSettings(missingOrderInput, clampedOrder));
    }
    // QCONFIG 7 had no Emby browser line: the browser comes back as
    // thumbnails by name.
    auto configV7 = configV8;
    configV7.replace(0, std::wstring(L"QCONFIG 8").size(), L"QCONFIG 7");
    const auto browserLine = configV7.find(L"embybrowser 1 3 1 1 0\n");
    assert(browserLine != std::wstring::npos);
    configV7.erase(browserLine, std::wstring(L"embybrowser 1 3 1 1 0\n").size());
    {
        std::wistringstream configV7Input(configV7);
        AppSettings v7Settings;
        v7Settings.embyBrowser.view = 0;
        assert(readAppSettings(configV7Input, v7Settings));
        assert(v7Settings.embyBrowser == EmbyBrowserPrefs{});
        assert(v7Settings.rtxVideo.superResolution && v7Settings.rtxVideo.rtxHdr);
        // An out-of-range view or sort in a QCONFIG 8 is clamped, not rejected.
        auto oddBrowser = settingsText.str();
        oddBrowser.replace(oddBrowser.find(L"embybrowser 1 3 1 1 0"), 21, L"embybrowser 9 9 1 1 0");
        std::wistringstream oddBrowserInput(oddBrowser);
        AppSettings clamped;
        assert(readAppSettings(oddBrowserInput, clamped));
        assert(clamped.embyBrowser.view == 2 && clamped.embyBrowser.sort == 7 && clamped.embyBrowser.descending);
        auto missingBrowser = settingsText.str();
        missingBrowser.erase(missingBrowser.find(L"embybrowser 1 3 1 1 0\n"), std::wstring(L"embybrowser 1 3 1 1 0\n").size());
        std::wistringstream missingBrowserInput(missingBrowser);
        assert(!readAppSettings(missingBrowserInput, clamped));
    }
    // QCONFIG 6 had no RTX Video line: both features come back off.
    auto configV6 = configV7;
    configV6.replace(0, std::wstring(L"QCONFIG 7").size(), L"QCONFIG 6");
    const auto rtxLine = configV6.find(L"rtxvideo 1 1\n");
    assert(rtxLine != std::wstring::npos);
    configV6.erase(rtxLine, std::wstring(L"rtxvideo 1 1\n").size());
    std::wistringstream configV6Input(configV6);
    AppSettings v6Settings;
    v6Settings.rtxVideo.superResolution = true;
    assert(readAppSettings(configV6Input, v6Settings));
    assert(!v6Settings.rtxVideo.superResolution && !v6Settings.rtxVideo.rtxHdr);
    assert(!v6Settings.nasCache);
    // A QCONFIG 7 without the line, or with a bad value, is rejected.
    auto missingRtx = settingsText.str();
    missingRtx.erase(missingRtx.find(L"rtxvideo 1 1\n"), std::wstring(L"rtxvideo 1 1\n").size());
    std::wistringstream missingRtxInput(missingRtx);
    assert(!readAppSettings(missingRtxInput, v6Settings));
    auto malformedRtx = settingsText.str();
    malformedRtx.replace(malformedRtx.find(L"rtxvideo 1 1"), 12, L"rtxvideo 1 x");
    std::wistringstream malformedRtxInput(malformedRtx);
    assert(!readAppSettings(malformedRtxInput, v6Settings));
    auto previousConfig = configV6;
    previousConfig.replace(0, std::wstring(L"QCONFIG 6").size(), L"QCONFIG 5");
    const auto cacheLine = previousConfig.find(L"nascache 0\n");
    assert(cacheLine != std::wstring::npos);
    previousConfig.erase(cacheLine, std::wstring(L"nascache 0\n").size());
    std::wistringstream previousConfigInput(previousConfig);
    AppSettings oldCacheSettings;
    oldCacheSettings.nasCache = false;
    assert(readAppSettings(previousConfigInput, oldCacheSettings));
    assert(oldCacheSettings.nasCache); // old configs get the network-only default
    auto malformedCache = settingsText.str();
    malformedCache.replace(malformedCache.find(L"nascache 0"), 10, L"nascache 2");
    std::wistringstream malformedCacheInput(malformedCache);
    assert(!readAppSettings(malformedCacheInput, oldCacheSettings));

    // --- RTX Video status line ------------------------------------------
    {
        VideoEnhancementSettings wanted;
        VideoEnhancementStatus status;
        status.adapterName = L"Intel(R) Arc(TM) Graphics";
        // Nothing wanted: an explanation, whatever the hardware.
        assert(videoEnhancementNote(wanted, status).find(L"NVIDIA") != std::wstring::npos);
        wanted.superResolution = true;
        // Wanted on the wrong GPU: names the GPU actually rendering.
        assert(videoEnhancementNote(wanted, status).find(L"Intel(R) Arc") != std::wstring::npos);
        status.adapterName = L"NVIDIA GeForce RTX 4060 Ti";
        status.nvidiaAdapter = true;
        // NVIDIA but the driver refused: says so and names the requirement.
        assert(videoEnhancementNote(wanted, status).find(L"declined Super Resolution") != std::wstring::npos);
        status.superResolutionSupported = true;
        // Supported but not magnifying anything: idle, and says why.
        assert(videoEnhancementNote(wanted, status).find(L"idle") != std::wstring::npos);
        status.superResolutionApplied = true;
        assert(videoEnhancementNote(wanted, status) ==
               L"Super Resolution requested for eligible video.");
        wanted.rtxHdr = true;
        // HDR wanted but the driver has no TrueHDR: that outranks the display.
        assert(videoEnhancementNote(wanted, status).find(L"declined RTX Video HDR") != std::wstring::npos);
        assert(videoEnhancementNote(wanted, status).find(L"Windows settings") == std::wstring::npos);
        status.rtxHdrSupported = true;
        // Driver fine, display in SDR: the user has to flip HDR in Windows.
        assert(videoEnhancementNote(wanted, status).find(L"HDR on for this display") != std::wstring::npos);
        status.displayHdr = true;
        assert(videoEnhancementNote(wanted, status).find(L"switching the output") != std::wstring::npos);
        status.hdrOutput = true;
        assert(videoEnhancementNote(wanted, status) ==
               L"PQ output ready; RTX HDR + Super Resolution requested.");
        wanted.superResolution = false;
        assert(videoEnhancementNote(wanted, status) ==
               L"PQ output ready; RTX HDR requested for eligible SDR.");
        // Two problems: the one to fix first wins, and the line stays short.
        status.superResolutionSupported = false;
        status.displayHdr = false;
        wanted.superResolution = true;
        const auto first = videoEnhancementNote(wanted, status);
        assert(first.find(L"declined Super Resolution") != std::wstring::npos &&
               first.find(L"HDR on for this display") == std::wstring::npos);
        // Every line fits the note row: about sixty characters.
        for (const bool sr : {false, true}) {
            for (const bool hdr : {false, true}) {
                for (const bool nvidia : {false, true}) {
                    for (const bool supported : {false, true}) {
                        for (const bool display : {false, true}) {
                            for (const bool output : {false, true}) {
                                VideoEnhancementStatus s;
                                s.adapterName = L"NVIDIA GeForce RTX 4060 Ti";
                                s.nvidiaAdapter = nvidia;
                                s.superResolutionSupported = s.rtxHdrSupported = supported;
                                s.displayHdr = display;
                                s.hdrOutput = output;
                                assert(videoEnhancementNote({sr, hdr}, s).size() <= 66);
                            }
                        }
                    }
                }
            }
        }
    }
    assert(preferencesLoaded.decode == DecodeMode::Software);
    assert(preferencesLoaded.seek == SeekMode::Independent);
    assert(!preferencesLoaded.repeatAll && preferencesLoaded.audioMask == 0b11101);
    assert(preferencesLoaded.paneRepeat == preferences.paneRepeat);
    assert(std::abs(preferencesLoaded.volume - 0.42F) < 0.001F);
    assert(preferencesLoaded.muted && preferencesLoaded.maximized);
    assert(preferencesLoaded.windowX == -1200 && preferencesLoaded.windowWidth == 1600);
    assert(preferencesLoaded.style.customShaderPath == visual.customShaderPath);
    assert(preferencesLoaded.paneVolume == preferences.paneVolume);
    assert(preferencesLoaded.paneMuted == preferences.paneMuted);

    // QDECK 4 predates per-video volume; those panes play at full volume.
    std::wstringstream sessionV4(
        L"QDECK 4\n"
        L"timeline 12 1\n"
        L"settings 0 1 0 0 0 1 1 -1\n"
        L"masterloop 0 0 0\n"
        L"shaderpath \"\"\n"
        L"smartvibrance 1.5 0.5 0.003 45\n"
        L"pane 0 \"\" 0 0 1 0 0 0 0 0 0 0 1\n"
        L"pane 1 \"\" 0 0 1 0 0 0 0 0 0 0 1\n"
        L"pane 2 \"\" 0 0 1 0 0 0 0 0 0 0 1\n"
        L"pane 3 \"\" 0 0 1 0 0 0 0 0 0 0 1\n");
    SessionState sessionV4Loaded;
    assert(readSession(sessionV4, sessionV4Loaded));
    for (const auto& pane : sessionV4Loaded.panes) {
        assert(std::abs(pane.volume - 1.0F) < 0.001F && !pane.muted);
    }
    assert(sessionV4Loaded.panes[4].path.empty() && !sessionV4Loaded.panes[4].repeat);

    // QCONFIG 3 likewise: an existing settings.qconfig must not be rejected,
    // and must not silence anything it never knew about.
    std::wstringstream configV3(
        L"QCONFIG 3\n"
        L"playback 0 0 1 1 0.5 0\n"
        L"window 10 20 1440 900 0\n"
        L"repeatpane 0 0 0 0\n"
        L"QSTYLE 2\n"
        L"settings 0 -1 0 0\n"
        L"shaderpath \"\"\n"
        L"smartvibrance 1.5 0.5 0.003 45\n"
        L"pane 0 0 1\n"
        L"pane 1 0 1\n"
        L"pane 2 0 1\n"
        L"pane 3 0 1\n");
    AppSettings configV3Loaded;
    assert(readAppSettings(configV3, configV3Loaded));
    for (std::size_t pane = 0; pane < kMaxPanes; ++pane) {
        assert(std::abs(configV3Loaded.paneVolume[pane] - 1.0F) < 0.001F);
        assert(!configV3Loaded.paneMuted[pane]);
    }

    // Clearing a paused pane's offset has to move the time it is holding.
    // Resuming rebuilds the adjustment as held - timeline * rate, so a held
    // value left at the old offset would restore the offset just cleared.
    {
        const double timeline = 30.0;
        const double rate = 1.5;
        const double offset = 4.0;
        const double heldWithOffset = sourceTime(timeline, 0.0, offset, rate);
        const double heldAtZero = sourceTime(timeline, 0.0, 0.0, rate);
        assert(std::abs(heldWithOffset - (timeline * rate + offset)) < 1e-9);
        assert(std::abs(heldAtZero - timeline * rate) < 1e-9);
        // What toggleSourcePause recomputes on resume, for each held value.
        assert(std::abs((heldWithOffset - timeline * rate) - offset) < 1e-9);
        assert(std::abs((heldAtZero - timeline * rate) - 0.0) < 1e-9);
        // A negative offset larger than the elapsed time clamps at zero rather
        // than resuming before the start of the source.
        assert(sourceTime(1.0, 0.0, -90.0, rate) == 0.0);
    }

    // QDECK 1 files remain readable and inherit the new safe defaults.
    std::wstringstream legacy(
        L"QDECK 1\n"
        L"timeline 5 0\n"
        L"settings 0 0 0 0 0 1\n"
        L"masterloop 0 0 0\n"
        L"shaderpath \"\"\n"
        L"pane 0 \"\" 0 0 1 0 0 0 0 0 0 1\n"
        L"pane 1 \"\" 0 0 1 0 0 0 0 0 0 1\n"
        L"pane 2 \"\" 0 0 1 0 0 0 0 0 0 1\n"
        L"pane 3 \"\" 0 0 1 0 0 0 0 0 0 1\n");
    SessionState legacyLoaded;
    assert(readSession(legacy, legacyLoaded));
    assert(legacyLoaded.repeatAll && legacyLoaded.expandedPane == -1);
    assert(legacyLoaded.audioMask == 1 && !legacyLoaded.panes[0].repeat);
    assert(legacyLoaded.shader == ShaderPreset::Normal);
    assert(std::abs(legacyLoaded.smartVibrance.intensity - 1.5F) < 0.001F);

    // QCONFIG 1 stores one selected pane; it is promoted to a one-bit audio mask.
    std::wstringstream legacySettings(
        L"QCONFIG 1\n"
        L"playback 0 0 1 3 0.75 0\n"
        L"window 10 20 1280 720 0\n"
        L"QSTYLE 1\n"
        L"settings 0 -1 0 0\n"
        L"shaderpath \"\"\n"
        L"pane 0 0 1\n"
        L"pane 1 0 1\n"
        L"pane 2 0 1\n"
        L"pane 3 0 1\n");
    AppSettings legacyPreferences;
    assert(readAppSettings(legacySettings, legacyPreferences));
    assert(legacyPreferences.audioMask == 0b1000);
    assert(std::none_of(legacyPreferences.paneRepeat.begin(),
                        legacyPreferences.paneRepeat.end(), [](bool repeat) { return repeat; }));
    assert(std::abs(legacyPreferences.style.smartVibrance.grayPivot - 0.003F) < 0.00001F);

    const std::string potShader =
        "sampler s0 : register(s0);\nfloat4 p0 : register(c0);\n"
        "float4 main(float2 uv:TEXCOORD0):COLOR { return tex2D(s0, uv); }";
    const std::string adapted = adaptPotPlayerShader(potShader);
    assert(adapted.find("Texture2D qdTexture") != std::string::npos);
    assert(adapted.find("PotPlayerParams") != std::string::npos);
    assert(adapted.find(": SV_TARGET") != std::string::npos);
    assert(adapted.find("sampler s0 :") == std::string::npos);
    assert(adapted.find("#define s0") == std::string::npos);
    assert(adapted.find("tex2D(s0") == std::string::npos);
    assert(adapted.find("qdTexture.Sample(qdSampler, uv)") != std::string::npos);
    const std::string namedSampler =
        "sampler2D VideoSampler : register(s0);\n"
        "float4 main(float2 uv:TEXCOORD1):COLOR { return tex2D(VideoSampler, uv); }";
    const std::string namedAdapted = adaptPotPlayerShader(namedSampler);
    assert(namedAdapted.find("sampler2D VideoSampler") == std::string::npos);
    assert(namedAdapted.find("qdTexture.Sample") != std::string::npos);
    const std::string temporalShader =
        "sampler s0:register(s0); float4 p1:register(c1); "
        "float4 pPrev:register(c2); "
        "float4 main(float2 uv:TEXCOORD):SV_TARGET { "
        "return tex2D(s0,uv) + pPrev * 0; }";
    const std::string temporalAdapted = adaptPotPlayerShader(temporalShader);
    assert(temporalAdapted.find("float4 pPrev:register(c2)") == std::string::npos);
    assert(temporalAdapted.find("float4 pPrev; }") != std::string::npos);
    assert(temporalAdapted.find("uv: TEXCOORD0") != std::string::npos);
    assert(temporalAdapted.find("tex2D(s0") == std::string::npos);

    const auto fitted = fitInside({0, 0, 1000, 1000}, 1920, 1080);
    assert(std::abs(fitted.width - 1000.0F) < 0.01F);
    assert(fitted.height < 1000.0F);

    // The picture subtitles are placed on: fitted with bars for Fit,
    // running past the cell for Fill, the cell for Stretch, grown by zoom.
    {
        const RectF cell{100, 50, 800, 600};
        const auto near = [](float a, float b) { return std::abs(a - b) < 0.01F; };
        const RectF fit = pictureRect(cell, 1920, 1080, ViewMode::Fit, 1.0F);
        assert(near(fit.x, 100) && near(fit.width, 800) && near(fit.height, 450) && near(fit.y, 125));
        const RectF fill = pictureRect(cell, 1920, 1080, ViewMode::Fill, 1.0F);
        assert(near(fill.height, 600) && near(fill.width, 600.0F * 16.0F / 9.0F) &&
               near(fill.x + fill.width * 0.5F, 500) && near(fill.y, 50));
        const RectF tall = pictureRect(cell, 1080, 1920, ViewMode::Fill, 1.0F);
        assert(near(tall.width, 800) && tall.height > 600 && near(tall.y + tall.height * 0.5F, 350));
        const RectF stretch = pictureRect(cell, 1920, 1080, ViewMode::Stretch, 1.0F);
        assert(near(stretch.x, 100) && near(stretch.y, 50) && near(stretch.width, 800) && near(stretch.height, 600));
        const RectF zoomed = pictureRect(cell, 1920, 1080, ViewMode::Fit, 2.0F);
        assert(near(zoomed.width, 1600) && near(zoomed.height, 900) && near(zoomed.x, -300) && near(zoomed.y, -100));
        const RectF unknown = pictureRect(cell, 0, 0, ViewMode::Fit, 1.0F);
        assert(near(unknown.width, 800) && near(unknown.height, 600));
    }

    const auto solo = quadCells(800, 600, 2);
    assert(solo[2].width == 800 && solo[2].height == 600);
    assert(solo[0].width == 0);

    // --- Walking a folder -------------------------------------------------
    assert(isMediaExtension(L".mp4"));
    assert(isMediaExtension(L".MKV"));   // listings preserve case; matching must not care
    assert(isMediaExtension(L".WebM"));
    assert(!isMediaExtension(L".txt"));
    assert(!isMediaExtension(L".qdeck"));
    assert(!isMediaExtension(L""));
    assert(!isMediaExtension(L"mp4"));   // an extension carries its dot

    // Stepping past either end wraps, so one direction walks the whole folder.
    assert(stepFileIndex(0, 5, 1) == 1);
    assert(stepFileIndex(4, 5, 1) == 0);
    assert(stepFileIndex(0, 5, -1) == 4);
    assert(stepFileIndex(3, 5, -1) == 2);
    // A single file has nowhere to go but itself.
    assert(stepFileIndex(0, 1, 1) == 0);
    assert(stepFileIndex(0, 1, -1) == 0);
    // A file that is not in the listing, or an empty folder, yields nothing.
    assert(stepFileIndex(-1, 5, 1) == -1);
    assert(stepFileIndex(7, 5, 1) == -1);
    assert(stepFileIndex(0, 0, 1) == -1);

    // --- Paused frame drain -----------------------------------------------
    // The regression this guards: the decoder marks the frame whose PTS
    // interval represents the destination, but the paused drain used to stop
    // only on its own timestamp tolerance. A marked destination frame outside
    // that tolerance was consumed and immediately replaced by the next frame,
    // which is not marked -- so a synchronized seek waited for a frame that no
    // longer existed and froze every pane until its 30-second fail-safe.
    {
        // Destination frame 36ms before the target: taken, and stopped on.
        const auto exact = pausedFrameStep(65.833, true, false, 0.0, 65.869);
        assert(exact.accept && exact.lock);
        // The same frame without the mark is taken but does not stop the
        // drain, which is what lets normal playback settle on the target.
        const auto plain = pausedFrameStep(65.833, false, false, 0.0, 65.869);
        assert(plain.accept && !plain.lock);
        // A destination frame is honoured even once something is displayed.
        const auto afterPreview = pausedFrameStep(65.850, true, true, 55.0, 65.869);
        assert(afterPreview.accept && afterPreview.lock);
        // A sparse stream may have no frame interval covering the target. The
        // decoder then marks the first following PTS, which must override the
        // generic 50ms first-frame allowance rather than deadlocking.
        const auto sparseExact = pausedFrameStep(66.100, true, false, 0.0, 65.869);
        assert(sparseExact.accept && sparseExact.lock);
        // Close enough to the target stops the drain with or without the mark.
        assert(pausedFrameStep(65.868, false, false, 0.0, 65.869).lock);
        // A frame beyond the target is not consumed; the drain stops before it.
        const auto ahead = pausedFrameStep(65.900, false, true, 65.850, 65.869);
        assert(!ahead.accept && ahead.lock);
        // Nothing shown yet allows a wider window, so a first frame slightly
        // past the target still primes the pane instead of leaving it blank.
        assert(pausedFrameStep(65.900, false, false, 0.0, 65.869).accept);
        assert(!pausedFrameStep(65.930, false, false, 0.0, 65.869).accept);
        // EOF can promote the same frame already exposed as the one permitted
        // keyframe preview. Its trusted marker replaces that preview so the
        // synchronized barrier can observe exact completion.
        const auto stalled = pausedFrameStep(65.850, true, true, 65.850, 65.869);
        assert(stalled.accept && stalled.lock);
        // The non-advancing guard still applies to ordinary frames.
        const auto malformed = pausedFrameStep(65.850, false, true, 65.850, 65.869);
        assert(!malformed.accept && malformed.lock);
    }

    // --- Decode thread budget ---------------------------------------------
    // The machine is shared between the panes rather than handed to each
    // decoder whole, but at two threads per core: frame threads block on
    // inter-frame dependencies, and a stricter budget measurably starved 4K
    // software decoding.
    assert(decodeThreadBudget(22, false) == 8);
    assert(decodeThreadBudget(16, false) == 6);
    // Small machines still get a workable minimum.
    assert(decodeThreadBudget(4, false) == 4);
    assert(decodeThreadBudget(1, false) == 4);
    assert(decodeThreadBudget(0, false) == 4);
    // Large machines are capped; each thread also costs an in-flight frame.
    assert(decodeThreadBudget(128, false) == 16);
    // Strict Hardware mode decodes on the GPU and never produces a CPU frame.
    assert(decodeThreadBudget(22, true) == 1);
    assert(decodeThreadBudget(128, true) == 1);
    // Fewer simultaneous sources leave more of the machine to each one.
    assert(decodeThreadBudget(22, false, 1) == 16);
    assert(decodeThreadBudget(8, false, 2) == 8);
    assert(decodeThreadBudget(4, false, 4) == 4);

    // --- Intermediate surface sizing --------------------------------------
    // A 4K source filling a 4K pane keeps its own resolution; it used to be
    // reduced to 1080p and magnified back by the sampler.
    const auto native4k = presentationSize(3840, 2160, 3840.0, 2160.0);
    assert(native4k.width == 3840 && native4k.height == 2160);
    // A 4K source in a quarter-sized pane does not need full resolution.
    const auto quartered = presentationSize(3840, 2160, 960.0, 540.0);
    assert(quartered.width < 3840 && quartered.width >= 960);
    // The source aspect is preserved exactly: Fit and Fill derive their
    // geometry from these dimensions rather than from the decoded frame.
    assert(std::abs(static_cast<double>(native4k.width) / native4k.height -
                    static_cast<double>(quartered.width) / quartered.height) < 0.01);
    // An intermediate is never larger than the source; magnification is the
    // sampler's job.
    const auto small = presentationSize(640, 360, 3840.0, 2160.0);
    assert(small.width == 640 && small.height == 360);
    // Zoom raises the resolution the pane can actually resolve.
    assert(presentationSize(3840, 2160, 960.0 * 4, 540.0 * 4).width >
           presentationSize(3840, 2160, 960.0, 540.0).width);
    // Snapping to a few scale steps keeps a window drag from recreating the
    // video processor and its textures on every pixel of movement.
    assert(presentationSize(3840, 2160, 1000.0, 562.0) ==
           presentationSize(3840, 2160, 1040.0, 585.0));
    // Dimensions stay even, and an absurd source is bounded.
    assert(native4k.width % 2 == 0 && native4k.height % 2 == 0);
    const auto huge = presentationSize(7680, 4320, 7680.0, 4320.0);
    assert(huge.width <= 3840 && huge.height <= 2160 && huge.width % 2 == 0);
    // Degenerate inputs never produce a zero-sized or negative surface.
    assert(presentationSize(0, 0, 100.0, 100.0).width == 0);
    const auto noHint = presentationSize(1920, 1080, 0.0, 0.0);
    assert(noHint.width == 1920 && noHint.height == 1080);
    // With Super Resolution the video processor must be the one to reach the
    // pane's pixels, so the intermediate is allowed to magnify: a 1080p
    // source in a 4K pane becomes 4K, a 720p one covers a 1600-wide pane.
    const auto magnified = presentationSize(1920, 1080, 3840.0, 2160.0, 3840, 8, true);
    assert(magnified.width == 3840 && magnified.height == 2160);
    const auto covered = presentationSize(1280, 720, 1600.0, 900.0, 3840, 8, true);
    assert(covered.width >= 1600 && covered.height >= 900 && covered.width <= 1600 + 80);
    // Magnification snaps to sixteenths so a window drag rarely rebuilds it.
    assert(presentationSize(1280, 720, 1610.0, 905.0, 3840, 8, true) ==
           presentationSize(1280, 720, 1640.0, 922.0, 3840, 8, true));
    // It still never exceeds the bound, keeps the aspect, and never shrinks a
    // source that already covers the pane.
    const auto magnifiedHuge = presentationSize(1920, 1080, 7680.0, 4320.0, 3840, 8, true);
    assert(magnifiedHuge.width <= 3840 && magnifiedHuge.height <= 2160);
    assert(std::abs(static_cast<double>(covered.width) / covered.height - 16.0 / 9.0) < 0.01);
    assert(presentationSize(3840, 2160, 960.0, 540.0, 3840, 8, true) == quartered);

    // --- Synchronization rules -------------------------------------------
    // A plain 60-second source that starts with the master timeline.
    PaneTiming plain;
    plain.loaded = plain.ready = true;
    plain.duration = 60.0;

    assert(std::abs(mappedPaneTime(plain, 12.5, SeekMode::Linked) - 12.5) < 0.001);
    assert(paneBeforeEnd(plain, 30.0, SeekMode::Linked));
    assert(!paneBeforeEnd(plain, 60.0, SeekMode::Linked));
    // The end tolerance treats a frame within 30ms of the duration as finished.
    assert(!paneBeforeEnd(plain, 59.99, SeekMode::Linked));
    assert(paneBeforeEnd(plain, 59.9, SeekMode::Linked));

    PaneTiming paused = plain;
    paused.paused = true;
    paused.pausedTime = 7.25;
    assert(std::abs(mappedPaneTime(paused, 40.0, SeekMode::Linked) - 7.25) < 0.001);

    // Paused panes intentionally ignore loop policy. Resuming after a loop was
    // enabled therefore has to recompute the target after clearing `paused`,
    // rather than seeking the raw held time and correcting one tick later.
    PaneTiming pausedLoop = plain;
    pausedLoop.duration = 120.0;
    pausedLoop.paused = true;
    pausedLoop.pausedTime = 100.0;
    pausedLoop.loopEnabled = true;
    pausedLoop.loopA = 10.0;
    pausedLoop.loopB = 20.0;
    const double heldBeforeResume = mappedPaneTime(
        pausedLoop, 100.0, SeekMode::Linked);
    pausedLoop.paused = false;
    const double mappedAfterResume = mappedPaneTime(
        pausedLoop, 100.0, SeekMode::Linked);
    assert(heldBeforeResume == 100.0 && mappedAfterResume == 10.0);
    assert(timelineMappingChanged(heldBeforeResume, mappedAfterResume));

    // Disabling an A-B loop after several cycles is a forward discontinuity;
    // it needs the same generation-safe remap as a backward wrap.
    PaneTiming activeLoop = pausedLoop;
    const double beforeLoopDisable = mappedPaneTime(
        activeLoop, 100.0, SeekMode::Linked);
    activeLoop.loopEnabled = false;
    const double afterLoopDisable = mappedPaneTime(
        activeLoop, 100.0, SeekMode::Linked);
    assert(beforeLoopDisable == 10.0 && afterLoopDisable == 100.0);
    assert(timelineMappingChanged(beforeLoopDisable, afterLoopDisable));

    // Auto repeat wraps the whole source, but only in Independent mode.
    PaneTiming repeating = plain;
    repeating.autoRepeat = true;
    assert(std::abs(mappedPaneTime(repeating, 75.0, SeekMode::Independent) - 15.0) < 0.001);
    assert(std::abs(mappedPaneTime(repeating, 75.0, SeekMode::Linked) - 75.0) < 0.001);
    PaneTiming repeatingOpening = repeating;
    repeatingOpening.ready = false;
    repeatingOpening.duration = 0.0;
    const double provisionalRepeatTarget = mappedPaneTime(
        repeatingOpening, 75.0, SeekMode::Independent);
    const double settledRepeatTarget = mappedPaneTime(
        repeating, 75.0, SeekMode::Independent);
    assert(provisionalRepeatTarget == 75.0);
    assert(settledRepeatTarget == 15.0);
    assert(timelineMappingChanged(provisionalRepeatTarget, settledRepeatTarget));
    // Linked mode and a paused pane do not acquire a duration-dependent remap
    // when metadata arrives, so their one-shot alignment can be cleared.
    assert(!timelineMappingChanged(
        mappedPaneTime(repeatingOpening, 20.0, SeekMode::Linked),
        mappedPaneTime(repeating, 20.0, SeekMode::Linked)));
    PaneTiming pausedOpening = paused;
    pausedOpening.ready = false;
    pausedOpening.duration = 0.0;
    assert(!timelineMappingChanged(
        mappedPaneTime(pausedOpening, 75.0, SeekMode::Independent),
        mappedPaneTime(paused, 75.0, SeekMode::Independent)));
    assert(readyAlignmentAction(
               true, true, false, false, false, 75.0, 75.0) ==
           ReadyAlignmentAction::Wait);
    assert(readyAlignmentAction(
               false, true, true, false, false, 75.0, 15.0) ==
           ReadyAlignmentAction::None);
    assert(readyAlignmentAction(
               true, false, false, false, false, 75.0, 15.0) ==
           ReadyAlignmentAction::Clear);
    assert(readyAlignmentAction(
               true, true, true, false, false, 75.0, 75.0) ==
           ReadyAlignmentAction::Clear);
    assert(readyAlignmentAction(
               true, true, true, false, false, 75.0, 15.0) ==
           ReadyAlignmentAction::SeekPane);
    assert(readyAlignmentAction(
               true, true, true, false, true, 75.0, 15.0) ==
           ReadyAlignmentAction::RefreshBarrier);
    assert(readyAlignmentAction(
               true, true, false, true, true, 75.0, 15.0) ==
           ReadyAlignmentAction::Clear);
    PaneTiming offsetRepeatOpening = repeatingOpening;
    offsetRepeatOpening.startDelay = 10.0;
    offsetRepeatOpening.adjustment = 5.0;
    offsetRepeatOpening.rate = 2.0;
    PaneTiming offsetRepeatReady = offsetRepeatOpening;
    offsetRepeatReady.ready = true;
    offsetRepeatReady.duration = 60.0;
    const double offsetProvisional = mappedPaneTime(
        offsetRepeatOpening, 50.0, SeekMode::Independent);
    const double offsetSettled = mappedPaneTime(
        offsetRepeatReady, 50.0, SeekMode::Independent);
    assert(offsetProvisional == 85.0 && offsetSettled == 25.0);
    assert(readyAlignmentAction(
               true, true, true, false, false,
               offsetProvisional, offsetSettled) ==
           ReadyAlignmentAction::SeekPane);
    assert(paneRepeats(repeating, SeekMode::Independent));
    assert(!paneRepeats(repeating, SeekMode::Linked));
    // A repeating source never counts as finished.
    assert(paneBeforeEnd(repeating, 600.0, SeekMode::Independent));
    // A source of unknown duration is assumed to still have content.
    PaneTiming unknown = plain;
    unknown.duration = 0.0;
    assert(paneBeforeEnd(unknown, 1e6, SeekMode::Linked));

    assert(shouldOutputPaneAudio(plain, true, 10.0, true, SeekMode::Linked));
    assert(!shouldOutputPaneAudio(plain, false, 10.0, true, SeekMode::Linked));
    assert(!shouldOutputPaneAudio(plain, true, 10.0, false, SeekMode::Linked));
    assert(!shouldOutputPaneAudio(paused, true, 10.0, true, SeekMode::Linked));
    assert(!shouldOutputPaneAudio(plain, true, 60.0, true, SeekMode::Linked));
    // A source whose offset has not been reached yet produces no audio.
    PaneTiming delayed = plain;
    delayed.startDelay = 20.0;
    assert(!shouldOutputPaneAudio(delayed, true, 10.0, true, SeekMode::Linked));
    assert(shouldOutputPaneAudio(delayed, true, 25.0, true, SeekMode::Linked));
    PaneTiming unopened;
    assert(!shouldOutputPaneAudio(unopened, true, 10.0, true, SeekMode::Linked));
    PaneTiming opening = plain;
    opening.ready = false;
    assert(!shouldOutputPaneAudio(opening, true, 10.0, true, SeekMode::Linked));
    opening.ready = true;
    assert(shouldOutputPaneAudio(opening, true, 10.0, true, SeekMode::Linked));

    // --- Audio clock tracking ---------------------------------------------
    // Audio ahead of the clock speeds the clock up, and behind slows it down.
    assert(audioDriftSlew(10.100, 10.0).factor > 1.0);
    assert(audioDriftSlew(9.900, 10.0).factor < 1.0);
    // Agreement leaves the clock alone.
    assert(audioDriftSlew(10.0, 10.0).factor == 1.0);
    assert(audioDriftSlew(10.0, 10.0).corrected);
    // The correction stays far below anything visible.
    assert(audioDriftSlew(10.4, 10.0).factor <= 1.0 + kMaximumClockSlew);
    assert(audioDriftSlew(9.6, 10.0).factor >= 1.0 - kMaximumClockSlew);
    // A discontinuity is a seek or a loop wrap, not drift: leave it to the
    // seek machinery rather than yanking the picture.
    assert(!audioDriftSlew(30.0, 10.0).corrected);
    assert(audioDriftSlew(30.0, 10.0).factor == 1.0);
    assert(!audioDriftSlew(10.0, 10.0, 1.0, kClockDriftGain, kMaximumClockSlew, -1.0).corrected);
    // Garbage in leaves the clock untouched.
    assert(!audioDriftSlew(-1.0, 10.0).corrected);
    assert(!audioDriftSlew(std::nan(""), 10.0).corrected);
    // A pane playing at 2x covers source seconds twice as fast, so the same
    // source-time error is half as large a master-clock error.
    assert(audioDriftSlew(10.1, 10.0, 2.0).factor < audioDriftSlew(10.1, 10.0, 1.0).factor);
    assert(audioDriftSlew(10.1, 10.0, 2.0).factor > 1.0);

    // The loop must converge, not merely point the right way. Simulate a
    // device running 50ppm fast against a clock that starts 200ms behind.
    {
        const double deviceRate = 1.0 + 0.00005;
        const double step = 0.05;
        double clockTime = 0.0;
        double deviceTime = 0.200;
        double slew = 1.0;
        for (int iteration = 0; iteration < 4000; ++iteration) {  // ~200 seconds
            clockTime += step * slew;
            deviceTime += step * deviceRate;
            slew = audioDriftSlew(deviceTime, clockTime).factor;
        }
        // The standing offset is absorbed and the rate mismatch is tracked.
        assert(std::abs(deviceTime - clockTime) < 0.005);
        // Steady state holds a small slew against the device's faster crystal.
        assert(slew > 1.0 && slew < 1.0 + kMaximumClockSlew);
    }

    // --- Audio handoff ----------------------------------------------------
    // V1 is selected and ends at 30s; V2 is longer and unselected, so the audio
    // bit moves to it rather than falling silent.
    PaneTimings deck{};
    deck[0] = plain;
    deck[0].duration = 30.0;
    deck[1] = plain;
    deck[1].duration = 120.0;
    assert(!audioHandoff(deck, 0b0001, 20.0, SeekMode::Linked).valid());

    // Audio may end before its video's timeline. A terminal state from the
    // selected pane's current audio generation triggers the same cyclic
    // handoff without waiting for the video to end.
    PaneSeekAudioStates audioStates{};
    audioStates[0] = SeekAudioState::Ended;
    const auto earlyAudioEnd = audioHandoff(
        deck, 0b0001, 20.0, SeekMode::Linked, audioStates);
    assert(earlyAudioEnd.valid() && earlyAudioEnd.from == 0 && earlyAudioEnd.to == 1);
    audioStates[0] = SeekAudioState::Error;
    const auto earlyAudioError = audioHandoff(
        deck, 0b0001, 20.0, SeekMode::Linked, audioStates);
    assert(earlyAudioError.valid() && earlyAudioError.from == 0 && earlyAudioError.to == 1);
    audioStates[0] = SeekAudioState::Primed;
    assert(!audioHandoff(deck, 0b0001, 20.0, SeekMode::Linked, audioStates).valid());

    // A terminal audio stream does not weaken destination eligibility.
    audioStates[0] = SeekAudioState::Ended;
    PaneTimings earlyPausedTarget = deck;
    earlyPausedTarget[1].paused = true;
    assert(!audioHandoff(
        earlyPausedTarget, 0b0001, 20.0, SeekMode::Linked, audioStates).valid());
    assert(!audioHandoff(
        deck, 0b0011, 20.0, SeekMode::Linked, audioStates).valid());
    PaneTimings earlyFinishedTarget = deck;
    earlyFinishedTarget[1].duration = 10.0;
    assert(!audioHandoff(
        earlyFinishedTarget, 0b0001, 20.0, SeekMode::Linked, audioStates).valid());
    PaneTimings earlyDelayedTarget = deck;
    earlyDelayedTarget[1].startDelay = 40.0;
    assert(!audioHandoff(
        earlyDelayedTarget, 0b0001, 20.0, SeekMode::Linked, audioStates).valid());
    audioStates[1] = SeekAudioState::Error;
    assert(!audioHandoff(
        deck, 0b0001, 20.0, SeekMode::Linked, audioStates).valid());
    audioStates[1] = SeekAudioState::Unknown;

    // Natural audio EOF in a repeating pane waits for the wrap seek, whose new
    // generation restarts that pane's audio. An errored generation may still
    // hand off after its already-queued tail drains.
    PaneTimings earlyRepeating = deck;
    earlyRepeating[0].autoRepeat = true;
    assert(!audioHandoff(
        earlyRepeating, 0b0001, 20.0, SeekMode::Independent, audioStates).valid());
    audioStates[0] = SeekAudioState::Error;
    assert(audioHandoff(
        earlyRepeating, 0b0001, 20.0, SeekMode::Independent, audioStates).valid());
    audioStates[0] = SeekAudioState::Unknown;
    PaneTimings loopRepeating = deck;
    loopRepeating[0].loopEnabled = true;
    loopRepeating[0].loopA = 5.0;
    loopRepeating[0].loopB = 25.0;
    audioStates[0] = SeekAudioState::Ended;
    assert(!audioHandoff(
        loopRepeating, 0b0001, 20.0, SeekMode::Linked, audioStates).valid());
    audioStates[0] = SeekAudioState::Error;
    assert(audioHandoff(
        loopRepeating, 0b0001, 20.0, SeekMode::Linked, audioStates).valid());
    audioStates[0] = SeekAudioState::Unknown;

    const auto handoff = audioHandoff(deck, 0b0001, 30.0, SeekMode::Linked);
    assert(handoff.valid() && handoff.from == 0 && handoff.to == 1);
    // Nothing to hand off to once the replacement has also finished.
    assert(!audioHandoff(deck, 0b0001, 120.0, SeekMode::Linked).valid());
    // An already-selected pane is never chosen as the replacement.
    assert(!audioHandoff(deck, 0b0011, 30.0, SeekMode::Linked).valid());
    // A repeating source never finishes, so it never hands its audio away.
    PaneTimings looping = deck;
    looping[0].autoRepeat = true;
    assert(!audioHandoff(looping, 0b0001, 30.0, SeekMode::Independent).valid());
    // A paused pane is not a valid destination.
    PaneTimings pausedTarget = deck;
    pausedTarget[1].paused = true;
    assert(!audioHandoff(pausedTarget, 0b0001, 30.0, SeekMode::Linked).valid());
    // Search order is cyclic from the finished pane: V4 wins over V2 when the
    // finished pane is V3.
    PaneTimings cyclic{};
    cyclic[1] = plain;
    cyclic[1].duration = 120.0;
    cyclic[2] = plain;
    cyclic[2].duration = 30.0;
    cyclic[3] = plain;
    cyclic[3].duration = 120.0;
    const auto wrapped = audioHandoff(cyclic, 0b0100, 30.0, SeekMode::Linked);
    assert(wrapped.valid() && wrapped.from == 2 && wrapped.to == 3);
    cyclic[3].duration = 30.0;
    cyclic[4] = plain;
    cyclic[4].duration = 120.0;
    const auto fifth = audioHandoff(cyclic, 0b1000, 30.0, SeekMode::Linked);
    assert(fifth.valid() && fifth.from == 3 && fifth.to == 4);

    // --- Seek barrier -----------------------------------------------------
    SeekBarrier barrier;
    PaneArray<SeekBarrier::PaneStatus> status{};
    status[0].loaded = status[1].loaded = true;

    assert(!barrier.active());
    assert(!barrier.ready(0, status));  // an inactive barrier is never ready

    // Rebuilding a barrier when an opening repeat source learns its duration
    // changes both target and generation. A late exact frame from the
    // provisional raw target cannot release the replacement barrier.
    PaneTimings openingRepeatDeck{};
    openingRepeatDeck[0] = repeatingOpening;
    SeekBarrier openingBarrier;
    assert(openingBarrier.begin(
        75.0, true, openingRepeatDeck, 0, SeekMode::Independent));
    assert(openingBarrier.paneTarget(0) == 75.0);
    openingBarrier.setGeneration(0, 41);
    PaneTimings settledRepeatDeck{};
    settledRepeatDeck[0] = repeating;
    assert(openingBarrier.begin(
        75.0, true, settledRepeatDeck, 0, SeekMode::Independent));
    assert(openingBarrier.paneTarget(0) == 15.0);
    openingBarrier.setGeneration(0, 42);
    PaneArray<SeekBarrier::PaneStatus> openingStatus{};
    openingStatus[0].loaded = true;
    openingBarrier.observeFrame(0, true, 41, true);
    assert(!openingBarrier.ready(0, openingStatus));
    openingBarrier.observeFrame(0, true, 42, true);
    assert(openingBarrier.ready(0, openingStatus));

    assert(barrier.begin(25.0, true, deck, 0b0001, SeekMode::Linked));
    assert(barrier.active() && barrier.resume());
    assert(std::abs(barrier.target() - 25.0) < 0.001);
    assert(barrier.waiting(0) && barrier.waiting(1));
    assert(!barrier.waiting(2));
    // Only the selected pane has to produce audio before the barrier lifts.
    assert(barrier.audioWaiting(0) && !barrier.audioWaiting(1));
    assert(std::abs(barrier.paneTarget(0) - 25.0) < 0.001);
    barrier.setGeneration(0, 7);
    barrier.setGeneration(1, 7);
    barrier.setAudioGeneration(0, 17);

    assert(!barrier.ready(0, status));
    // A keyframe preview is a frame, but not an exact frame: it must not
    // release the barrier.
    auto event = barrier.observeFrame(0, true, 7, false);
    assert(event.firstFrame && !event.exactFrame);
    assert(!barrier.ready(0, status));
    // Neither may a frame decoded for a superseded seek generation.
    assert(!barrier.observeFrame(0, true, 6, true).exactFrame);
    assert(!barrier.ready(0, status));
    // The exact frame for the recorded generation does, and is logged once.
    event = barrier.observeFrame(0, true, 7, true);
    assert(!event.firstFrame && event.exactFrame);
    assert(!barrier.observeFrame(0, true, 7, true).exactFrame);
    assert(!barrier.ready(0, status));  // pane 1 has not arrived yet
    barrier.observeFrame(1, true, 7, true);
    // Pane 0 still owes audio for this seek. Knowing which generation is in
    // flight is not enough: a destination buffer must reach the sink.
    status[0].audioGeneration = 17;
    status[0].audioState = SeekBarrier::AudioState::Unknown;
    assert(!barrier.ready(0, status));
    status[0].audioState = SeekBarrier::AudioState::Primed;
    assert(barrier.ready(0, status));

    // Audio can legitimately finish before the video. Once the audio worker
    // reaches EOF for this generation, the short track has nothing to prime
    // and must not leave the whole deck parked for 30 seconds. A source with
    // no audio stream reports the same terminal state.
    SeekBarrier shortAudio;
    PaneTimings onePane{};
    onePane[0] = plain;
    assert(shortAudio.begin(50.0, true, onePane, 0b0001, SeekMode::Linked));
    shortAudio.setGeneration(0, 8);
    shortAudio.setAudioGeneration(0, 18);
    PaneArray<SeekBarrier::PaneStatus> shortStatus{};
    shortStatus[0].loaded = true;
    shortStatus[0].audioGeneration = 18;
    shortStatus[0].audioState = SeekBarrier::AudioState::Ended;
    assert(!shortAudio.ready(0, shortStatus));  // video is still required
    shortAudio.observeFrame(0, true, 8, true);
    assert(shortAudio.ready(0, shortStatus));

    // A failed seek/decode is terminal for the audio part of the barrier too.
    // Video still has to arrive, but audio failure cannot manufacture audio by
    // waiting until the global fail-safe.
    SeekBarrier brokenAudio;
    assert(brokenAudio.begin(25.0, true, onePane, 0b0001, SeekMode::Linked));
    brokenAudio.setGeneration(0, 9);
    brokenAudio.setAudioGeneration(0, 19);
    brokenAudio.observeFrame(0, true, 9, true);
    auto brokenAudioStatus = shortStatus;
    brokenAudioStatus[0].audioGeneration = 19;
    brokenAudioStatus[0].audioState = SeekBarrier::AudioState::Error;
    assert(brokenAudio.ready(0, brokenAudioStatus));

    // A late result from a superseded audio seek is not evidence about the new
    // destination, even if that old seek was primed or reached a terminal
    // state. Only a matching generation may release the current barrier.
    SeekBarrier supersededAudio;
    assert(supersededAudio.begin(25.0, true, onePane, 0b0001, SeekMode::Linked));
    supersededAudio.setGeneration(0, 10);
    supersededAudio.setAudioGeneration(0, 20);
    supersededAudio.observeFrame(0, true, 10, true);
    auto supersededStatus = shortStatus;
    supersededStatus[0].audioGeneration = 19;
    for (const auto oldState : {SeekBarrier::AudioState::Primed,
                                SeekBarrier::AudioState::Ended,
                                SeekBarrier::AudioState::Error}) {
        supersededStatus[0].audioState = oldState;
        assert(!supersededAudio.ready(0, supersededStatus));
    }
    supersededStatus[0].audioGeneration = 20;
    supersededStatus[0].audioState = SeekBarrier::AudioState::Unknown;
    assert(!supersededAudio.ready(0, supersededStatus));
    supersededStatus[0].audioState = SeekBarrier::AudioState::Primed;
    assert(supersededAudio.ready(0, supersededStatus));

    // Pausing while a barrier is decoding keeps the exact-video requirement
    // but cancels audio priming. Playing again installs a fresh audio
    // generation, so the pre-pause observation cannot release the barrier.
    SeekBarrier toggledBarrier;
    assert(toggledBarrier.begin(25.0, true, onePane, 0b0001, SeekMode::Linked));
    toggledBarrier.setGeneration(0, 11);
    toggledBarrier.setAudioGeneration(0, 21);
    toggledBarrier.observeFrame(0, true, 11, true);
    auto toggledStatus = shortStatus;
    toggledStatus[0].audioGeneration = 21;
    toggledStatus[0].audioState = SeekBarrier::AudioState::Unknown;
    assert(!toggledBarrier.ready(0, toggledStatus));
    toggledBarrier.setResume(false);
    toggledBarrier.setAudioWaiting(0, false);
    assert(toggledBarrier.ready(0, toggledStatus));
    toggledBarrier.setResume(true);
    toggledBarrier.setAudioGeneration(0, 22);
    toggledBarrier.setAudioWaiting(0, true);
    toggledStatus[0].audioState = SeekBarrier::AudioState::Primed;
    assert(!toggledBarrier.ready(0, toggledStatus));
    toggledStatus[0].audioGeneration = 22;
    assert(toggledBarrier.ready(0, toggledStatus));

    // A source that fails cannot contribute a frame and must not hold the rest
    // of the deck hostage.
    SeekBarrier stalled;
    stalled.begin(25.0, false, deck, 0, SeekMode::Linked);
    stalled.setGeneration(0, 1);
    stalled.setGeneration(1, 1);
    stalled.observeFrame(0, true, 1, true);
    assert(!stalled.ready(0, status));
    auto brokenStatus = status;
    brokenStatus[1].errored = true;
    assert(stalled.ready(0, brokenStatus));
    // The fail-safe releases a barrier that never completes.
    assert(!stalled.timedOut());
    assert(stalled.ready(SeekBarrier::kTimeoutMs, status));
    stalled.noteElapsed(SeekBarrier::kTimeoutMs);
    assert(stalled.timedOut());
    stalled.reset();
    assert(!stalled.active() && !stalled.timedOut() && !stalled.waiting(0));

    // Seeking past the end of every source leaves nobody to wait for, so the
    // caller completes the barrier immediately instead of stalling playback.
    SeekBarrier finished;
    assert(!finished.begin(300.0, true, deck, 0b0001, SeekMode::Linked));
    // A paused pane is not a participant either.
    PaneTimings allPaused{};
    allPaused[0] = plain;
    allPaused[0].paused = true;
    assert(!SeekBarrier{}.begin(10.0, true, allPaused, 0b0001, SeekMode::Linked));
    // Neither is a pane whose negative offset keeps it inactive.
    PaneTimings waiting{};
    waiting[0] = plain;
    waiting[0].startDelay = 40.0;
    assert(!SeekBarrier{}.begin(10.0, true, waiting, 0b0001, SeekMode::Linked));
    assert(SeekBarrier{}.begin(45.0, true, waiting, 0b0001, SeekMode::Linked));

    // A paused barrier never marks a pane as owing audio.
    SeekBarrier quiet;
    quiet.begin(25.0, false, deck, 0b0011, SeekMode::Linked);
    assert(quiet.waiting(0) && !quiet.audioWaiting(0) && !quiet.audioWaiting(1));

    PlaybackClock clock;
    clock.seek(3.0);
    assert(std::abs(clock.position() - 3.0) < 0.02);
    clock.play();
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    clock.pause();
    assert(clock.position() >= 3.015);
    const auto generation = clock.generation();
    clock.seek(1.0);
    assert(clock.generation() == generation + 1);

    std::cout << "QuadDeck core tests passed\n";
    return 0;
}
