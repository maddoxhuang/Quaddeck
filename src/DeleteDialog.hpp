#pragma once

// The window that asks to confirm deleting a video from the Emby server,
// laid out: a caption strip with what it asks and a square close, the
// item's title with its type, where it plays, the files on a box of their
// own, the warning, and Cancel and Delete at the bottom right. As small as
// a message box: the type sizes are the menus' and the notices', the
// buttons a menu line's height. Pure functions on floats, in pixels, every
// 96-DPI metric multiplied by `scale` here once, with no window, device or
// font needed. AppEmbyDeleteWindow.cpp lays
// the window out and hits it with them; Overlay::drawDeleteDialog paints
// the same rectangles.

#include "OverlayLayout.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace quaddeck {

// What the window says.
struct DeleteDialogContent {
    // In the caption strip: the question.
    std::wstring caption;
    std::wstring title;
    // Movie, Episode or Video, after the title.
    std::wstring type;
    // Which pane it plays in and that it closes; empty when it plays nowhere.
    std::wstring playing;
    // The files the server names, one a line, each wrapped where it is long.
    std::vector<std::wstring> files;
    std::wstring warning;
    std::wstring cancel{L"Cancel"};
    std::wstring confirm{L"Delete"};
};

struct DeleteDialogMetrics {
    float width{380.0F};
    // Between the window's sides and what it says.
    float padX{20.0F};
    // Under the caption strip, before the title.
    float bodyTop{6.0F};
    // A body line -- the title -- and a small one: a file's, the warning's.
    float titleLine{20.0F};
    float smallLine{18.0F};
    float typeGap{8.0F};
    float playingGap{4.0F};
    float listGap{10.0F};
    float listPadX{10.0F};
    float listPadY{7.0F};
    float listRadius{6.0F};
    // Kept clear at the list's right for its thumb, whether or not it shows,
    // so a path wraps the same either way.
    float listThumbGutter{8.0F};
    // Lines the list keeps when the window is too short for all of it.
    float listLeastLines{2.0F};
    float warningGap{8.0F};
    float footerGap{14.0F};
    float footerBottom{16.0F};
    float footerRight{16.0F};
    // A menu line's height (PanelMenuMetrics::compact).
    float button{28.0F};
    float buttonGap{8.0F};
    float buttonPad{16.0F};
    float buttonMinimum{72.0F};
    float thumbMinimum{20.0F};
};

// What the text measures, in pixels, from the fonts it is drawn in
// (Overlay::measureDeleteDialog); 0, or a file's height left out, for an
// estimate where no font is ready, as on a lost device.
struct DeleteDialogMeasures {
    // The title on one line, and wrapped to the inner width.
    float titleWidth{};
    float titleHeight{};
    float typeWidth{};
    std::vector<float> fileHeights;
    float warningHeight{};
    float cancelWidth{};
    float confirmWidth{};
};

enum class DeleteDialogHit { None, Caption, Close, List, Scrollbar, Cancel, Confirm };

struct DeleteDialogLayout {
    OverlayRect caption;
    OverlayRect captionTitle;
    OverlayRect close;
    OverlayRect title;
    // Beside the title when both fit on one line, else under it.
    OverlayRect type;
    bool titleWrapped{};
    OverlayRect playing;
    // The box the files stand on, and inside its padding, where they are
    // drawn and clipped to.
    OverlayRect list;
    OverlayRect listView;
    // Each file's lines, scrolled by `listScroll`.
    std::vector<OverlayRect> files;
    float listContentHeight{};
    float listScroll{};
    float listMaxScroll{};
    // The strip at the list's right the thumb runs in; empty when the list
    // shows every file.
    OverlayRect scrollbar;
    OverlayRect warning;
    OverlayRect cancel;
    OverlayRect confirm;
    // The height the window asks: all it says, or as much as `height` holds
    // with the list cut short.
    float wanted{};
};

// Where the text wraps: inside the window's padding, and on the list,
// inside its own and clear of its thumb.
inline float deleteDialogInnerWidth(float width, float scale, const DeleteDialogMetrics& m = {}) {
    return std::max(0.0F, width - 2.0F * m.padX * scale);
}

inline float deleteDialogFileWidth(float width, float scale, const DeleteDialogMetrics& m = {}) {
    return std::max(0.0F, deleteDialogInnerWidth(width, scale, m) - 2.0F * m.listPadX * scale -
                              m.listThumbGutter * scale);
}

// A width for text no font measured: Segoe UI's average, wide characters
// as wide as they are tall.
inline float deleteDialogTextEstimate(const std::wstring& text, float em) {
    float width = 0.0F;
    for (const wchar_t c : text) width += (c >= 0x2E80 ? 1.0F : 0.56F) * em;
    return width;
}

