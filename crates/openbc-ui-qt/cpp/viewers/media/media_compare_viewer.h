// media_compare_viewer.h
// ---------------------------------------------------------------------------
// MediaCompareViewer compares media metadata and shows an aligned waveform/delta
// visualization for supported audio files.
//
// Presentation only: header parsing, property alignment and tag WRITING happen
// in the backend behind the openbc_media_* C ABI. Rows whose values differ are
// highlighted.
//
// Opening: the tab can be opened with no files (from the Home page); each side
// has a PathSelector (combo + Browse). Picking / pressing Enter / choosing an MRU
// entry starts (or restarts) the session for the paths that are set.
//
// Playback is a preview aid only: MediaPlayerPane uses Qt Multimedia with the
// operating system's decoders, so no codecs are shipped with the app.
//   - Each side has its own Play / Pause / Stop buttons.
//   - A shared strip holds a master volume + mute and an A/B CROSSFADE slider.
//     While both files play, the crossfade moves smoothly from Left-only to
//     Right-only (equal-power curve) so a switch between the two can be judged
//     for continuity. "Link seek" mirrors a slider drag to the other side.
//
// Editing: rows the backend flags as editable (bit 2) can be edited by
// double-clicking the Left/Right cell. Edits (and cover replace/remove) stay
// pending until saved. A pending cell is tinted green - the same tint text
// compare paints on edited lines - and the side's Save button (next to its path)
// turns blue. Save writes with the side's button, "Save Left" / "Save Right", or
// Ctrl+S, which saves the ACTIVE side (the column of the current row, or the
// player pane that last had focus). Saving releases that side's player (file
// lock on Windows), asks the backend to rewrite the file, and the backend answers
// with a fresh Result (a failed save is reported through that side's error text,
// "Save failed: ..."). Closing the tab / Reload / changing a path with pending
// edits shows the shared two-sided UnsavedChangesDialog.
// ---------------------------------------------------------------------------
#pragma once

#ifdef OPENBC_HAVE_QT_MULTIMEDIA
#include <QAudio>
#endif

#include <QAction>
#include <QBrush>
#include <QByteArray>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QFileInfo>
#include <QFont>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QKeySequence>
#include <QMessageBox>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QSlider>
#include <QShowEvent>
#include <QSplitter>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QVariant>
#include <QWidget>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

#include "backend/backend_channel.h"
#include "backend/compare_backend_api.h"
#include "viewers/media/media_player_pane.h"
#include "viewers/media/audio_diff_visualizer.h"
#include "common/path_selector.h"
#include "common/remote_selector.h"
#include "common/unsaved_changes_dialog.h"
#include "common/file_change_controller.h"
#include "style/qt_style.h"

namespace openbc::app {

namespace icons = openbc::ui::icons;

using MediaViewPtr = std::shared_ptr<OpenBcMediaView>;

namespace media_detail {

// Item data roles (per column unless noted).
constexpr int kOrigRole = Qt::UserRole;          // original value text of that side
constexpr int kHadRole = Qt::UserRole + 1;       // original value existed on that side
constexpr int kShownRole = Qt::UserRole + 2;     // cell currently shows a real value (not the "-" placeholder)
constexpr int kEditableRole = Qt::UserRole + 3;  // col 0: backend flagged the row editable
constexpr int kKeyRole = Qt::UserRole + 4;       // col 0: group + 0x1f + key
constexpr int kBackendDiffRole = Qt::UserRole + 5;
constexpr int kInfoRole = Qt::UserRole + 6;
constexpr double kPi = 3.14159265358979323846;

// Starts the inline editor empty when the cell only shows the "-" placeholder.
class DashAwareDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void setEditorData(QWidget* editor, const QModelIndex& index) const override {
        QStyledItemDelegate::setEditorData(editor, index);
        if (!index.data(kShownRole).toBool()) {
            if (auto* edit = qobject_cast<QLineEdit*>(editor)) edit->clear();
        }
    }
};

}  // namespace media_detail

class MediaCompareViewer : public QWidget {
public:
    // Both paths may be empty: the tab then opens directly and waits for the user to pick files.
    MediaCompareViewer(const QString& leftPath = QString(), const QString& rightPath = QString(), QWidget* parent = nullptr)
        : QWidget(parent), leftPath_(leftPath), rightPath_(rightPath),
          channel_(this, &MediaCompareViewer::destroyHandle, [this](const BackendChannel::Event& event) { onEvent(event); }) {
        setObjectName("mediaCompareView");
        buildUi();
        changes_ = std::make_unique<FileChangeController>(this, [this](std::uint8_t) { reloadLatest(); }, /*wholeTab=*/true,
                                                          [this]() { return confirmDiscard(); });
        if (!leftPath_.isEmpty() || !rightPath_.isEmpty()) startSession();
        else showEmptyState();
    }

    ~MediaCompareViewer() override {
        leftPane_->stop();
        rightPane_->stop();
        stopSession();
    }

    QString title() const {
        if (leftPath_.isEmpty() && rightPath_.isEmpty()) return "Media Compare";
        const auto name = [](const QString& p) { return p.isEmpty() ? QString("(missing)") : QFileInfo(p).fileName(); };
        return name(leftPath_) + " <-> " + name(rightPath_);
    }
    QString detailedTitle() const { return leftPath_ + "  <->  " + rightPath_; }
    QString leftText() const { return leftPath_; }
    QString rightText() const { return rightPath_; }

    // Drop target entry point (common/drop_router.h): an empty argument leaves that side untouched.
    void applyDroppedPaths(const QString& left, const QString& right) {
        const QString wanted[2] = {cleanInput(left), cleanInput(right)};
        for (std::uint8_t side = 0; side < 2; ++side) {
            if (wanted[side].isEmpty()) continue;
            commitPath(side, wanted[side]);
            if ((side == 0 ? leftPath_ : rightPath_) != wanted[side]) return;  // refused or cancelled: stop
        }
    }

    // For the tab host: call before closing the tab. confirmClose() shows the two-sided
    // dialog when there are unsaved edits and returns false if the user cancels.
    bool hasUnsavedChanges() const { return dirty(0) || dirty(1); }
    bool confirmClose() { return confirmDiscard(); }

    std::function<void()> onHomeRequested;
    std::function<void()> onSessionsRequested;
    std::function<void()> onTitleChanged;

