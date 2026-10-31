// context_menu_state.h
// ---------------------------------------------------------------------------
// The "left" item chosen with the file-manager entry "openbc: Select Left Folder/File".
//
//   * It lives in memory of the running OpenBC process only. Selecting it opens no tab; the first
//     tab appears when "Compare To Left" supplies the right side.
//   * File managers show "openbc: Compare To Left ..." only while a left item exists, so the state is
//     also published to the desktop and removed again when it is cleared or OpenBC exits:
//       Windows : per-user Explorer verbs under HKCU\Software\Classes (...\shell\OpenBC.CompareToLeft)
//       Linux   : $XDG_RUNTIME_DIR/openbc/left-selection (read by openbc-nautilus.py) and the Dolphin
//                 service menu ~/.local/share/kio/servicemenus/openbc-compare-to-left-<kind>.desktop
//
// Never persisted in preferences.ini: a left item from an earlier run is not offered again.
// ---------------------------------------------------------------------------
#pragma once

#include <QString>

namespace openbc::app {

class ContextMenuState {
public:
    enum class Kind { None, Folder, File };

    ContextMenuState() = default;
    ~ContextMenuState() { clear(); }
    ContextMenuState(const ContextMenuState&) = delete;
    ContextMenuState& operator=(const ContextMenuState&) = delete;

    // Remembers `path` as the left side and publishes the "Compare To Left" entry for its kind.
    void setLeft(Kind kind, const QString& path);
    // Forgets the left item and removes the published entries.
    void clear();

    // Removes entries left behind by an OpenBC that crashed. Call once at startup.
    static void purgeStale();

    bool hasLeft() const { return kind_ != Kind::None && !path_.isEmpty(); }
    Kind kind() const { return kind_; }
    const QString& path() const { return path_; }
    bool matches(Kind kind) const { return hasLeft() && kind_ == kind; }

private:
    Kind kind_ = Kind::None;
    QString path_;
};

}  // namespace openbc::app
