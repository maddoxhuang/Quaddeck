#pragma once

// QuadDeck's menus: the sheet's drop-downs -- the Emby browser's View, the
// browsers' Sort, an information page's Play and Add choices, an Emby
// item's right-click -- and the menus over the picture -- its right-click
// (also a pane's pill), the bar's Subtitles and arrangement menus. They are
// drawn by the Overlay, as the rest of the window is, not shown as Windows
// popup menus: a popup menu is a window of its own, and over it the pointer
// had left the player, so a browser the right edge had opened closed under a
// menu being read. The settings window's drop-downs, which nothing closes
// for the pointer leaving, hang their lists in a window of their own, drawn
// by the Overlay too, so a list can reach past the window's edge as a
// Windows combo box's does (panelMenuWindowFrame).
//
// Pure geometry and keyboard navigation on floats, as OverlayLayout.hpp:
// App builds the items and hit-tests the layout, Overlay measures the text
// and draws it. Every 96-DPI metric is multiplied by `scale` here, once.

#include "OverlayLayout.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace quaddeck {

enum class PanelMenuItemKind {
    // One choice among several: the chosen one is ticked. One no other is
    // ticked beside is a command.
    Option,
    // On or off by itself: it wears a switch.
    Check,
    // Segments in a row share one line, the chosen one filled. No menu has
    // them now: the sort's direction is gone, and the browsers' Folders /
    // All videos is a toolbar button of its own.
    Segment,
    Separator,
    // A line that opens a card of its own beside it, its `children`, as a
    // Windows menu's submenu does: the picture's Playback speed, Subtitles.
    Submenu,
};

struct PanelMenuItem {
    PanelMenuItemKind kind{PanelMenuItemKind::Option};
    // What choosing it hands back; 0 for a separator and a submenu.
    unsigned command{};
    std::wstring label;
    // Its key, in a dim cap at the end of its line.
    std::wstring shortcut;
    bool checked{};
    bool enabled{true};
    // A submenu's own items.
    std::vector<PanelMenuItem> children;
};

struct PanelMenu {
    std::vector<PanelMenuItem> items;

    PanelMenu& option(unsigned command, std::wstring label, bool checked = false, std::wstring shortcut = {},
                      bool enabled = true) {
        items.push_back({PanelMenuItemKind::Option, command, std::move(label), std::move(shortcut), checked, enabled});
        return *this;
    }
    PanelMenu& check(unsigned command, std::wstring label, bool on, std::wstring shortcut = {}, bool enabled = true) {
        items.push_back({PanelMenuItemKind::Check, command, std::move(label), std::move(shortcut), on, enabled});
        return *this;
    }
    PanelMenu& segment(unsigned command, std::wstring label, bool chosen, bool enabled = true) {
        items.push_back({PanelMenuItemKind::Segment, command, std::move(label), {}, chosen, enabled});
        return *this;
    }
    // Greyed, it does not open.
    PanelMenu& submenu(std::wstring label, PanelMenu menu, bool enabled = true) {
        items.push_back({PanelMenuItemKind::Submenu, 0, std::move(label), {}, false, enabled, std::move(menu.items)});
        return *this;
    }
    // Between two groups: never first, never two together. One left last
    // takes no room (panelMenuLayout).
    PanelMenu& separator() {
        if (!items.empty() && items.back().kind != PanelMenuItemKind::Separator) {
            items.push_back({PanelMenuItemKind::Separator});
        }
        return *this;
    }
    // The item choosing hands this command back, in a submenu too.
    const PanelMenuItem* find(unsigned command) const { return find(items, command); }
    static const PanelMenuItem* find(const std::vector<PanelMenuItem>& list, unsigned command) {
        for (const auto& item : list) {
            if (item.kind == PanelMenuItemKind::Submenu) {
                if (const auto* inner = find(item.children, command)) return inner;
            } else if (item.kind != PanelMenuItemKind::Separator && item.command == command) {
                return &item;
            }
        }
        return nullptr;
    }
    // The submenu line with this label, at the top level.
    const PanelMenuItem* findSubmenu(const std::wstring& label) const {
        for (const auto& item : items) {
            if (item.kind == PanelMenuItemKind::Submenu && item.label == label) return &item;
        }
        return nullptr;
    }
};