    QList<QAction*> editMenuItems() const { return {saveAction_, saveLeftAction_, saveRightAction_}; }

    QList<QAction*> actionsMenuItems() const {
        return {playLeftAction_, playRightAction_, playBothAction_, saveLeftAction_, saveRightAction_, reloadAction_};
    }

protected:
    void showEvent(QShowEvent* event) override {
        QWidget::showEvent(event);
        if (!splitInit_) {
            splitInit_ = true;
            applyDefaultSplit();
        }
    }

private:
    using Edits = QMap<QString, QVariant>;  // group\x1fkey -> new value; a null QVariant removes the tag
    enum CoverMode : std::uint8_t { CoverKeep = 0, CoverReplace = 1, CoverRemove = 2 };

    struct Cell {
        bool shown = false;
        QString text;
        bool pending = false;
    };

    static QString editKey(const QString& group, const QString& key) { return group + QChar(0x1f) + key; }

    static void destroyHandle(int kind, void* handle) {
        if (kind == OPENBC_EV_RESULT) openbc_media_view_destroy(static_cast<OpenBcMediaView*>(handle));
    }

    // ---- session ----------------------------------------------------------

    void startSession() {
        stopSession();
        if (changes_) { changes_->track(0, leftPath_); changes_->track(1, rightPath_); }
        view_.reset();
        audioVisualizer_->hide();
        audioStatus_->hide();
        previewed_[0] = previewed_[1] = false;
        clearPending(0);
        clearPending(1);
        leftPane_->reset();
        rightPane_->reset();
        const QByteArray left = openbc_remote::localCopy(leftPath_).toUtf8(), right = openbc_remote::localCopy(rightPath_).toUtf8();
        session_ = openbc_media_session_create(left.constData(), right.constData(), BackendChannel::callback(), channel_.user());
        summary_->setText("Reading media headers...");
        if (onTitleChanged) onTitleChanged();
    }

    void stopSession() {
        if (!session_) return;
        openbc_media_session_destroy(session_);
        session_ = nullptr;
    }

    void showEmptyState() {
        stopSession();
        view_.reset();
        tree_->clear();
        plain_.clear();
        errors_->hide();
        sideEditable_[0] = sideEditable_[1] = false;
        for (std::uint8_t s = 0; s < 2; ++s) pane(s)->setCoverEditable(false);
        summary_->setText("Choose a file for the left and/or right side above");
        updateSaveActions();
        if (onTitleChanged) onTitleChanged();
    }

    void onEvent(const BackendChannel::Event& event) {
        if (event.kind == OPENBC_EV_STATUS) {
            summary_->setText(event.message);
        } else if (event.kind == OPENBC_EV_RESULT) {
            view_ = std::static_pointer_cast<OpenBcMediaView>(event.handle);
            updateAudioVisualization();
            bool wasSave = false, saveOk = true;
            QString saveError;
            if (savingSide_ >= 0) {
                // A save re-read that side: drop its pending edits unless the backend says the save failed.
                const auto side = static_cast<std::uint8_t>(savingSide_);
                savingSide_ = -1;
                wasSave = true;
                saveError = sideError(side);
                saveOk = !saveError.startsWith("Save failed");
                if (saveOk) clearPending(side);
            } else {
                clearPending(0);  // fresh session / reload
                clearPending(1);
            }
            rebuild();
            if (wasSave) finishSave(saveOk, saveOk ? QString() : saveError);  // after rebuild: the dialog sees final state
        }
    }

    // ---- path selection ---------------------------------------------------

    static QString cleanInput(QString p) {
        p = p.trimmed();
        if (p.size() >= 2 && p.startsWith('"') && p.endsWith('"')) p = p.mid(1, p.size() - 2);
        return p;
    }

    void updatePlaybackActions() {
        if (!playLeftAction_) return;
#ifdef OPENBC_HAVE_QT_MULTIMEDIA
        playLeftAction_->setEnabled(!leftPath_.isEmpty());
        playRightAction_->setEnabled(!rightPath_.isEmpty());
        playBothAction_->setEnabled(!leftPath_.isEmpty() && !rightPath_.isEmpty());
        playLeftAction_->setToolTip("Play / pause the left file (uses the system's media decoders)");
        playRightAction_->setToolTip("Play / pause the right file (uses the system's media decoders)");
        playBothAction_->setToolTip("Restart and play both files together (use the crossfade slider to switch between them)");
#else
        playLeftAction_->setEnabled(false);
        playRightAction_->setEnabled(false);
        playBothAction_->setEnabled(false);
        playLeftAction_->setToolTip("Playback requires Qt Multimedia");
        playRightAction_->setToolTip("Playback requires Qt Multimedia");
        playBothAction_->setToolTip("Playback requires Qt Multimedia");
#endif
    }

    // "Reload Latest" after an external change: remote copies are downloaded again and both panes re-pointed.
    void reloadLatest() {
        QString local, error;
        for (std::uint8_t side = 0; side < 2; ++side) {
            const QString& path = side == 0 ? leftPath_ : rightPath_;
            if (path.isEmpty()) continue;
            if (!openbc_remote::resolveLocalPath(path, &local, &error, /*refresh=*/true)) { summary_->setText(error); return; }
            pane(side)->setPath(local);
        }
        updatePlaybackActions();
        startSession();
    }

    void commitPath(std::uint8_t side, const QString& raw) {
        QString& current = side == 0 ? leftPath_ : rightPath_;
        PathSelector* selector = side == 0 ? leftSelector_ : rightSelector_;
        const QString path = cleanInput(raw);
        if (path == current) return;
        QString localPath, resolveError;
        if (!path.isEmpty() && !openbc_remote::resolveLocalPath(path, &localPath, &resolveError)) {
            summary_->setText(resolveError);
            selector->setText(current);
            return;
        }
        if (!confirmDiscard()) {
            selector->setText(current);
            return;
        }
        current = path;
        selector->setText(path);
        if (!path.isEmpty()) selector->remember(path);
        pane(side)->setPath(openbc_remote::localCopy(path));
        updatePlaybackActions();
        reloadAction_->setEnabled(!leftPath_.isEmpty() || !rightPath_.isEmpty());
        if (leftPath_.isEmpty() && rightPath_.isEmpty()) showEmptyState();
        else startSession();
    }

