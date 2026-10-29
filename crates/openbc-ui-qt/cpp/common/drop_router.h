// drop_router.h
// ---------------------------------------------------------------------------
// The single, central drop-routing policy for Home and every compare tab.
//
// Pure and std-only: it never touches widgets, files or Qt. The caller (the
// Qt layer in drop_payload.h / main_window.h) resolves each dropped item to a
// `ViewerKind` using the EXISTING dispatch (classifyFilePath in file_kinds.h,
// refined by content_sniff.h) and passes it in; the router only decides what
// to do with the result. That keeps one set of type-dispatch rules and makes
// this policy unit-testable (tools/drop_router_test).
//
// POLICY (documented here, mirrored in docs/DROP_AND_RELOAD.md)
//
//  Items
//   * 0 usable items                 -> Reject.
//   * more than 2 items              -> Reject (explicit message; nothing is guessed).
//   * a folder together with a file  -> Reject (cannot be compared).
//   * order                          -> payload order: first = left, second = right.
//
//  Target comparison type
//   * folders                        -> Folder Compare.
//   * one file / two files same kind -> that kind's viewer (Version -> Hex: there is
//                                       no version viewer).
//   * two files of different kinds   -> Hex Compare, the only viewer that accepts any
//                                       pair, with a message saying so.
//
//  Destination
//   * Home                           -> OpenNewTab (Home is never replaced).
//   * same kind as the tab           -> ApplyToTab (tab reused, identity preserved).
//   * different kind, tab Empty      -> ReplaceEmptyTab (new viewer takes the tab's slot).
//   * different kind, tab Populated  -> OpenNewTab (existing comparison untouched).
//   * different kind, tab Dirty      -> OpenNewTab (unsaved work is never touched).
//
//  Which side a single item fills (same-kind tab)
//   * the side under the cursor (`sideHint`) ALWAYS wins: a file dropped on the right half
//     fills the right side, on the left half the left side, whether or not that side was
//     already set. The other side is preserved.
//   * only when the UI cannot tell the side (no hint): left empty -> left; else right empty
//     -> right; else left.
//   * Two items always set both sides.
//
//  In-place conversion between viewer types is deliberately NOT done: each viewer
//  is a distinct widget that owns a native session, so "convert" means swapping in a
//  fresh viewer, which is only safe when the old tab holds nothing.
// ---------------------------------------------------------------------------
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "common/viewer_kind.h"