// What the pointer and the keys can reach: a submenu with something in it,
// anything else with a command, and nothing greyed.
inline bool panelMenuSelectable(const PanelMenuItem& item) {
    if (!item.enabled || item.kind == PanelMenuItemKind::Separator) return false;
    return item.kind == PanelMenuItemKind::Submenu ? !item.children.empty() : item.command != 0;
}

struct PanelMenuMetrics {
    // The card's own margin around its lines.
    float pad{6.0F};
    float itemHeight{34.0F};
    float separatorHeight{13.0F};
    // A line of segments: the track inside it, the segments inside that.
    float segmentLine{44.0F};
    float trackHeight{34.0F};
    float trackPad{3.0F};
    float segmentPad{16.0F};
    // Where an option's label starts when some option is ticked, the tick
    // centred in the column before it; otherwise every label starts here.
    float tickColumn{34.0F};
    float inset{12.0F};
    // Between a label and its key, switch or a submenu's chevron.
    float trailingGap{32.0F};
    // A switch as the sheet's and the settings window's (PanelMetrics).
    float switchWidth{40.0F};
    float switchHeight{20.0F};
    float chevronWidth{12.0F};
    float minWidth{176.0F};
    // Between the button and the card, and the card and its bounds' edge.
    float gap{6.0F};
    float margin{8.0F};
    // Between a drop-down's box and its list in a window of its own, which
    // hangs from the box as a Windows combo box's does.
    float windowGap{2.0F};
    // How far a submenu's card lies over the edge of the one it opened from.
    float submenuOverlap{4.0F};
    // A character's width where no font measured it: before DWrite is
    // ready, or after a lost device.
    float fallbackCharacter{7.5F};

    // The menus over the picture are commands rather than a few choices,
    // and many: shorter lines, so the picture's fits a window.
    static PanelMenuMetrics compact() {
        PanelMenuMetrics metrics;
        metrics.pad = 5.0F;
        metrics.itemHeight = 28.0F;
        metrics.separatorHeight = 9.0F;
        return metrics;
    }
};

// Where a card stands against what it hangs from.
enum class PanelMenuPlacement {
    // Under its button, or over it where there is more room: a drop-down.
    Below,
    // Its corner at a point -- the anchor an empty box there -- reaching
    // right and down, or turned left or up where it would not fit that way,
    // as a Windows menu at the pointer turns: a right-click's menu.
    AtPoint,
    // Beside a line of another card -- the anchor that line, widened to its
    // card -- its first line level with it, on the right or, where there is
    // no room there, on the left; fitting neither, under or over that line,
    // never on it: a submenu.
    Beside,
    // Filling its bounds from their top, no taller than its lines: a
    // drop-down's list in a window of its own, the window its card
    // (panelMenuWindowFrame).
    Fill,
};

struct PanelMenuLayout {
    OverlayRect card;
    // The band of the card its lines show through: all of it but half its
    // pad top and bottom. Lines scrolled past it are clipped and not hit.
    OverlayRect view;
    // One per item: an option's, a switch's or a submenu's line, a segment's
    // own pill, a separator's band; empty for a separator that takes no
    // room.
    std::vector<OverlayRect> items;
    // Behind each run of segments, the track they sit in.
    std::vector<OverlayRect> tracks;
    // From a line's left to its label: an option's and a submenu's past the
    // tick column when any option is ticked, a switch's always at the inset.
    float optionLabelInset{};
    float checkLabelInset{};
    // How far its lines are scrolled up, and how far they can be: above 0
    // only for a menu taller than its bounds, whose card is cut to them.
    float scroll{};
    float maxScroll{};
    // Opened above its button or point: there was more room there.
    bool above{};
    // Turned to the left of its point or of the card it opened from.
    bool left{};
};