    // One dialog for both sides: Save per side (only for a side with changes), Continue, Cancel.
    bool confirmDiscard() {
        commitEditors();
        if (!hasUnsavedChanges()) return true;
        return confirmUnsavedChanges(this, {leftPath_, changeCount(0)}, {rightPath_, changeCount(1)},
                                     [this](bool left, SaveDone done) { saveSide(left ? 0 : 1, std::move(done)); });
    }

    int changeCount(std::uint8_t side) const { return edits_[side].size() + (coverMode_[side] != CoverKeep ? 1 : 0); }

    // A cell editor that is still open has not reached itemChanged yet: leaving it commits the text.
    void commitEditors() {
        QWidget* focus = QApplication::focusWidget();
        if (focus && focus != tree_ && tree_->isAncestorOf(focus)) tree_->setFocus();
    }

    // Ctrl+S: the side of the current row's column, or the player pane that last had focus.
    void saveActiveSide() {
        commitEditors();
        const std::uint8_t side = static_cast<std::uint8_t>(activeSide_);
        const QString name = side == 0 ? "left" : "right";
        if (!dirty(side)) {
            summary_->setText("Nothing to save on the " + name + " side.");
            return;
        }
        saveSide(side);
    }

    // ---- backend string helpers (pointer is fetched BEFORE the length is read) ----

    QString text(std::size_t row, std::uint8_t field) const {
        std::size_t length = 0;
        const auto* p = openbc_media_row_text(view_.get(), row, field, &length);
        return backendString(p, length);
    }

    bool hasText(std::size_t row, std::uint8_t field) const {
        std::size_t length = 0;  // do not pass nullptr: the C side may write the length
        return openbc_media_row_text(view_.get(), row, field, &length) != nullptr;
    }

    QString sideError(std::uint8_t side) const {
        if (!view_) return {};
        std::size_t length = 0;
        const auto* p = openbc_media_side_error(view_.get(), side, &length);
        return backendString(p, length);
    }

    QPixmap sideCover(std::uint8_t side) const {
        if (!view_) return {};
        std::size_t length = 0;
        const auto* p = openbc_media_side_cover(view_.get(), side, &length);
        QPixmap pixmap;
        if (p && length) pixmap.loadFromData(reinterpret_cast<const uchar*>(p), static_cast<uint>(length));
        return pixmap;
    }

    static AudioWaveform loadAudioWaveform(const OpenBcMediaView* view, std::uint8_t side) {
        AudioWaveform waveform;
        waveform.frameCount = openbc_media_audio_frame_count(view, side);
        waveform.sampleRate = openbc_media_audio_sample_rate(view, side);
        const std::size_t levelCount = openbc_media_audio_level_count(view, side);
        waveform.levels.reserve(static_cast<qsizetype>(levelCount));
        for (std::size_t levelIndex = 0; levelIndex < levelCount; ++levelIndex) {
            AudioPeakLevel level;
            level.framesPerBin = openbc_media_audio_level_frames_per_bin(view, side, levelIndex);
            const std::size_t binCount = openbc_media_audio_bin_count(view, side, levelIndex);
            const OpenBcAudioPeak* bins = openbc_media_audio_level_data(view, side, levelIndex);
            if (binCount != 0 && !bins) return {};
            level.bins.reserve(static_cast<qsizetype>(binCount));
            for (std::size_t bin = 0; bin < binCount; ++bin) {
                level.bins.append({bins[bin].minimum, bins[bin].maximum});
            }
            waveform.levels.append(std::move(level));
        }
        return waveform;
    }

    void updateAudioVisualization() {
        const OpenBcMediaView* mediaView = view_.get();
        if (openbc_media_audio_present(mediaView)) {
            AudioWaveform left = loadAudioWaveform(mediaView, 0);
            AudioWaveform right = loadAudioWaveform(mediaView, 1);
            AudioWaveform delta = loadAudioWaveform(mediaView, 2);
            QString error;
            if (audioVisualizer_->setWaveforms(std::move(left), std::move(right), std::move(delta), &error)) {
                audioStatus_->hide();
                audioVisualizer_->show();
                return;
            }
            audioStatus_->setText("Audio visualization unavailable: " + error);
        } else {
            std::size_t length = 0;
            const auto* error = openbc_media_audio_error(mediaView, &length);
            const QString message = backendString(error, length);
            if (message.isEmpty()) {
                audioVisualizer_->hide();
                audioStatus_->hide();
                return;
            }
            audioStatus_->setText("Audio visualization unavailable: " + message);
        }
        audioVisualizer_->hide();
        audioStatus_->show();
    }

    MediaPlayerPane* pane(std::uint8_t side) const { return side == 0 ? leftPane_ : rightPane_; }

    // ---- pending edits ------------------------------------------------------

    bool dirty(std::uint8_t side) const { return !edits_[side].isEmpty() || coverMode_[side] != CoverKeep; }

    void clearPending(std::uint8_t side) {
        edits_[side].clear();
        coverMode_[side] = CoverKeep;
        coverBytes_[side].clear();
    }

    Cell cellFor(const QString& key, std::uint8_t side, bool had, const QString& orig) const {
        const auto it = edits_[side].constFind(key);
        if (it != edits_[side].constEnd()) return it->isNull() ? Cell{false, {}, true} : Cell{true, it->toString(), true};
        return Cell{had, had ? orig : QString(), false};
    }

    static bool rowDiffers(const Cell& l, const Cell& r, bool backendDiffers, bool informational) {
        if (informational) return false;
        if (!l.pending && !r.pending) return backendDiffers;
        return l.shown != r.shown || l.text != r.text;
    }

    void showCover(std::uint8_t side) {
        QPixmap pixmap;
        if (coverMode_[side] == CoverReplace) pixmap.loadFromData(coverBytes_[side]);
        else if (coverMode_[side] == CoverKeep) pixmap = sideCover(side);
        pane(side)->setCover(pixmap);
        // The compact audio-only height cap depends on whether a cover is visible; the pane's
        // layout updates asynchronously, so re-evaluate once it has settled.
        QTimer::singleShot(0, this, [this]() { updateTopHeight(); });
    }

    void setPendingCover(std::uint8_t side, CoverMode mode, const QByteArray& bytes = QByteArray()) {
        coverMode_[side] = mode;
        coverBytes_[side] = bytes;
        showCover(side);
        updateSaveActions();
    }