// The lines `text` takes wrapped at `width`, each `line` tall, estimated.
inline float deleteDialogWrappedEstimate(const std::wstring& text, float em, float width, float line) {
    if (text.empty() || width <= 0.0F) return 0.0F;
    const float lines = std::max(1.0F, std::ceil(deleteDialogTextEstimate(text, em) / width));
    return lines * line;
}

// Laid out in a window `width` by `height` pixels, the list scrolled by
// `listScroll` (held to its range). The parts stand from the top; the
// buttons stand at the bottom, so a window taller than it asks -- a monitor
// of another scale -- leaves its room above them.
inline DeleteDialogLayout deleteDialogLayout(const DeleteDialogContent& content, const DeleteDialogMeasures& measures,
                                             float width, float height, float scale, float listScroll,
                                             const DeleteDialogMetrics& m = {}) {
    DeleteDialogLayout layout;
    const float s = std::max(0.0F, scale);
    if (!(width > 0.0F) || !(height > 0.0F) || !(s > 0.0F)) return layout;
    const CaptionMetrics captionMetrics{};
    const float captionHeight = std::min(captionMetrics.rowHeight * s, height);
    layout.caption = {0.0F, 0.0F, width, captionHeight};
    const float closeWidth = std::min(captionMetrics.button * s, width);
    layout.close = {width - closeWidth, 0.0F, closeWidth, captionHeight};
    const float titleInset = captionMetrics.titleInset * s;
    layout.captionTitle = {titleInset, 0.0F, std::max(0.0F, layout.close.x - 2.0F * titleInset), captionHeight};

    const float left = m.padX * s;
    const float inner = deleteDialogInnerWidth(width, s, m);
    const float titleLine = m.titleLine * s;
    const float smallLine = m.smallLine * s;
    float y = captionHeight + m.bodyTop * s;

    // The title, its type beside it where both fit on a line.
    const float titleWidth = measures.titleWidth > 0.0F ? measures.titleWidth
                                                        : deleteDialogTextEstimate(content.title, 14.0F * s);
    const float typeWidth = content.type.empty()        ? 0.0F
                            : measures.typeWidth > 0.0F ? measures.typeWidth
                                                        : deleteDialogTextEstimate(content.type, 12.0F * s);
    const float typeGap = typeWidth > 0.0F ? m.typeGap * s : 0.0F;
    if (titleWidth + typeGap + typeWidth <= inner) {
        layout.title = {left, y, std::ceil(titleWidth), titleLine};
        if (typeWidth > 0.0F) layout.type = {layout.title.right() + typeGap, y, std::ceil(typeWidth), titleLine};
        y += titleLine;
    } else {
        layout.titleWrapped = true;
        const float titleHeight = measures.titleHeight > 0.0F
            ? measures.titleHeight
            : deleteDialogWrappedEstimate(content.title, 14.0F * s, inner, titleLine);
        layout.title = {left, y, inner, std::max(titleLine, titleHeight)};
        y = layout.title.bottom();
        if (typeWidth > 0.0F) {
            layout.type = {left, y, inner, smallLine};
            y += smallLine;
        }
    }
    if (!content.playing.empty()) {
        y += m.playingGap * s;
        layout.playing = {left, y, inner, smallLine};
        y += smallLine;
    }

    // The files, each as tall as it wraps.
    const float fileWidth = deleteDialogFileWidth(width, s, m);
    std::vector<float> fileHeights(content.files.size());
    float listContent = 0.0F;
    for (std::size_t i = 0; i < content.files.size(); ++i) {
        const float measured = i < measures.fileHeights.size() ? measures.fileHeights[i] : 0.0F;
        fileHeights[i] = std::max(smallLine, measured > 0.0F
                                                 ? measured
                                                 : deleteDialogWrappedEstimate(content.files[i], 12.0F * s,
                                                                               fileWidth, smallLine));
        listContent += fileHeights[i];
    }
    const float listPadY = m.listPadY * s;
    const float listTop = y + m.listGap * s;
    const float warningHeight = content.warning.empty() ? 0.0F
        : std::max(smallLine, measures.warningHeight > 0.0F
                                  ? measures.warningHeight
                                  : deleteDialogWrappedEstimate(content.warning, 12.0F * s, inner, smallLine));
    const float below = (warningHeight > 0.0F ? m.warningGap * s + warningHeight : 0.0F) + m.footerGap * s +
                        m.button * s + m.footerBottom * s;
    // As tall as its files, or as the window leaves it, but never shorter
    // than a couple of lines.
    float listView = listContent;
    const float room = height - listTop - 2.0F * listPadY - below;
    if (listView > room) listView = std::max(std::min(listContent, m.listLeastLines * smallLine), room);
    listView = std::max(0.0F, listView);
    const bool list = !content.files.empty();
    if (list) {
        layout.list = {left, listTop, inner, listView + 2.0F * listPadY};
        layout.listView = {left + m.listPadX * s, listTop + listPadY, fileWidth, listView};
        layout.listContentHeight = listContent;
        layout.listMaxScroll = std::max(0.0F, listContent - listView);
        layout.listScroll = std::clamp(listScroll, 0.0F, layout.listMaxScroll);
        float fileY = layout.listView.y - layout.listScroll;
        layout.files.reserve(content.files.size());
        for (const float fileHeight : fileHeights) {
            layout.files.push_back({layout.listView.x, fileY, fileWidth, fileHeight});
            fileY += fileHeight;
        }
        if (layout.listMaxScroll > 0.0F) {
            const float gutter = m.listThumbGutter * s;
            layout.scrollbar = {layout.list.right() - gutter - 2.0F * s, layout.listView.y, gutter, listView};
        }
        y = layout.list.bottom();
    }
    if (warningHeight > 0.0F) {
        y += (list ? m.warningGap : m.listGap) * s;
        layout.warning = {left, y, inner, warningHeight};
        y = layout.warning.bottom();
    }
    y += m.footerGap * s;
    layout.wanted = std::min(height, y + m.button * s + m.footerBottom * s);

    // Delete outermost at the right, as the action a message box asks about.
    const float buttonHeight = m.button * s;
    const float buttonTop = std::max(y, height - m.footerBottom * s - buttonHeight);
    const auto buttonWidth = [&](float measured, const std::wstring& caption) {
        const float text = measured > 0.0F ? measured : deleteDialogTextEstimate(caption, 13.0F * s);
        return std::ceil(std::max(m.buttonMinimum * s, text + 2.0F * m.buttonPad * s));
    };
    const float confirmWidth = buttonWidth(measures.confirmWidth, content.confirm);
    const float cancelWidth = buttonWidth(measures.cancelWidth, content.cancel);
    layout.confirm = {width - m.footerRight * s - confirmWidth, buttonTop, confirmWidth, buttonHeight};
    layout.cancel = {layout.confirm.x - m.buttonGap * s - cancelWidth, buttonTop, cancelWidth, buttonHeight};
    return layout;
}