// A menu hanging from `anchor` and kept inside `bounds` (the sheet, or the
// window under its caption), placed as `placement` says: a drop-down's left
// edge at its button's and moved in from the right; a right-click's corner
// at the point. As wide as its widest line, a drop-down's button, and the
// minimum; no taller than the bounds, its lines scrolled by `scroll` (kept
// in range) where it would be. A Windows menu could leave the window; this
// one cannot, so a short window must not hide its last lines. labelWidths
// and shortcutWidths are the text widths in pixels by item, 0 or missing
// where unknown.
inline PanelMenuLayout panelMenuLayout(const std::vector<PanelMenuItem>& items,
                                       const std::vector<float>& labelWidths,
                                       const std::vector<float>& shortcutWidths,
                                       const OverlayRect& anchor, const OverlayRect& bounds, float scale,
                                       float scroll = 0.0F,
                                       PanelMenuPlacement placement = PanelMenuPlacement::Below,
                                       const PanelMenuMetrics& metrics = {}) {
    PanelMenuLayout layout;
    const float s = scale > 0.0F ? scale : 1.0F;
    const auto widthOf = [&](const std::vector<float>& widths, std::size_t index, const std::wstring& text) {
        const float measured = index < widths.size() ? widths[index] : 0.0F;
        return measured > 0.0F ? measured : static_cast<float>(text.size()) * metrics.fallbackCharacter * s;
    };
    const auto separatorShown = [&](std::size_t index) { return index > 0 && index + 1 < items.size(); };
    bool ticks = false;
    for (const auto& item : items) ticks = ticks || (item.kind == PanelMenuItemKind::Option && item.checked);
    layout.optionLabelInset = (ticks ? metrics.tickColumn : metrics.inset) * s;
    layout.checkLabelInset = metrics.inset * s;

    float lineWidth = 0.0F;
    float height = 2.0F * metrics.pad * s;
    for (std::size_t i = 0; i < items.size();) {
        const auto& item = items[i];
        if (item.kind == PanelMenuItemKind::Segment) {
            std::size_t end = i;
            float widest = 0.0F;
            for (; end < items.size() && items[end].kind == PanelMenuItemKind::Segment; ++end) {
                widest = std::max(widest, widthOf(labelWidths, end, items[end].label));
            }
            const float count = static_cast<float>(end - i);
            lineWidth = std::max(lineWidth, count * (widest + 2.0F * metrics.segmentPad * s) +
                                                2.0F * metrics.trackPad * s);
            height += metrics.segmentLine * s;
            i = end;
            continue;
        }
        if (item.kind == PanelMenuItemKind::Separator) {
            if (separatorShown(i)) height += metrics.separatorHeight * s;
            ++i;
            continue;
        }
        float line = (item.kind == PanelMenuItemKind::Check ? layout.checkLabelInset : layout.optionLabelInset) +
                     widthOf(labelWidths, i, item.label) + metrics.inset * s;
        if (item.kind == PanelMenuItemKind::Check) line += metrics.trailingGap * s + metrics.switchWidth * s;
        if (item.kind == PanelMenuItemKind::Submenu) line += metrics.trailingGap * s + metrics.chevronWidth * s;
        if (!item.shortcut.empty()) line += metrics.trailingGap * s + widthOf(shortcutWidths, i, item.shortcut);
        lineWidth = std::max(lineWidth, line);
        height += metrics.itemHeight * s;
        ++i;
    }

    const float pad = metrics.pad * s;
    const float margin = metrics.margin * s;
    const float gap = metrics.gap * s;
    const float overlap = metrics.submenuOverlap * s;
    // Only a drop-down is as wide as what it hangs from: a submenu's anchor
    // is the card it opens from.
    float width = std::max({metrics.minWidth * s, placement == PanelMenuPlacement::Below ? anchor.width : 0.0F,
                            lineWidth + 2.0F * pad});
    float x = anchor.x;
    float y = anchor.bottom() + gap;
    if (placement == PanelMenuPlacement::AtPoint) y = anchor.y;
    if (placement == PanelMenuPlacement::Beside) {
        x = anchor.right() - overlap;
        y = anchor.y - pad;
    }
    const float content = height;
    if (placement == PanelMenuPlacement::Fill) {
        x = bounds.x;
        y = bounds.y;
        width = bounds.width;
        height = std::min(height, bounds.height);
    } else if (bounds.visible()) {
        width = std::min(width, std::max(1.0F, bounds.width - 2.0F * margin));
        const float leftmost = bounds.x + margin;
        const float rightmost = bounds.right() - margin;
        // A submenu fitting on neither side of its card, moved in, would lie
        // over its own line, a press there landing on its first line: it
        // hangs under or over that line instead.
        bool hang = false;
        if (placement != PanelMenuPlacement::Below && x + width > rightmost) {
            // Turned to the left where it would not fit on the right -- or,
            // fitting neither way, to the side with more room.
            const float turned = placement == PanelMenuPlacement::AtPoint ? anchor.x - width
                                                                          : anchor.x + overlap - width;
            const float roomRight = rightmost - x;
            const float roomLeft = (placement == PanelMenuPlacement::AtPoint ? anchor.x : anchor.x + overlap) -
                                   leftmost;
            hang = placement == PanelMenuPlacement::Beside && turned < leftmost;
            if (turned >= leftmost || roomLeft > roomRight) {
                x = turned;
                layout.left = true;
            }
        }
        x = std::max(leftmost, std::min(x, rightmost - width));
        height = std::min(height, std::max(1.0F, bounds.height - 2.0F * margin));
        const float highest = bounds.y + margin;
        const float lowest = bounds.bottom() - margin;
        if (hang) {
            // On the side of the line with more room, cut to it to scroll,
            // and not moved back over the line as a drop-down would be.
            const float roomBelow = lowest - anchor.bottom();
            const float roomAbove = anchor.y - highest;
            if (roomBelow >= roomAbove) {
                y = anchor.bottom();
                height = std::min(height, std::max(1.0F, roomBelow));
            } else {
                height = std::min(height, std::max(1.0F, roomAbove));
                y = anchor.y - height;
                layout.above = true;
            }
        } else if (y + height > lowest) {
            if (placement == PanelMenuPlacement::Below) {
                const float roomBelow = lowest - y;
                const float roomAbove = anchor.y - gap - highest;
                if (roomAbove > roomBelow) {
                    y = anchor.y - gap - height;
                    layout.above = true;
                } else {
                    // Moved up to fit, over its button if it must be.
                    y = lowest - height;
                }
            } else if (placement == PanelMenuPlacement::AtPoint && anchor.y - height >= highest) {
                y = anchor.y - height;
                layout.above = true;
            } else {
                y = lowest - height;
            }
        }
        // Inside the bounds even when its button is not: a window made
        // shorter under an open menu can leave the button below them.
        y = std::clamp(y, highest, std::max(highest, lowest - height));
    }
    layout.card = {x, y, width, height};
    layout.view = {x, y + 0.5F * pad, width, std::max(0.0F, height - pad)};
    layout.maxScroll = std::max(0.0F, content - height);
    layout.scroll = std::clamp(scroll, 0.0F, layout.maxScroll);

    const float left = x + pad;
    const float inner = std::max(0.0F, width - 2.0F * pad);
    float top = y + pad - layout.scroll;
    layout.items.assign(items.size(), OverlayRect{});
    for (std::size_t i = 0; i < items.size();) {
        const auto& item = items[i];
        if (item.kind == PanelMenuItemKind::Segment) {
            std::size_t end = i;
            while (end < items.size() && items[end].kind == PanelMenuItemKind::Segment) ++end;
            const float trackHeight = std::min(metrics.trackHeight, metrics.segmentLine) * s;
            const OverlayRect track{left, top + (metrics.segmentLine * s - trackHeight) * 0.5F, inner, trackHeight};
            layout.tracks.push_back(track);
            const float trackPad = metrics.trackPad * s;
            const float segmentWidth = std::max(0.0F, track.width - 2.0F * trackPad) / static_cast<float>(end - i);
            for (std::size_t k = i; k < end; ++k) {
                layout.items[k] = {track.x + trackPad + segmentWidth * static_cast<float>(k - i),
                                   track.y + trackPad, segmentWidth, std::max(0.0F, track.height - 2.0F * trackPad)};
            }
            top += metrics.segmentLine * s;
            i = end;
            continue;
        }
        if (item.kind == PanelMenuItemKind::Separator) {
            if (separatorShown(i)) {
                layout.items[i] = {left, top, inner, metrics.separatorHeight * s};
                top += metrics.separatorHeight * s;
            }
            ++i;
            continue;
        }
        layout.items[i] = {left, top, inner, metrics.itemHeight * s};
        top += metrics.itemHeight * s;
        ++i;
    }
    return layout;
}