    void updateSaveActions() {
        saveLeftAction_->setEnabled(session_ && dirty(0));
        saveRightAction_->setEnabled(session_ && dirty(1));
        // Blue save button next to each path while that side has edits - the same cue text compare gives.
        static const QColor kDirtyBlue(0x4e, 0x9c, 0xff);
        static const QColor kCleanInk(0xdd, 0xe0, 0xee);
        for (std::uint8_t s = 0; s < 2; ++s) {
            if (!saveBtn_[s]) continue;
            const bool d = dirty(s);
            saveBtn_[s]->setEnabled(session_ && d);
            saveBtn_[s]->setIcon(icons::saveIcon(d ? kDirtyBlue : kCleanInk));
            saveBtn_[s]->setProperty("dirty", d);
            saveBtn_[s]->setToolTip(d ? "Unsaved changes on this side - click or Ctrl+S to write to disk"
                                      : (s == 0 ? "Save left file (Ctrl+S while this side is active)"
                                                : "Save right file (Ctrl+S while this side is active)"));
            saveBtn_[s]->style()->unpolish(saveBtn_[s]);
            saveBtn_[s]->style()->polish(saveBtn_[s]);
        }
        const int pending = changeCount(0) + changeCount(1);
        pendingLabel_->setText(pending ? QString("%1 unsaved change(s)").arg(pending) : QString());
    }

    // Writes a cell + row styling from the (pending-aware) cells.
    void applyItem(QTreeWidgetItem* item, const Cell& l, const Cell& r, bool differs) {
        using namespace media_detail;
        const QString dash = QStringLiteral("-");
        const QBrush differBrush(QColor(0xe5, 0x53, 0x4b, 56));
        const QBrush editedBrush(QColor(40, 170, 90, 170));  // same tint text compare paints on edited lines
        const Cell* cells[2] = {&l, &r};
        item->setText(1, l.shown ? l.text : dash);
        item->setText(2, r.shown ? r.text : dash);
        item->setToolTip(1, item->text(1));
        item->setToolTip(2, item->text(2));
        for (int c = 0; c < 2; ++c) {
            item->setData(c + 1, kShownRole, cells[c]->shown);
            QFont f = tree_->font();
            f.setBold(differs);
            item->setFont(c + 1, f);
            item->setForeground(c + 1, QBrush());
        }
        QFont key = tree_->font();
        key.setBold(differs);
        item->setFont(0, key);
        for (int c = 0; c < 3; ++c) item->setBackground(c, differs ? differBrush : QBrush());
        for (int c = 0; c < 2; ++c)
            if (cells[c]->pending) item->setBackground(c + 1, editedBrush);  // edited side's cell: green
    }

    void onItemChanged(QTreeWidgetItem* item, int col) {
        using namespace media_detail;
        if (populating_ || (col != 1 && col != 2)) return;
        const auto side = static_cast<std::uint8_t>(col - 1);
        const QString k = item->data(0, kKeyRole).toString();
        if (k.isEmpty()) return;
        const bool had = item->data(col, kHadRole).toBool();
        const QString orig = item->data(col, kOrigRole).toString();
        const QString now = item->text(col);
        const bool blank = now.isEmpty() || now == "-";
        if (!had && blank) edits_[side].remove(k);
        else if (had && now == orig) edits_[side].remove(k);
        else if (blank) edits_[side].insert(k, QVariant());  // remove the tag
        else edits_[side].insert(k, now);

        populating_ = true;
        const Cell l = cellFor(k, 0, item->data(1, kHadRole).toBool(), item->data(1, kOrigRole).toString());
        const Cell r = cellFor(k, 1, item->data(2, kHadRole).toBool(), item->data(2, kOrigRole).toString());
        applyItem(item, l, r, rowDiffers(l, r, item->data(0, kBackendDiffRole).toBool(), item->data(0, kInfoRole).toBool()));
        populating_ = false;
        updateSaveActions();
    }

    // ---- saving ---------------------------------------------------------------

    // `done` (dialog / Ctrl+S) is told once the backend has rewritten the file and re-read it.
    void saveSide(std::uint8_t side, SaveDone done = {}) {
        if (!dirty(side)) {
            if (done) done(true, {});
            return;
        }
        if (!session_) {
            if (done) done(false, "No open comparison.");
            return;
        }
        std::vector<QByteArray> groups, keys, values;
        std::vector<const char*> groupPtrs, keyPtrs, valuePtrs;
        for (auto it = edits_[side].cbegin(); it != edits_[side].cend(); ++it) {
            const QStringList parts = it.key().split(QChar(0x1f));
            groups.push_back(parts.value(0).toUtf8());
            keys.push_back(parts.value(1).toUtf8());
            values.push_back(it->isNull() ? QByteArray() : it->toString().toUtf8());
        }
        std::size_t i = 0;
        for (auto it = edits_[side].cbegin(); it != edits_[side].cend(); ++it, ++i) {
            groupPtrs.push_back(groups[i].constData());
            keyPtrs.push_back(keys[i].constData());
            valuePtrs.push_back(it->isNull() ? nullptr : values[i].constData());
        }
        const QByteArray& cover = coverBytes_[side];

        pane(side)->release();  // the player must not hold the file open while it is rewritten
        previewed_[side] = false;
        savingSide_ = side;
        saveDone_ = std::move(done);
        summary_->setText("Saving " + QFileInfo(side == 0 ? leftPath_ : rightPath_).fileName() + "...");
        const std::uint8_t accepted = openbc_media_save(
            session_, side, groupPtrs.size(), groupPtrs.data(), keyPtrs.data(), valuePtrs.data(),
            static_cast<std::uint8_t>(coverMode_[side]), reinterpret_cast<const std::uint8_t*>(cover.constData()),
            static_cast<std::size_t>(cover.size()));
        if (!accepted) {
            savingSide_ = -1;
            summary_->setText("Could not start saving (session busy or closed)");
            finishSave(false, "Could not start saving (session busy or closed).");
        }
    }

    void finishSave(bool ok, const QString& error) {
        SaveDone done;
        std::swap(done, saveDone_);
        if (done) done(ok, error);
    }

    // ---- table ------------------------------------------------------------------

