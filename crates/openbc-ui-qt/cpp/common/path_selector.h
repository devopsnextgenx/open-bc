    // path_selector.h
    // ---------------------------------------------------------------------------
    // PathSelector: the "combo box + Browse [+ Up]" path-entry row, pulled out
    // of FolderCompareViewer's folder pickers so TextCompareViewer's per-side file
    // pickers can use the exact same widget instead of a plain read-only
    // QLabel. One place now owns the row's look (spacing, icons, placeholder
    // text, MRU dropdown history) and its OS dialog, so the two spots can't
    // quietly drift apart the way two hand-built copies eventually do.
    //
    // The widget only owns the row itself and the picking dialog. What should
    // happen once a path is picked - refresh a folder compare, load a new file
    // into an editor, log a message, remember it in the MRU list, etc. - stays
    // the caller's job, wired through the onBrowsed/onWentUp callbacks below
    // (same std::function-callback style FolderCompareViewer/TextCompareViewer already
    // use for onProgress/onComplete/onTitleChanged, so this fits in without
    // needing Q_OBJECT/moc for a signal nobody else needs).
    //
    // As of the remote-connections work, the row also grows a second button
    // (on by default - pass enableRemote=false to opt out) that opens
    // RemotePathBrowserDialog: pick "This computer" and browse a real local
    // tree, or pick a saved SFTP/FTP/FTPS/SMB/network-drive connection and
    // browse (or, without a live backend wired up yet, type) its path. A
    // remote pick is remembered in the MRU dropdown exactly like a local one,
    // as a "scheme://user@host/path" string - see selectedIsRemote()/
    // selectedProfile() on the browser dialog if a caller needs to know a
    // given pick came from a remote host rather than the local disk.
    // ---------------------------------------------------------------------------
    #pragma once

    #include <QComboBox>
    #include <QDir>
    #include <QFileDialog>
    #include <QHBoxLayout>
    #include <QLineEdit>
    #include <QMenu>
    #include <QSignalBlocker>
    #include <QString>
    #include <QStyle>
    #include <QToolButton>
    #include <QWidget>
    #include <functional>
    #include <utility>

    #include "style/qt_style.h"
    #include "remote/remote_path_browser_dialog.h"

    namespace openbc::app {

    namespace icons = openbc::ui::icons;

    class PathSelector : public QWidget {
    public:
        enum class Mode { Folder, File };

        // `side` names whose path this is ("Left", "Right", ...) - used in the
        // placeholder text and in the dialog's title/tooltips, just like the
        // "left"/"right" strings FolderCompareViewer already threads through its own
        // chooseFolder()/log() calls. `showUp` adds the "go to parent folder"
        // button; FolderCompareViewer's folder pickers want it, TextCompareViewer's
        // file pickers don't (a file has no meaningful "parent" to browse up
        // to), so it defaults off and Mode::File ignores it even if passed.
        explicit PathSelector(Mode mode, QString side, QWidget* parent = nullptr, bool showUp = false,
                            QString fileFilter = "All files (*.*)", bool enableRemote = true,
                            RemoteBrowseBridge* remoteBridge = nullptr)
            : QWidget(parent), mode_(mode), side_(std::move(side)), fileFilter_(std::move(fileFilter)),
            remoteBridge_(remoteBridge) {
            auto* layout = new QHBoxLayout(this);
            layout->setContentsMargins(0, 0, 0, 0);
            layout->setSpacing(0);

            combo_ = new QComboBox(this);
            combo_->setEditable(true);
            combo_->setInsertPolicy(QComboBox::NoInsert);
            combo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
            combo_->setMinimumContentsLength(10);
            combo_->lineEdit()->setPlaceholderText(side_ +
                                                    (mode_ == Mode::Folder ? " folder path" : " file path"));

            browse_ = new QToolButton(this);
            browse_->setIcon(icons::glyph(icons::Glyph::FolderOpen));
            browse_->setToolTip((mode_ == Mode::Folder ? "Select " : "Choose ") + side_.toLower() +
                                (mode_ == Mode::Folder ? " folder" : " file"));

            layout->addWidget(combo_, 1);
            layout->addWidget(browse_);

            if (enableRemote) {
                remote_ = new QToolButton(this);
                remote_->setIcon(style()->standardIcon(QStyle::SP_DriveNetIcon));
                remote_->setToolTip("Browse local files, or connect to a remote host (SFTP/FTP/FTPS/SMB/network drive)...");
                layout->addWidget(remote_);
            }

            if (showUp && mode_ == Mode::Folder) {
                up_ = new QToolButton(this);
                up_->setIcon(icons::glyph(icons::Glyph::FolderUp));
                up_->setToolTip("Go to parent folder");
                layout->addWidget(up_);
            }

            connect(browse_, &QToolButton::clicked, this, [this]() {
                const QString selected = pickPath();
                if (!selected.isEmpty()) {
                    lastPickWasRemote_ = false;
                    lastRemoteProfile_ = {};
                    setText(selected);
                    if (onBrowsed) onBrowsed(selected);
                }
            });
            if (remote_) {
                connect(remote_, &QToolButton::clicked, this, [this]() { browseRemote(); });
            }
            if (up_) {
                connect(up_, &QToolButton::clicked, this, [this]() {
                    QDir dir(text());
                    if (!text().isEmpty() && dir.cdUp()) {
                        setText(dir.absolutePath());
                        if (onWentUp) onWentUp(dir.absolutePath());
                    }
                });
            }
        }

        QComboBox* combo() const { return combo_; }
        QToolButton* browseButton() const { return browse_; }
        QToolButton* remoteButton() const { return remote_; }  // nullptr unless enableRemote was requested
        QToolButton* upButton() const { return up_; }  // nullptr unless showUp was requested

        QString text() const { return combo_->currentText().trimmed(); }
        void setText(const QString& text) { combo_->setEditText(text); }

        // Pushes `text` to the top of the dropdown history, removing any
        // earlier duplicate of it first - the same MRU behaviour every path
        // field in the app has always had.
        void remember(const QString& text) {
            if (text.isEmpty()) return;
            const QSignalBlocker blocker(combo_);
            const int existing = combo_->findText(text);
            if (existing >= 0) combo_->removeItem(existing);
            combo_->insertItem(0, text);
            combo_->setCurrentIndex(0);
        }

        // Runs the OS folder/file dialog appropriate for `mode`, seeded with
        // the current text. Returns the chosen path, or an empty string if the
        // user cancelled. Does not touch the combo box or fire onBrowsed -
        // exposed separately so a caller with its own trigger for "browse"
        // (FolderCompareViewer's "Select Left/Right folder..." menu actions, which
        // need the exact same dialog as the row's own Browse button) doesn't
        // have to click the button itself to reuse the logic.
        QString pickPath() {
            if (mode_ == Mode::Folder) {
                return QFileDialog::getExistingDirectory(this, "Select " + side_ + " folder", text());
            }
            const QString filter = fileFilterProvider ? fileFilterProvider() : fileFilter_;
            return QFileDialog::getOpenFileName(this, "Choose " + side_ + " file", text(), filter);
        }

        // Opens RemotePathBrowserDialog (This computer / a saved remote
        // connection) and, on Select, commits the result the same way pickPath()
        // does for the local-only dialog: updates the text, records whether the
        // pick was remote, and fires onBrowsed.
        void browseRemote() {
            RemotePathBrowserDialog dialog(mode_ == Mode::Folder ? PathPickMode::Folder : PathPickMode::File,
                                        remoteBridge_, this);
            dialog.setInitialPath(text());
            if (dialog.exec() != QDialog::Accepted) return;
            const QString selected = dialog.selectedPath();
            if (selected.isEmpty()) return;
            lastPickWasRemote_ = dialog.selectedIsRemote();
            lastRemoteProfile_ = dialog.selectedProfile();
            setText(selected);
            if (onBrowsed) onBrowsed(selected);
        }

        // True if the most recent Browse/browseRemote pick came from a saved
        // remote connection rather than the local filesystem; callers that need
        // to route reads through openbc-vfs instead of the local disk check
        // this (and lastRemoteProfile()) after onBrowsed fires.
        bool lastPickWasRemote() const { return lastPickWasRemote_; }
        RemoteProfile lastRemoteProfile() const { return lastRemoteProfile_; }

        // Fired after a successful Browse pick, or Up navigation, respectively
        // - the two things this widget does on its own. Enter-in-the-line-edit
        // and picking an existing MRU entry are left for the caller to wire
        // directly to combo()/combo()->lineEdit(), same as before, since what
        // "commit this path" should do (refresh a compare vs. load a file)
        // differs per caller.
        std::function<void(QString)> onBrowsed;

        // Optional: computes the file-dialog filter each time the dialog opens, for viewers whose list
        // of supported extensions is only known once their backend handle exists (e.g. 3D models).
        std::function<QString()> fileFilterProvider;
        std::function<void(QString)> onWentUp;

    private:
        Mode mode_;
        QString side_;
        QString fileFilter_;
        QComboBox* combo_ = nullptr;
        QToolButton* browse_ = nullptr;
        QToolButton* remote_ = nullptr;
        QToolButton* up_ = nullptr;
        RemoteBrowseBridge* remoteBridge_ = nullptr;
        bool lastPickWasRemote_ = false;
        RemoteProfile lastRemoteProfile_;
    };

    // Builds the standard "file path row" used by every single-file-per-side viewer (table, registry,
    // hex, media, picture, 3D): a PathSelector whose Browse / Enter / MRU-pick all funnel into one
    // `commit(path)` callback, so each viewer only implements "what to do with a new path".
    inline PathSelector* makeCommitSelector(QWidget* parent, const QString& side, const QString& initial,
                                            const QString& fileFilter, std::function<void(QString)> commit) {
        auto* selector = new PathSelector(PathSelector::Mode::File, side, parent, false, fileFilter, false);
        if (!initial.isEmpty()) {
            selector->remember(initial);
            selector->setText(initial);
        }
        selector->onBrowsed = commit;
        QObject::connect(selector->combo()->lineEdit(), &QLineEdit::returnPressed, selector,
                        [selector, commit]() { commit(selector->text()); });
        QObject::connect(selector->combo(), QOverload<int>::of(&QComboBox::activated), selector,
                        [selector, commit](int) { commit(selector->text()); });
        return selector;
    }

    }  // namespace openbc::app