namespace openbc::app::drop {

enum class TabKind { Home, Folder, Text, Picture, Model3d, Table, Registry, Hex, Media };
enum class Occupancy { Empty, Populated, Dirty };
enum class Side { Left, Right };
enum class Action { Reject, ApplyToTab, ReplaceEmptyTab, OpenNewTab };
enum class Assign { None, Both, Left, Right };

struct Item {
    std::string path;       // normalised local path, or a remote URI
    bool isDir = false;
    ViewerKind kind = ViewerKind::Text;  // already refined by content sniffing; ignored for folders
};

struct TabContext {
    TabKind tab = TabKind::Home;
    Occupancy occupancy = Occupancy::Empty;
    bool leftSet = false;
    bool rightSet = false;
    std::optional<Side> sideHint;  // side under the cursor, when the UI knows it
};

struct Decision {
    Action action = Action::Reject;
    TabKind target = TabKind::Home;   // comparison type to use (== tab for ApplyToTab)
    Assign assign = Assign::None;
    std::string left;                 // path for the left side ("" = leave unchanged)
    std::string right;                // path for the right side ("" = leave unchanged)
    bool run = false;                 // start the comparison after applying
    bool confirmDiscard = false;      // ApplyToTab over unsaved edits: viewer must confirm first
    std::string message;              // status-bar text; non-empty for rejects and fallbacks
};

inline const char* name(TabKind k) {
    switch (k) {
    case TabKind::Home: return "Home";
    case TabKind::Folder: return "Folder Compare";
    case TabKind::Text: return "Text Compare";
    case TabKind::Picture: return "Picture Compare";
    case TabKind::Model3d: return "3D Compare";
    case TabKind::Table: return "Table Compare";
    case TabKind::Registry: return "Registry Compare";
    case TabKind::Hex: return "Hex Compare";
    case TabKind::Media: return "Media Compare";
    }
    return "Compare";
}

// Version has no viewer of its own; it is byte-level data, so it goes to Hex.
inline TabKind tabKindFor(ViewerKind k) {
    switch (k) {
    case ViewerKind::Text: return TabKind::Text;
    case ViewerKind::Picture: return TabKind::Picture;
    case ViewerKind::Model3d: return TabKind::Model3d;
    case ViewerKind::Table: return TabKind::Table;
    case ViewerKind::Media: return TabKind::Media;
    case ViewerKind::Registry: return TabKind::Registry;
    case ViewerKind::Version:
    case ViewerKind::Hex: return TabKind::Hex;
    }
    return TabKind::Text;
}

inline Decision reject(std::string message) {
    Decision d;
    d.action = Action::Reject;
    d.message = std::move(message);
    return d;
}

inline Decision route(const TabContext& ctx, const std::vector<Item>& items) {
    if (items.empty()) return reject("Nothing to open: the drop contained no usable files or folders.");
    if (items.size() > 2) {
        return reject("Drop one or two items at a time to compare (" + std::to_string(items.size()) +
                      " were dropped). Nothing was opened.");
    }
    bool anyDir = false, allDir = true;
    for (const Item& it : items) {
        anyDir = anyDir || it.isDir;
        allDir = allDir && it.isDir;
    }
    if (anyDir && !allDir) return reject("A folder cannot be compared with a file. Drop two folders or two files.");

    Decision d;
    std::string fallbackNote;
    if (allDir) {
        d.target = TabKind::Folder;
    } else {
        const TabKind first = tabKindFor(items[0].kind);
        d.target = first;
        if (items.size() == 2) {
            const TabKind second = tabKindFor(items[1].kind);
            if (second != first) {
                d.target = TabKind::Hex;
                fallbackNote = std::string("Different file types (") + name(first) + " / " + name(second) +
                               "): opened as a byte-level Hex Compare.";
            }
        }
    }

    const bool sameKind = ctx.tab == d.target;

    // --- which side(s) get which path
    if (items.size() == 2) {
        d.assign = Assign::Both;
        d.left = items[0].path;
        d.right = items[1].path;
        d.run = true;
    } else if (sameKind) {
        if (ctx.sideHint) d.assign = *ctx.sideHint == Side::Right ? Assign::Right : Assign::Left;  // predictable: where it lands
        else if (!ctx.leftSet) d.assign = Assign::Left;
        else if (!ctx.rightSet) d.assign = Assign::Right;
        else d.assign = Assign::Left;
        (d.assign == Assign::Left ? d.left : d.right) = items[0].path;
        // Run as soon as both sides will be populated (one dropped + the preserved other one).
        d.run = d.assign == Assign::Left ? ctx.rightSet : ctx.leftSet;
    } else {
        d.assign = Assign::Left;  // a fresh viewer starts from the dropped item
        d.left = items[0].path;
        d.run = false;
    }

    // --- tab handling
    if (ctx.tab == TabKind::Home) {
        d.action = Action::OpenNewTab;
    } else if (sameKind) {
        d.action = Action::ApplyToTab;
        d.confirmDiscard = ctx.occupancy == Occupancy::Dirty;
    } else if (ctx.occupancy == Occupancy::Empty) {
        d.action = Action::ReplaceEmptyTab;
    } else {
        d.action = Action::OpenNewTab;
        d.message = std::string("The ") + name(ctx.tab) + " tab was kept as it is; opened " + name(d.target) +
                    " in a new tab.";
    }
    if (!fallbackNote.empty()) d.message = d.message.empty() ? fallbackNote : fallbackNote + " " + d.message;
    return d;
}

}  // namespace openbc::app::drop