    void rebuild() {
        using namespace media_detail;
        populating_ = true;
        tree_->clear();
        plain_.clear();
        const OpenBcMediaView* v = view_.get();
        QStringList problems;
        for (std::uint8_t side = 0; side < 2; ++side) {
            const QString error = sideError(side);
            if (!error.isEmpty()) problems << error;
        }
        errors_->setText(problems.join("\n"));
        errors_->setVisible(!problems.isEmpty());

        const std::size_t rows = openbc_media_view_rows(v);
        // A side is editable when the backend flagged any row editable and the file is present.
        sideEditable_[0] = sideEditable_[1] = false;
        bool anyEditable = false;
        for (std::size_t i = 0; i < rows; ++i) anyEditable |= (openbc_media_row_flags(v, i) & 4) != 0;
        for (std::uint8_t s = 0; s < 2; ++s) {
            sideEditable_[s] = anyEditable && openbc_media_side_present(v, s) != 0;
            pane(s)->setCoverEditable(sideEditable_[s]);
            showCover(s);
        }

        const bool onlyDifferences = onlyDifferences_->isChecked();
        int compared = 0, differing = 0;
        bool hasVideo[2] = {false, false};
        QStringList lines{"Property\tLeft\tRight"};
        QTreeWidgetItem* top = nullptr;
        QString currentGroup;
        for (std::size_t i = 0; i < rows; ++i) {
            const std::uint32_t flags = openbc_media_row_flags(v, i);
            const bool backendDiffers = flags & 1, informational = flags & 2, editable = flags & 4;
            const QString group = text(i, 0), key = text(i, 1);
            const bool hasLeft = hasText(i, 2);
            const bool hasRight = hasText(i, 3);
            if (group == "Video") {
                hasVideo[0] |= hasLeft;
                hasVideo[1] |= hasRight;
            }
            const QString origL = hasLeft ? text(i, 2) : QString(), origR = hasRight ? text(i, 3) : QString();
            const QString k = editKey(group, key);
            const Cell l = cellFor(k, 0, hasLeft, origL), r = cellFor(k, 1, hasRight, origR);
            const bool differs = rowDiffers(l, r, backendDiffers, informational);
            if (!informational) {
                ++compared;
                differing += differs;
            }
            if (onlyDifferences && !(differs && !informational)) continue;
            if (!top || group != currentGroup) {
                top = new QTreeWidgetItem(tree_, {group});
                QFont bold = top->font(0);
                bold.setBold(true);
                top->setFont(0, bold);
                top->setFirstColumnSpanned(true);
                currentGroup = group;
            }
            auto* item = new QTreeWidgetItem(top, {key, QString(), QString()});
            item->setData(0, kKeyRole, k);
            item->setData(0, kEditableRole, editable);
            item->setData(0, kBackendDiffRole, backendDiffers);
            item->setData(0, kInfoRole, informational);
            item->setData(1, kOrigRole, origL);
            item->setData(1, kHadRole, hasLeft);
            item->setData(2, kOrigRole, origR);
            item->setData(2, kHadRole, hasRight);
            if (editable) item->setFlags(item->flags() | Qt::ItemIsEditable);
            applyItem(item, l, r, differs);
            lines << key + '\t' + item->text(1) + '\t' + item->text(2);
        }
        tree_->expandAll();  // after children exist (setExpanded on an empty item does not stick)
        populating_ = false;

        plain_ = lines.join('\n');
        summary_->setText(rows == 0 ? QString("No metadata") : compared == 0 ? QString("Metadata") : QString("%1 of %2 properties differ").arg(differing).arg(compared));
        if (anyEditable) summary_->setText(summary_->text() + "  -  double-click a Left/Right value to edit");

        // Show the first video frame as a thumbnail (once per side per load, not on every filter toggle).
        for (std::uint8_t s = 0; s < 2; ++s) {
            if (previewed_[s]) continue;
            previewed_[s] = true;
            if (hasVideo[s]) pane(s)->preview();
        }
        updateSaveActions();
        if (onTitleChanged) onTitleChanged();
    }

    // ---- volume / crossfade -------------------------------------------------------

    void applyVolumes() {
        audioVisualizer_->setPaused(!leftPane_->playing() && !rightPane_->playing());
        if (!master_ || !crossfade_) return;
#ifdef OPENBC_HAVE_QT_MULTIMEDIA
        const double master = muteButton_->isChecked()
                                  ? 0.0
                                  : QAudio::convertVolume(master_->value() / 100.0, QAudio::LogarithmicVolumeScale, QAudio::LinearVolumeScale);
#else
        const double master = muteButton_->isChecked() ? 0.0 : master_->value() / 100.0;
#endif
        // Equal-power crossfade: 0 = Left only, 1000 = Right only, centre = both at -3 dB.
        const double t = crossfade_->value() / 1000.0;
        const double gl = std::cos(t * media_detail::kPi / 2.0), gr = std::sin(t * media_detail::kPi / 2.0);
        // The crossfade only shapes the sound while BOTH sides play; a lone side stays at full master volume.
        const bool both = leftPane_->playing() && rightPane_->playing();
        leftPane_->setVolume(master * (both ? gl : 1.0));
        rightPane_->setVolume(master * (both ? gr : 1.0));
        masterLabel_->setText(QString("%1%").arg(master_->value()));
        crossfadeLabel_->setText(QString("L %1%  R %2%%3").arg(qRound(gl * 100)).arg(qRound(gr * 100)).arg(both ? "" : "  (when both play)"));
    }

    // ---- UI ---------------------------------------------------------------------------

    QToolBar* makeToolbar() {
        using icons::Glyph;
        auto* bar = new QToolBar(this);
        bar->setObjectName("textCompareToolbar");
        bar->setMovable(false);
        bar->setFloatable(false);
        bar->setIconSize(QSize(20, 20));
        bar->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        homeAction_ = bar->addAction(icons::glyph(Glyph::Home), "Home");
        sessionsAction_ = bar->addAction(icons::glyph(Glyph::Sessions), "Sessions");
        bar->addSeparator();
        playLeftAction_ = bar->addAction(icons::glyph(Glyph::Play), "Play Left");
        playRightAction_ = bar->addAction(icons::glyph(Glyph::Play), "Play Right");
        playBothAction_ = bar->addAction(icons::glyph(Glyph::Play), "Play Both");
        updatePlaybackActions();
        bar->addSeparator();
        saveLeftAction_ = bar->addAction(icons::glyph(Glyph::Save), "Save Left");
        saveRightAction_ = bar->addAction(icons::glyph(Glyph::Save), "Save Right");
        saveLeftAction_->setToolTip("Write the edited tags / cover of the left file");
        saveRightAction_->setToolTip("Write the edited tags / cover of the right file");
        saveLeftAction_->setEnabled(false);
        saveRightAction_->setEnabled(false);
        // Ctrl+S saves the active side; scoped to this tab (and its children) like text compare's save action.
        saveAction_ = new QAction(icons::glyph(Glyph::Save), "Save", this);
        saveAction_->setShortcut(QKeySequence::Save);
        saveAction_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        saveAction_->setToolTip("Save the active side (the side of the current row's column)");
        addAction(saveAction_);
        bar->addSeparator();
        reloadAction_ = bar->addAction(icons::glyph(Glyph::Refresh), "Reload");
        reloadAction_->setEnabled(!leftPath_.isEmpty() || !rightPath_.isEmpty());
        return bar;
    }