inline DeleteDialogHit deleteDialogHitTest(const DeleteDialogLayout& layout, float x, float y) {
    if (layout.close.contains(x, y)) return DeleteDialogHit::Close;
    if (layout.caption.contains(x, y)) return DeleteDialogHit::Caption;
    if (layout.confirm.contains(x, y)) return DeleteDialogHit::Confirm;
    if (layout.cancel.contains(x, y)) return DeleteDialogHit::Cancel;
    if (layout.scrollbar.contains(x, y)) return DeleteDialogHit::Scrollbar;
    if (layout.list.contains(x, y)) return DeleteDialogHit::List;
    return DeleteDialogHit::None;
}

// The list's thumb in its strip: as tall as the share of the files shown,
// at least `thumbMinimum`; empty when every file shows.
inline OverlayRect deleteDialogThumb(const DeleteDialogLayout& layout, float scale,
                                     const DeleteDialogMetrics& m = {}) {
    const OverlayRect& strip = layout.scrollbar;
    if (!strip.visible() || layout.listContentHeight <= 0.0F || layout.listMaxScroll <= 0.0F) return {};
    const float height = std::min(strip.height, std::max(m.thumbMinimum * std::max(0.0F, scale),
                                                         strip.height * strip.height / layout.listContentHeight));
    const float travel = strip.height - height;
    return {strip.x, strip.y + travel * layout.listScroll / layout.listMaxScroll, strip.width, height};
}

// The scroll that puts the thumb's top at `top`.
inline float deleteDialogScrollForThumbTop(const DeleteDialogLayout& layout, float scale, float top,
                                           const DeleteDialogMetrics& m = {}) {
    const OverlayRect thumb = deleteDialogThumb(layout, scale, m);
    const float travel = layout.scrollbar.height - thumb.height;
    if (!thumb.visible() || travel <= 0.0F) return layout.listScroll;
    return std::clamp((top - layout.scrollbar.y) / travel, 0.0F, 1.0F) * layout.listMaxScroll;
}

}  // namespace quaddeck