// Where a drop-down's list stands in a window of its own, in screen pixels,
// as a Windows combo box's does: its left edge at its box's and its top
// just under it, `width` by `height` (the card panelMenuLayout gives it with
// no bounds). Over the box instead where the screen -- `work`, the work
// area of the box's monitor -- has no room for it under the box and more
// over it; cut to the room on the side it takes, its lines to scroll, where
// it fits neither way; moved in from the screen's right edge only where it
// would cross it. Whole pixels, a window's: one as wide as its box covers
// the pixels the box's edges are drawn on.
inline OverlayRect panelMenuWindowFrame(const OverlayRect& box, const OverlayRect& work, float width, float height,
                                        float scale, const PanelMenuMetrics& metrics = {}) {
    const float s = scale > 0.0F ? scale : 1.0F;
    const float gap = std::round(metrics.windowGap * s);
    if (std::abs(width - box.width) <= 0.5F) width = std::round(box.right()) - std::round(box.x);
    width = std::ceil(std::max(1.0F, std::min(width, work.width)));
    height = std::ceil(std::max(1.0F, height));
    const float x = std::max(std::round(work.x), std::min(std::round(box.x), std::floor(work.right() - width)));
    const float below = std::round(box.bottom()) + gap;
    const float above = std::round(box.y) - gap;
    const float roomBelow = std::floor(work.bottom()) - below;
    const float roomAbove = above - std::ceil(work.y);
    if (height <= roomBelow || roomBelow >= roomAbove) {
        return {x, below, width, std::max(1.0F, std::min(height, roomBelow))};
    }
    height = std::max(1.0F, std::min(height, roomAbove));
    return {x, above - height, width, height};
}