    PathSelector* makeSelector(const QString& side, const QString& initial, std::uint8_t sideIndex) {
        // Remote picker enabled like the text viewer; remote files are downloaded to a temp copy for playback/backend.
        return openbc_remote::makeRemoteFileSelector(
            this, side, initial,
            "Media files (*.mp3 *.wav *.flac *.ogg *.oga *.opus *.m4a *.aac *.wma *.mp4 *.m4v *.mov *.avi *.mkv *.webm);;All files (*.*)",
            [this, sideIndex](QString p) { commitPath(sideIndex, p); });
    }

    QHBoxLayout* makeMixerStrip() {
#ifndef OPENBC_HAVE_QT_MULTIMEDIA
        return new QHBoxLayout;
#else
        using icons::Glyph;
        auto* mix = new QHBoxLayout;
        mix->setContentsMargins(6, 2, 6, 2);

        muteButton_ = new QToolButton(this);
        muteButton_->setCheckable(true);
        muteButton_->setIcon(icons::glyph(Glyph::VolumeOn));
        muteButton_->setToolTip("Mute both");
        master_ = new QSlider(Qt::Horizontal, this);
        master_->setRange(0, 100);
        master_->setValue(100);
        master_->setMaximumWidth(180);
        master_->setToolTip("Master volume (applies to both sides)");
        masterLabel_ = new QLabel("100%", this);
        masterLabel_->setMinimumWidth(36);

        auto* leftLabel = new QLabel("Left", this);
        auto* rightLabel = new QLabel("Right", this);
        crossfade_ = new QSlider(Qt::Horizontal, this);
        crossfade_->setRange(0, 1000);
        crossfade_->setValue(500);
        crossfade_->setTickPosition(QSlider::TicksBelow);
        crossfade_->setTickInterval(250);
        crossfade_->setToolTip("A/B crossfade while both files play: drag left for Left only, right for Right only");
        crossfadeLabel_ = new QLabel(this);
        centerButton_ = new QToolButton(this);
        centerButton_->setText("=");
        centerButton_->setToolTip("Centre the crossfade (both at equal level)");
        linkSeek_ = new QCheckBox("Link seek", this);
        linkSeek_->setChecked(true);
        linkSeek_->setToolTip("Dragging one position slider moves the other side to the same time");

        mix->addWidget(muteButton_);
        mix->addWidget(master_);
        mix->addWidget(masterLabel_);
        mix->addSpacing(16);
        mix->addWidget(leftLabel);
        mix->addWidget(crossfade_, 1);
        mix->addWidget(rightLabel);
        mix->addWidget(centerButton_);
        mix->addWidget(crossfadeLabel_);
        mix->addSpacing(8);
        mix->addWidget(linkSeek_);

        QObject::connect(master_, &QSlider::valueChanged, this, [this](int) { applyVolumes(); });
        QObject::connect(crossfade_, &QSlider::valueChanged, this, [this](int) { applyVolumes(); });
        QObject::connect(centerButton_, &QToolButton::clicked, this, [this]() { crossfade_->setValue(500); });
        QObject::connect(muteButton_, &QToolButton::toggled, this, [this](bool muted) {
            muteButton_->setIcon(icons::glyph(muted ? icons::Glyph::VolumeMuted : icons::Glyph::VolumeOn));
            applyVolumes();
        });
        return mix;
#endif
    }

    void updateTopHeight() {
        const bool anyVideo = leftPane_->hasVideo() || rightPane_->hasVideo();
        if (!anyVideo) {
            // Force pending layout work so minimumSizeHint() includes a just-shown cover.
            for (QWidget* p : {static_cast<QWidget*>(leftPane_), static_cast<QWidget*>(rightPane_)})
                if (p->layout()) p->layout()->activate();
            top_->layout()->activate();
            top_->setMaximumHeight(top_->minimumSizeHint().height() + 40);
            return;
        }
        const bool wasCapped = top_->maximumHeight() != QWIDGETSIZE_MAX;
        top_->setMaximumHeight(QWIDGETSIZE_MAX);
        // First video after a compact (audio-only) state: open the video area to a usable size.
        if (wasCapped) applyDefaultSplit();
    }

    // Sets the video/table split once the splitter has a real height (deferred to the event loop,
    // because the video signal can fire before the first layout pass).
    void applyDefaultSplit() {
        QTimer::singleShot(0, this, [this]() {
            if (!split_) return;
            const int total = split_->height();
            if (total < 300) return;  // not laid out yet; showEvent / the next video change retries
            const int topH = total * 3 / 5;
            split_->setSizes({topH, total - topH});
        });
    }

