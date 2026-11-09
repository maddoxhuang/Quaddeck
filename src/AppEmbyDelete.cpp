// App: deleting a video from the Emby server. F5 > General allows it; the
// browser's item menu, and the menu of a pane playing one, then offer it
// for a movie, an episode or a plain video. Before the window asks to
// confirm, and again once it is confirmed, the server is asked whether the
// item is a favorite of the account, or for an episode whether its series
// or its season is: a favorite is never deleted. The window
// (AppEmbyDeleteWindow.cpp) lists the files the server says go with the
// item (DeleteInfo, as Emby's own web client lists them). Replies arrive on
// the window thread through EmbyClient::pump(), as described in AppEmby.cpp.

#include "App.hpp"
#include "AppInternal.hpp"
#include "Diagnostics.hpp"
#include "TextEncoding.hpp"

#include <algorithm>
#include <string>

namespace quaddeck {

using namespace app_internal;

namespace {

std::wstring wide(const std::string& text) { return utf8ToWideText(text); }

std::wstring refusalText(emby::DeleteRefusal refusal) {
    switch (refusal) {
    case emby::DeleteRefusal::Type: return L"only a movie, an episode or a video can be deleted";
    case emby::DeleteRefusal::Favorite: return L"it is a favorite; unmark it to delete it";
    case emby::DeleteRefusal::SeriesFavorite: return L"its series is a favorite; unmark the series to delete it";
    case emby::DeleteRefusal::SeasonFavorite: return L"its season is a favorite; unmark the season to delete it";
    case emby::DeleteRefusal::Unconfirmed: return L"Emby did not say whether its series or season is a favorite";
    case emby::DeleteRefusal::None: break;
    }
    return {};
}

std::wstring typeName(const emby::Item& item) {
    if (item.type == "Movie") return L"Movie";
    if (item.type == "Episode") return L"Episode";
    return L"Video";
}

}  // namespace

bool App::embyDeleteCurrent(std::uint64_t serial) const {
    return embyPendingDelete_ && embyPendingDelete_->serial == serial &&
           embyPendingDelete_->accountSerial == embyAccountSerial_ && emby_.session().signedIn();
}

bool App::embyDeleteAsking() const {
    return embyPendingDelete_ && embyPendingDelete_->stage == EmbyPendingDelete::Stage::Asking;
}

void App::appendEmbyDeleteMenu(PanelMenu& menu, std::size_t pane) const {
    embyDeleteMenuOffer_.reset();
    if (!embyDeleteEnabled_ || !embyPaneAccountMatches(pane) || embyPanes_[pane]->itemId.empty()) return;
    const auto& state = *embyPanes_[pane];
    // The item as far as the pane knows it; the server is asked for its own
    // copy before anything is asked.
    emby::Item item;
    item.id = state.itemId;
    item.type = state.itemType;
    item.name = wideToUtf8Text(state.title);
    item.seriesId = state.seriesId;
    item.seasonId = state.seasonId;
    item.favorite = state.favorite.value_or(false);
    // Its type comes with the item reply: no entry before it, as for what
    // is never deleted.
    if (!emby::deletableType(item)) return;
    const bool known = state.favorite.has_value();
    const bool offered = known && !item.favorite && !embyPendingDelete_;
    menu.option(CmdEmbyDelete, item.favorite ? L"Delete from Emby server  (a favorite)" : L"Delete from Emby server...",
                false, {}, offered);
    if (offered) embyDeleteMenuOffer_ = EmbyDeleteOffer{embyAccountSerial_, static_cast<int>(pane), std::move(item)};
}

void App::embyAskDelete(const emby::Item& item) {
    if (!embyDeleteEnabled_ || !emby_.session().signedIn() || item.id.empty()) return;
    if (embyPendingDelete_) {
        showNotice(L"Emby is still busy with the last deletion");
        return;
    }
    const auto title = wide(emby::displayTitle(item));
    if (!emby::deletableType(item)) {
        showNotice(L"Not deleted: " + refusalText(emby::DeleteRefusal::Type));
        return;
    }
    if (item.favorite) {
        showNotice(L"Not deleted: " + title + L" -- " + refusalText(emby::DeleteRefusal::Favorite));
        return;
    }
    EmbyPendingDelete pending;
    pending.serial = ++embyDeleteSerial_;
    pending.accountSerial = embyAccountSerial_;
    pending.item = item;
    embyPendingDelete_ = std::move(pending);
    appendDiagnostic("Emby: asked to delete item " + item.id + "; checking it first");
    embyDeleteCheck(embyPendingDelete_->serial);
}

// The item as the server has it now: its type and its mark, and an
// episode's series and season, whose marks are asked next.
void App::embyDeleteCheck(std::uint64_t serial) {
    if (!embyDeleteCurrent(serial)) return;
    emby_.request("GET", emby::itemPath(emby_.session().userId, embyPendingDelete_->item.id), "",
                  [this, serial](const EmbyClient::Response& response) { embyDeleteItemArrived(serial, response); });
}

void App::embyDeleteItemArrived(std::uint64_t serial, const EmbyClient::Response& response) {
    if (!embyDeleteCurrent(serial)) return;
    if (!response.ok) {
        embyDeleteFailed(serial, L"Emby could not say what the item is now", &response);
        return;
    }
    const auto fresh = emby::parseItem(response.body);
    if (!fresh || fresh->id != embyPendingDelete_->item.id) {
        embyDeleteFailed(serial, L"Emby's reply about the item could not be read", nullptr);
        return;
    }
    embyPendingDelete_->fresh = *fresh;
    const auto guards = emby::deleteGuardIds(*fresh);
    if (guards.empty()) {
        embyDeleteJudge(serial, {});
        return;
    }
    emby_.request("GET", emby::deleteGuardsPath(emby_.session().userId, guards), "",
                  [this, serial](const EmbyClient::Response& reply) { embyDeleteGuardsArrived(serial, reply); });
}

void App::embyDeleteGuardsArrived(std::uint64_t serial, const EmbyClient::Response& response) {
    if (!embyDeleteCurrent(serial)) return;
    if (!response.ok) {
        embyDeleteFailed(serial, L"Emby could not say whether its series or season is a favorite", &response);
        return;
    }
    const auto list = emby::parseItems(response.body);
    if (!list) {
        embyDeleteFailed(serial, L"Emby's reply about its series and season could not be read", nullptr);
        return;
    }
    embyDeleteJudge(serial, list->items);
}

// Every mark answered: a refusal ends it here. Before the sheet, the files
// that go with the item are asked for; after it, the item is deleted.
void App::embyDeleteJudge(std::uint64_t serial, const std::vector<emby::Item>& guards) {
    if (!embyDeleteCurrent(serial)) return;
    const auto& fresh = embyPendingDelete_->fresh;
    if (const auto refusal = emby::deleteRefusal(fresh, guards); refusal != emby::DeleteRefusal::None) {
        appendDiagnostic("Emby: item " + fresh.id + " was not deleted: " + wideToUtf8Text(refusalText(refusal)));
        showNotice(L"Not deleted: " + wide(emby::displayTitle(fresh)) + L" -- " + refusalText(refusal));
        embyPendingDelete_.reset();
        return;
    }
    if (embyPendingDelete_->stage == EmbyPendingDelete::Stage::Deleting) {
        embyDeleteSend(serial);
        return;
    }
    emby_.request("GET", emby::deleteInfoPath(fresh.id), "",
                  [this, serial](const EmbyClient::Response& response) { embyDeleteInfoArrived(serial, response); });
}

void App::embyDeleteInfoArrived(std::uint64_t serial, const EmbyClient::Response& response) {
    if (!embyDeleteCurrent(serial)) return;
    if (!response.ok) {
        embyDeleteFailed(serial, L"Emby would not say which files go with it", &response);
        return;
    }
    const auto paths = emby::parseDeleteInfo(response.body);
    if (!paths) {
        embyDeleteFailed(serial, L"Emby's list of the files that go with it could not be read", nullptr);
        return;
    }
    auto& pending = *embyPendingDelete_;
    pending.paths = *paths;
    pending.stage = EmbyPendingDelete::Stage::Asking;
    // Another dialog holds the player (an Open dialog, say): a second one
    // over it would hand the player back while the first is still up.
    if (!openEmbyDeleteWindow()) {
        appendDiagnostic("Emby: item " + pending.fresh.id + " was not deleted: the window to confirm it could not open");
        showNotice(L"Not deleted: " + wide(emby::displayTitle(pending.fresh)) +
                   L" -- close the other QuadDeck dialog first");
        embyPendingDelete_.reset();
    }
}

void App::embyConfirmDelete() {
    if (!embyDeleteAsking()) return;
    auto& pending = *embyPendingDelete_;
    closeEmbyDeleteWindow();
    if (!embyDeleteCurrent(pending.serial)) {
        embyPendingDelete_.reset();
        return;
    }
    // A mark may have been set since the window asked, on another device
    // too: the marks are asked again, and only then is the item deleted.
    pending.stage = EmbyPendingDelete::Stage::Deleting;
    appendDiagnostic("Emby: deleting item " + pending.fresh.id + " was confirmed; checking it again");
    embyDeleteCheck(pending.serial);
}

void App::embyCancelDelete() {
    // Confirmed, it goes on: only the checks before the window and the
    // window itself are given up.
    if (!embyPendingDelete_ || embyPendingDelete_->stage == EmbyPendingDelete::Stage::Deleting) return;
    embyDropDelete();
}

void App::embyDropDelete() {
    closeEmbyDeleteWindow();
    embyPendingDelete_.reset();
}

void App::embyDeleteSend(std::uint64_t serial) {
    if (!embyDeleteCurrent(serial)) return;
    const auto& fresh = embyPendingDelete_->fresh;
    const auto path = emby::deleteItemPath(fresh.id);
    if (path.empty()) {
        embyDeleteFailed(serial, L"its id could name more than one item", nullptr);
        return;
    }
    // A pane playing it closes first, its Stopped report going out ahead of
    // the deletion on the same queue, so the server is not still sending
    // the file it is told to delete.
    for (std::size_t pane = 0; pane < kMaxPanes; ++pane) {
        if (embyPaneAccountMatches(pane) && embyPanes_[pane]->itemId == fresh.id) closePane(pane);
    }
    appendDiagnostic("Emby: deleting item " + fresh.id);
    emby_.request("POST", path, "",
                  [this, serial](const EmbyClient::Response& response) { embyDeleteArrived(serial, response); });
}

void App::embyDeleteArrived(std::uint64_t serial, const EmbyClient::Response& response) {
    if (!embyDeleteCurrent(serial)) return;
    const auto item = embyPendingDelete_->fresh;
    embyPendingDelete_.reset();
    const auto title = wide(emby::displayTitle(item));
    if (!response.ok) {
        if (response.status == 401) {
            embySessionExpired();
            return;
        }
        appendDiagnostic("Emby: could not delete item " + item.id + ": " + response.error);
        showNotice(L"Emby did not delete " + title + L" (" + wide(response.error) + L")");
        return;
    }
    appendDiagnostic("Emby: deleted item " + item.id);
    embyRemoveDeleted(item.id);
    showNotice(L"Deleted from the Emby server: " + title);
}

void App::embyDeleteFailed(std::uint64_t serial, const std::wstring& reason, const EmbyClient::Response* response) {
    if (!embyDeleteCurrent(serial)) return;
    const auto itemId = embyPendingDelete_->item.id;
    embyPendingDelete_.reset();
    if (response && response->status == 401) {
        embySessionExpired();
        return;
    }
    const std::string detail = response ? ": " + response->error : std::string{};
    appendDiagnostic("Emby: item " + itemId + " was not deleted: " + wideToUtf8Text(reason) + detail);
    showNotice(L"Not deleted: " + reason + (response ? L" (" + wide(response->error) + L")" : std::wstring{}));
}

// The server no longer has it: every copy QuadDeck holds goes -- the list
// and its count, Continue watching, the information page (closed when it
// was its own), each pane's queue and a pending replacement.
void App::embyRemoveDeleted(const std::string& itemId) {
    const auto drop = [&](std::vector<emby::Item>& items) {
        return std::erase_if(items, [&](const emby::Item& entry) { return entry.id == itemId; });
    };
    const auto listed = drop(embyItems_);
    embyTotal_ = std::max(0, embyTotal_ - static_cast<int>(listed));
    drop(embyResume_);
    if (embyDetails_) {
        if (embyDetails_->item.id == itemId) {
            embyCloseDetails();
        } else {
            drop(embyDetails_->sourceItems);
            drop(embyDetails_->seasons);
            drop(embyDetails_->episodes);
        }
    }
    for (std::size_t pane = 0; pane < kMaxPanes; ++pane) {
        if (!embyPanes_[pane]) continue;
        if (embyPaneAccountMatches(pane) && embyPanes_[pane]->itemId == itemId) closePane(pane);
        else emby::removeFromQueue(embyPanes_[pane]->queue, itemId);
    }
    if (embyPendingPlay_) {
        if (embyPendingPlay_->item.id == itemId) embyCancelReplacement();
        else emby::removeFromQueue(embyPendingPlay_->queue, itemId);
    }
    // The browser's rows moved under a press that was waiting for its
    // release.
    panelPressArmed_ = false;
    pressedPanelKind_ = PanelHitKind::None;
}

DeleteDialogContent App::embyDeleteDialogContent() const {
    DeleteDialogContent content;
    if (!embyDeleteAsking()) return content;
    const auto& pending = *embyPendingDelete_;
    content.caption = L"Delete from Emby server";
    content.title = wide(emby::displayTitle(pending.fresh));
    content.type = typeName(pending.fresh);
    if (const int pane = embyPlayingPane(pending.fresh.id); pane >= 0) {
        content.playing =
            L"Playing in V" + std::to_wstring(positionForPane(static_cast<std::size_t>(pane)) + 1) + L", which closes";
    }
    content.files.reserve(pending.paths.size());
    for (const auto& path : pending.paths) content.files.push_back(wide(path));
    const std::wstring what = pending.paths.empty()       ? L"Emby names no file for it: only its entry goes."
                              : pending.paths.size() == 1 ? L"Emby deletes this file from its disk."
                                                          : L"Emby deletes these " + std::to_wstring(pending.paths.size()) +
                                                                L" files from its disk.";
    content.warning = what + L" This can't be undone.";
    return content;
}

}  // namespace quaddeck