// What the submenu opened from line `index` of `parent` hangs from (with
// PanelMenuPlacement::Beside): that line, widened to its card. Empty when
// the line is not one of the card's, or scrolled out of its view.
inline OverlayRect panelSubmenuAnchor(const PanelMenuLayout& parent, int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= parent.items.size()) return {};
    const OverlayRect& line = parent.items[static_cast<std::size_t>(index)];
    const float middle = line.y + line.height * 0.5F;
    if (!line.visible() || middle < parent.view.y || middle > parent.view.bottom()) return {};
    return {parent.card.x, line.y, parent.card.width, line.height};
}

// The item a point is on, -1 off the card, between items, on a line
// scrolled out of view or on one that cannot be chosen.
inline int panelMenuItemAt(const PanelMenuLayout& layout, const std::vector<PanelMenuItem>& items,
                           float x, float y) {
    if (!layout.card.contains(x, y) || !layout.view.contains(x, y)) return -1;
    for (std::size_t i = 0; i < items.size() && i < layout.items.size(); ++i) {
        if (panelMenuSelectable(items[i]) && layout.items[i].contains(x, y)) return static_cast<int>(i);
    }
    return -1;
}

// The scroll that brings item `index`'s line (a segment's whole track) into
// the view with the card's pad around it, the least way -- the first line
// all the way back to the top; the layout's own where it is in view already.
inline float panelMenuScrollToShow(const PanelMenuLayout& layout, int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= layout.items.size()) return layout.scroll;
    OverlayRect line = layout.items[static_cast<std::size_t>(index)];
    for (const auto& track : layout.tracks) {
        if (track.y <= line.y && line.bottom() <= track.bottom()) line = track;
    }
    const float edge = layout.view.y - layout.card.y;
    line.y -= edge;
    line.height += 2.0F * edge;
    float scroll = layout.scroll;
    if (line.bottom() > layout.view.bottom()) scroll += line.bottom() - layout.view.bottom();
    if (line.y < layout.view.y) scroll -= layout.view.y - line.y;
    return std::clamp(scroll, 0.0F, layout.maxScroll);
}

// The item the keyboard reaches from `from` going `direction` (1 down, -1
// up), round the ends: from none, the first going down and the last going
// up. -1 when there is nothing to choose.
inline int panelMenuStep(const std::vector<PanelMenuItem>& items, int from, int direction) {
    const int count = static_cast<int>(items.size());
    if (count == 0) return -1;
    const int step = direction < 0 ? -1 : 1;
    int index = from >= 0 && from < count ? from : (step > 0 ? -1 : count);
    for (int tried = 0; tried < count; ++tried) {
        index += step;
        if (index >= count) index = 0;
        if (index < 0) index = count - 1;
        if (panelMenuSelectable(items[static_cast<std::size_t>(index)])) return index;
    }
    return -1;
}

}  // namespace quaddeck