    void buildUi() {
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(0, 0, 0, 0);
        root->setSpacing(0);
        root->addWidget(makeToolbar());

        auto* paths = new QHBoxLayout;
        paths->setContentsMargins(6, 2, 6, 2);
        leftSelector_ = makeSelector("Left", leftPath_, 0);
        rightSelector_ = makeSelector("Right", rightPath_, 1);
        for (std::uint8_t side = 0; side < 2; ++side) {
            saveBtn_[side] = new QToolButton(this);
            saveBtn_[side]->setObjectName("saveSideBtn");  // qt_style.h paints it blue while property dirty=true
            saveBtn_[side]->setIcon(icons::saveIcon(QColor(0xdd, 0xe0, 0xee)));
            saveBtn_[side]->setIconSize(QSize(15, 15));
            saveBtn_[side]->setCursor(Qt::PointingHandCursor);
            saveBtn_[side]->setEnabled(false);
            saveBtn_[side]->setToolTip(side == 0 ? "Save left file (Ctrl+S while this side is active)"
                                                 : "Save right file (Ctrl+S while this side is active)");
            QObject::connect(saveBtn_[side], &QToolButton::clicked, this, [this, side]() { commitEditors(); saveSide(side); });
        }
        paths->addWidget(leftSelector_, 1);
        paths->addWidget(saveBtn_[0]);
        paths->addWidget(rightSelector_, 1);
        paths->addWidget(saveBtn_[1]);
        root->addLayout(paths);

        // ---- top half: preview players + mixer strip -----------------------
        top_ = new QWidget(this);
        auto* topLayout = new QVBoxLayout(top_);
        topLayout->setContentsMargins(0, 0, 0, 0);
        topLayout->setSpacing(0);

        auto* players = new QHBoxLayout;
        players->setContentsMargins(0, 0, 0, 0);
        leftPane_ = new MediaPlayerPane(leftPath_, this);
        rightPane_ = new MediaPlayerPane(rightPath_, this);
        leftPane_->setVisible(!leftPath_.isEmpty());
        rightPane_->setVisible(!rightPath_.isEmpty());
        players->addWidget(leftPane_, 1);
        players->addWidget(rightPane_, 1);
        topLayout->addLayout(players, 1);
        topLayout->addLayout(makeMixerStrip());

        // ---- bottom half: errors + header + metadata table ------------------
        auto* bottom = new QWidget(this);
        auto* bottomLayout = new QVBoxLayout(bottom);
        bottomLayout->setContentsMargins(0, 0, 0, 0);
        bottomLayout->setSpacing(0);

        errors_ = new QLabel(this);
        errors_->setStyleSheet("color:#e5534b;");
        errors_->setWordWrap(true);
        errors_->setContentsMargins(6, 0, 6, 0);
        errors_->hide();
        bottomLayout->addWidget(errors_);

        audioStatus_ = new QLabel(this);
        audioStatus_->setStyleSheet("color:#e5a84b;");
        audioStatus_->setWordWrap(true);
        audioStatus_->setContentsMargins(6, 2, 6, 2);
        audioStatus_->hide();
        bottomLayout->addWidget(audioStatus_);

        audioVisualizer_ = new AudioDiffVisualizer(this);
        audioVisualizer_->setObjectName("audioDiffVisualizer");
        audioVisualizer_->hide();
        bottomLayout->addWidget(audioVisualizer_);

        auto* header = new QHBoxLayout;
        header->setContentsMargins(6, 2, 6, 2);
        summary_ = new QLabel("Metadata", this);
        pendingLabel_ = new QLabel(this);
        pendingLabel_->setStyleSheet("color:#4e9cff;");  // blue = needs saving, like the save buttons
        onlyDifferences_ = new QCheckBox("Differences only", this);
        auto* copy = new QPushButton("Copy", this);
        copy->setToolTip("Copy all rows to the clipboard (tab separated)");
        header->addWidget(summary_, 1);
        header->addWidget(pendingLabel_);
        header->addWidget(onlyDifferences_);
        header->addWidget(copy);
        bottomLayout->addLayout(header);

        tree_ = new QTreeWidget(this);
        tree_->setColumnCount(3);
        tree_->setHeaderLabels({"Property", "Left", "Right"});
        tree_->setUniformRowHeights(true);
        tree_->setAlternatingRowColors(true);
        tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
        tree_->setTextElideMode(Qt::ElideMiddle);
        tree_->setEditTriggers(QAbstractItemView::NoEditTriggers);  // editing starts from itemDoubleClicked only
        tree_->setItemDelegateForColumn(1, new media_detail::DashAwareDelegate(tree_));
        tree_->setItemDelegateForColumn(2, new media_detail::DashAwareDelegate(tree_));
        tree_->header()->setStretchLastSection(false);
        tree_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        tree_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
        tree_->header()->setSectionResizeMode(2, QHeaderView::Stretch);
        tree_->setMinimumHeight(120);
        bottomLayout->addWidget(tree_, 1);

        // ---- the draggable divider ------------------------------------------
        split_ = new QSplitter(Qt::Vertical, this);
        split_->setChildrenCollapsible(false);
        split_->setHandleWidth(8);
        // Widget-level style beats the app-wide "QSplitter::handle:vertical" rule in qt_style.h
        // (#2d2d2d, 4px), which is nearly invisible on the dark theme.
        split_->setStyleSheet(
            "QSplitter::handle:vertical { background:#5a5a5a; margin:3px 0; border-radius:1px; }"
            "QSplitter::handle:vertical:hover, QSplitter::handle:vertical:pressed { background:#4a9eff; }");
        split_->addWidget(top_);
        split_->addWidget(bottom);
        split_->setStretchFactor(0, 3);  // window growth goes mostly to the video
        split_->setStretchFactor(1, 2);
        split_->handle(1)->setCursor(Qt::SplitVCursor);
        split_->handle(1)->setToolTip("Drag to resize the players / metadata table");
        root->addWidget(split_, 1);

        // Audio-only files have nothing to enlarge: keep the top half compact and
        // give all spare height to the table. Re-evaluated when a video appears.
        leftPane_->onVideoChanged = [this]() { updateTopHeight(); };
        rightPane_->onVideoChanged = [this]() { updateTopHeight(); };
        updateTopHeight();

        QObject::connect(onlyDifferences_, &QCheckBox::toggled, this, [this](bool) { if (view_) rebuild(); });
        QObject::connect(copy, &QPushButton::clicked, this, [this]() { QGuiApplication::clipboard()->setText(plain_); });
        QObject::connect(homeAction_, &QAction::triggered, this, [this]() { if (onHomeRequested) onHomeRequested(); });
        QObject::connect(sessionsAction_, &QAction::triggered, this, [this]() { if (onSessionsRequested) onSessionsRequested(); });
        QObject::connect(reloadAction_, &QAction::triggered, this, [this]() {
            if (!session_ || !confirmDiscard()) return;
            previewed_[0] = previewed_[1] = false;
            clearPending(0);
            clearPending(1);
            leftPane_->reset();
            rightPane_->reset();
            openbc_media_reload(session_);
        });

        // Editing.
        QObject::connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int col) {
            if (col != 1 && col != 2) return;
            if (!item->data(0, media_detail::kEditableRole).toBool() || !sideEditable_[col - 1]) return;
            tree_->editItem(item, col);
        });
        QObject::connect(tree_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int col) { onItemChanged(item, col); });
        QObject::connect(saveLeftAction_, &QAction::triggered, this, [this]() { commitEditors(); saveSide(0); });
        QObject::connect(saveRightAction_, &QAction::triggered, this, [this]() { commitEditors(); saveSide(1); });
        QObject::connect(saveAction_, &QAction::triggered, this, [this]() { saveActiveSide(); });

        // Ctrl+S target: the column of the row being worked on, else the player pane that last had focus.
        QObject::connect(tree_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem*, int col) {
            if (col == 1 || col == 2) activeSide_ = col - 1;
        });
        QObject::connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem*, QTreeWidgetItem*) {
            const int col = tree_->currentColumn();
            if (col == 1 || col == 2) activeSide_ = col - 1;
        });
        QObject::connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) {
            if (!now) return;
            if (leftPane_ == now || leftPane_->isAncestorOf(now) || leftSelector_ == now || leftSelector_->isAncestorOf(now)) activeSide_ = 0;
            else if (rightPane_ == now || rightPane_->isAncestorOf(now) || rightSelector_ == now || rightSelector_->isAncestorOf(now)) activeSide_ = 1;
        });
        leftPane_->onCoverReplaced = [this](const QByteArray& bytes) { setPendingCover(0, CoverReplace, bytes); };
        rightPane_->onCoverReplaced = [this](const QByteArray& bytes) { setPendingCover(1, CoverReplace, bytes); };
        leftPane_->onCoverRemoved = [this]() { setPendingCover(0, CoverRemove); };
        rightPane_->onCoverRemoved = [this]() { setPendingCover(1, CoverRemove); };

        // Playback: volume gains depend on which sides are playing; seeks can be mirrored.
        leftPane_->onStateChanged = [this]() { applyVolumes(); };
        rightPane_->onStateChanged = [this]() { applyVolumes(); };
        leftPane_->onSeeked = [this](qint64 ms) { if (linkSeek_->isChecked()) rightPane_->seek(ms); };
        rightPane_->onSeeked = [this](qint64 ms) { if (linkSeek_->isChecked()) leftPane_->seek(ms); };
        audioVisualizer_->onSeekRequested = [this](qint64 ms) {
            leftPane_->seek(ms);
            if (linkSeek_->isChecked()) rightPane_->seek(ms);
        };
        leftPane_->onPositionChanged = [this](qint64 ms) {
            if (leftPane_->playing() || !rightPane_->playing()) audioVisualizer_->setPlaybackPosition(ms);
        };
        rightPane_->onPositionChanged = [this](qint64 ms) {
            if (!leftPane_->playing()) audioVisualizer_->setPlaybackPosition(ms);
        };
        applyVolumes();

        // Play Left / Right: toggle that side and pause the other so two files never overlap by accident.
        // (The per-pane Play buttons are independent, which is how a deliberate A/B crossfade is started.)
        QObject::connect(playLeftAction_, &QAction::triggered, this, [this]() {
            if (leftPane_->playing()) {
                leftPane_->pause();
            } else {
                rightPane_->pause();
                leftPane_->play();
            }
        });
        QObject::connect(playRightAction_, &QAction::triggered, this, [this]() {
            if (rightPane_->playing()) {
                rightPane_->pause();
            } else {
                leftPane_->pause();
                rightPane_->play();
            }
        });
        // Play Both: toggle; starting restarts both from 0 so they stay roughly in step.
        QObject::connect(playBothAction_, &QAction::triggered, this, [this]() {
            if (leftPane_->playing() || rightPane_->playing()) {
                leftPane_->pause();
                rightPane_->pause();
                return;
            }
            leftPane_->rewind();
            rightPane_->rewind();
            leftPane_->play();
            rightPane_->play();
        });
    }

    QString leftPath_;
    QString rightPath_;
    OpenBcMediaSession* session_ = nullptr;
    MediaViewPtr view_;
    QString plain_;
    bool previewed_[2] = {false, false};
    bool populating_ = false;
    bool sideEditable_[2] = {false, false};
    int savingSide_ = -1;
    int activeSide_ = 0;     // side of the current column / last focused pane; target of Ctrl+S
    SaveDone saveDone_;      // completion of the save currently in flight (dialog / Ctrl+S)
    Edits edits_[2];
    CoverMode coverMode_[2] = {CoverKeep, CoverKeep};
    QByteArray coverBytes_[2];

    QAction* homeAction_ = nullptr;
    QAction* sessionsAction_ = nullptr;
    QAction* playLeftAction_ = nullptr;
    QAction* playRightAction_ = nullptr;
    QAction* playBothAction_ = nullptr;
    QAction* saveAction_ = nullptr;
    QAction* saveLeftAction_ = nullptr;
    QAction* saveRightAction_ = nullptr;
    QAction* reloadAction_ = nullptr;
    QToolButton* saveBtn_[2] = {nullptr, nullptr};
    PathSelector* leftSelector_ = nullptr;
    std::unique_ptr<FileChangeController> changes_;  // external-change notice + reload (common/file_change_controller.h)
    PathSelector* rightSelector_ = nullptr;
    MediaPlayerPane* leftPane_ = nullptr;
    MediaPlayerPane* rightPane_ = nullptr;
    QToolButton* muteButton_ = nullptr;
    QToolButton* centerButton_ = nullptr;
    QSlider* master_ = nullptr;
    QSlider* crossfade_ = nullptr;
    QLabel* masterLabel_ = nullptr;
    QLabel* crossfadeLabel_ = nullptr;
    QCheckBox* linkSeek_ = nullptr;
    QLabel* summary_ = nullptr;
    QLabel* pendingLabel_ = nullptr;
    QLabel* errors_ = nullptr;
    QLabel* audioStatus_ = nullptr;
    AudioDiffVisualizer* audioVisualizer_ = nullptr;
    QCheckBox* onlyDifferences_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QWidget* top_ = nullptr;
    QSplitter* split_ = nullptr;
    bool splitInit_ = false;
    BackendChannel channel_;  // last member: destroyed after stopSession()
};

}  // namespace openbc::app