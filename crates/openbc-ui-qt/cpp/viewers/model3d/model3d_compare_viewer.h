// model3d_compare_viewer.h
// ---------------------------------------------------------------------------
// 3D Compare tab (separate from picture_compare_viewer.h; same conventions): header-only, no
// Q_OBJECT, plain std::function callbacks, cross-thread delivery via the functor overload of
// QMetaObject::invokeMethod.
//
// View modes (toolbar, exclusive):  2-Up | 1-Up | Split | Overlay | Heatmap | Changes
//   2-Up     A and B side by side, cameras locked          Split    one camera, A left / B right of a handle
//   1-Up     one model (A or B)                            Overlay  A and B translucent, A<->B slider
//   Heatmap  signed deviation colours + histogram legend   Changes  ghost model + removed (red) / added (blue)
// Other toolbar buttons: wireframe, alignment menu (modes, offset, ICP, align parts), swap, reload,
// fit view, side panel, settings, more.
//
// ALL loading, aligning and comparing happens in the Rust backend (openbc-engine -> openbc-compute,
// GPU preferred when eligible, CPU/Rayon otherwise). This widget only pushes option changes
// (latest wins), copies owned events from the Rust thread, draws, and asks for click probes.
// ---------------------------------------------------------------------------
#pragma once

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QCursor>
#include <QDateTime>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QMessageBox>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMetaObject>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QSplitter>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidgetAction>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>

#include "viewers/model3d/model3d_compare_api.h"
#include "viewers/model3d/model3d_extensions.h"
#include "viewers/model3d/model3d_icons.h"
#include "viewers/model3d/model3d_viewport.h"
#include "style/qt_style.h"
#include "common/path_selector.h"
#include "common/remote_selector.h"

namespace openbc::app {

namespace icons = openbc::ui::icons;
namespace m3 = openbc::ui::icons::model3dicons;

struct Model3dOptions {
    float tolerance = 0.05f;    // mm
    float range = 1.0f;         // heat-map +/- mm
    int alignment = 0;          // 0 original, 1 centre, 2 bbox corners, 3 centroids
    QVector3D offset;           // mm, applied to B
    bool preferGpu = true;
    bool computeChanges = true;
};

enum class ViewMode { TwoUp = 0, OneUp, Split, Overlay, Heatmap, Changes };

// ---------------------------------------------------------------------------------------------
// Overlay legends (children of the main viewport)
// ---------------------------------------------------------------------------------------------
class M3HeatLegend : public QWidget {
public:
    explicit M3HeatLegend(QWidget* parent) : QWidget(parent) { setFixedSize(232, 124); setAttribute(Qt::WA_TransparentForMouseEvents); }
    void set(std::vector<float> hist, float range, float tol, float maxDev, float meanDev) {
        hist_ = std::move(hist); range_ = range; tol_ = tol; max_ = maxDev; mean_ = meanDev; update();
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen); p.setBrush(QColor(60, 60, 64, 238)); p.drawRoundedRect(rect(), 8, 8);
        const QRectF plot(12, 10, width() - 24, 38);
        float mx = 1e-20f;
        for (float v : hist_) mx = std::max(mx, v);
        if (!hist_.empty()) {
            const double bw = plot.width() / double(hist_.size());
            p.setBrush(QColor(0xdd, 0xdd, 0xe6, 220));
            for (std::size_t i = 0; i < hist_.size(); ++i) {
                const double h = plot.height() * std::sqrt(double(hist_[i] / mx));   // sqrt: small bins stay visible
                p.drawRect(QRectF(plot.left() + i * bw, plot.bottom() - h, std::max(1.0, bw - 1.0), h));
            }
        }
        const QRectF bar(12, 54, width() - 24, 9);
        QLinearGradient g(bar.left(), 0, bar.right(), 0);
        const double half = std::clamp(double(tol_ / std::max(range_, 1e-6f)), 0.0, 1.0) * 0.5;
        g.setColorAt(0.0, QColor(0x1f, 0x4f, 0xd8)); g.setColorAt(0.5 - half, QColor(0xc4, 0xc4, 0xc4));
        g.setColorAt(0.5 + half, QColor(0xc4, 0xc4, 0xc4)); g.setColorAt(1.0, QColor(0xe0, 0x2a, 0x22));
        p.setBrush(g); p.drawRoundedRect(bar, 3, 3);
        QFont f = font(); f.setPointSizeF(f.pointSizeF() * 0.85); p.setFont(f);
        p.setPen(QColor(0xdd, 0xdd, 0xdd));
        p.drawText(QRectF(12, 66, 60, 14), Qt::AlignLeft, QString::number(-range_, 'f', 2));
        p.drawText(QRectF(width() / 2.0 - 40, 66, 80, 14), Qt::AlignCenter, QString::fromUtf8("\xC2\xB1") + QString::number(tol_, 'g', 3) + " mm");
        p.drawText(QRectF(width() - 72, 66, 60, 14), Qt::AlignRight, QString::number(range_, 'f', 2));
        p.setPen(QColor(0xb8, 0xb8, 0xb8));
        p.drawText(QRectF(12, 82, 100, 14), Qt::AlignLeft, QString("max %1%2 mm").arg(max_ >= range_ ? QString::fromUtf8("\xE2\x89\xA5") : QString()).arg(max_, 0, 'f', 3));
        p.drawText(QRectF(width() - 112, 82, 100, 14), Qt::AlignRight, QString("mean %1 mm").arg(mean_, 0, 'f', 3));
        p.setPen(QColor(0x90, 0x90, 0x90));
        p.drawText(QRectF(12, 102, width() - 24, 14), Qt::AlignLeft, "Click the model to pin a deviation probe");
    }
private:
    std::vector<float> hist_; float range_ = 1, tol_ = 0.05f, max_ = 0, mean_ = 0;
};

class M3ChangesLegend : public QWidget {
public:
    explicit M3ChangesLegend(QWidget* parent) : QWidget(parent) { setFixedSize(176, 56); setAttribute(Qt::WA_TransparentForMouseEvents); }
    void set(double removed, double added, bool truncated) { removed_ = removed; added_ = added; truncated_ = truncated; update(); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen); p.setBrush(QColor(60, 60, 64, 238)); p.drawRoundedRect(rect(), 8, 8);
        p.setBrush(m3::kRed); p.drawEllipse(QPointF(14, 18), 4, 4);
        p.setBrush(m3::kBlue); p.drawEllipse(QPointF(14, 38), 4, 4);
        p.setPen(Qt::white);
        p.drawText(QRectF(26, 8, width() - 30, 20), Qt::AlignVCenter, QString("Removed %1 mm\xC2\xB3").arg(removed_, 0, 'g', 4));
        p.drawText(QRectF(26, 28, width() - 30, 20), Qt::AlignVCenter, QString("Added %1 mm\xC2\xB3").arg(added_, 0, 'g', 4));
        if (truncated_) { p.setPen(QColor(0xf0, 0xb0, 0x40)); p.drawText(QRectF(100, 46, 70, 10), Qt::AlignRight, "partial"); }
    }
private:
    double removed_ = 0, added_ = 0; bool truncated_ = false;
};

// ---------------------------------------------------------------------------------------------
class Model3dCompareViewer : public QWidget {
public:
    std::function<void()> onTitleChanged;
    std::function<void(const QString& left, const QString& right)> onComparisonRequested;

    explicit Model3dCompareViewer(QSettings* preferences, QWidget* parent = nullptr) : QWidget(parent), prefs_(preferences) {
        setAcceptDrops(true);
        setFocusPolicy(Qt::StrongFocus);
        loadPreferences();

        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(0, 0, 0, 0);
        root->setSpacing(0);
        createActions();
        root->addWidget(buildToolbar());
        root->addWidget(buildPathRows());
        root->addWidget(buildBody(), 1);
        root->addWidget(buildBottomBar());
        wireViewports();
        syncControlsFromOptions();
        refreshViews();

        const Openbc3dOptions native = nativeOptions();
        handle_ = openbc_model3d_create(&Model3dCompareViewer::onNativeEvent, this, &native);
        if (handle_) {
            QByteArray name(256, '\0');
            const std::size_t n = openbc_model3d_gpu_name(handle_, reinterpret_cast<std::uint8_t*>(name.data()), std::size_t(name.size()));
            gpuName_ = QString::fromUtf8(name.constData(), qsizetype(std::min<std::size_t>(n, name.size())));
        }
    }

    ~Model3dCompareViewer() override {
        savePreferences();
        if (handle_) openbc_model3d_destroy(handle_);   // blocks until no callback can run any more
    }

    // ----- public API used by the main window ----------------------------------------------
    void setPaths(const QString& left, const QString& right) {
        leftSelector_->setText(left);
        rightSelector_->setText(right);
        if (onTitleChanged) onTitleChanged();
    }

    void compare() {
        const QString left = leftSelector_->text().trimmed(), right = rightSelector_->text().trimmed();
        if (left.isEmpty() || right.isEmpty() || !handle_) return;
        const Openbc3dOptions native = nativeOptions();
        openbc_model3d_set_options(handle_, &native);
        QString leftLocal, rightLocal, resolveError;
        if (!openbc_remote::resolveLocalPath(left, &leftLocal, &resolveError) ||
            !openbc_remote::resolveLocalPath(right, &rightLocal, &resolveError)) {
            QMessageBox::warning(this, "3D Compare", resolveError);
            return;
        }
        const QByteArray l = leftLocal.toUtf8(), r = rightLocal.toUtf8();
        fitPending_ = true;
        openbc_model3d_open(handle_, reinterpret_cast<const std::uint8_t*>(l.constData()), std::size_t(l.size()),
                            reinterpret_cast<const std::uint8_t*>(r.constData()), std::size_t(r.size()));
        leftSelector_->remember(left);   // MRU history in the path dropdown
        rightSelector_->remember(right);
        if (onComparisonRequested) onComparisonRequested(left, right);
        if (onTitleChanged) onTitleChanged();
    }

    QString leftText() const { return leftSelector_->text(); }
    QString rightText() const { return rightSelector_->text(); }
    QString title() const {
        const QString l = QFileInfo(leftSelector_->text()).fileName(), r = QFileInfo(rightSelector_->text()).fileName();
        if (l.isEmpty() && r.isEmpty()) return "3D Compare";
        return l + " <-> " + r;
    }
    QString detailedTitle() const { return leftSelector_->text() + " <-> " + rightSelector_->text(); }

    QList<QAction*> actionsMenuItems() const { return {actSwap_, actReload_, nullptr, actFit_, actWire_}; }
    QList<QAction*> editMenuItems() const { return {}; }
    QList<QAction*> searchMenuItems() const { return {actPrevRegion_, actNextRegion_}; }
    QList<QAction*> toolsMenuItems() const { return {}; }
    QList<QAction*> viewMenuItems() const {
        QList<QAction*> v;
        for (QAction* a : modeActions_) v << a;
        v << nullptr << actPanel_;
        return v;
    }

protected:
    void dragEnterEvent(QDragEnterEvent* e) override { if (e->mimeData()->hasUrls()) e->acceptProposedAction(); }
    void dropEvent(QDropEvent* e) override {
        QStringList files;
        for (const QUrl& url : e->mimeData()->urls()) if (url.isLocalFile()) files << url.toLocalFile();
        if (files.isEmpty()) return;
        if (files.size() >= 2) setPaths(files[0], files[1]);
        else if (leftSelector_->text().isEmpty() || !rightSelector_->text().isEmpty()) setPaths(files[0], rightSelector_->text());
        else setPaths(leftSelector_->text(), files[0]);
        compare();
        e->acceptProposedAction();
    }

private:
    // ===== native event plumbing ===========================================================
    struct ViewInfo {
        QString path, format;
        qint64 size = 0, modifiedMs = 0;
        bool hasModified = false;
        quint64 triangles = 0, vertices = 0;
        double area = 0, volume = 0;
        bool watertight = false;
        QVector3D bmin, bmax;
        bool valid = false;
    };
    struct RegionInfo { int side = 0; quint32 tris = 0; double area = 0; float maxDev = 0; QVector3D bmin, bmax; };
    struct SideCopy { bool present = false; ViewInfo info; Mesh3dPtr mesh; };
    struct ViewEvent {
        std::uint32_t kind = 0, side = 0;
        QString path, message;
        SideCopy left, right;
        Model3dOptions options;
        std::uint32_t backend = 0;
        QVector3D shift;
        bool hasB = false;  std::vector<float> bPositions;
        bool hasDev = false; quint64 devToken = 0; std::vector<float> devA, devB;
        float maxDev = 0, meanDev = 0, changed = 0, tol = 0, range = 1;
        std::vector<float> hist;
        std::vector<RegionInfo> regions; quint32 regionsTotal = 0;
        bool hasChanges = false, hasChangeMeshes = false, truncated = false; quint64 changesToken = 0;
        double removed = 0, added = 0; float cell = 0;
        std::vector<float> rmPos, adPos; std::vector<std::uint32_t> rmIdx, adIdx;
        float alignMs = 0, devMs = 0, analysisMs = 0, changesMs = 0;
    };

    static QString str(const Openbc3dStr& s) { return s.ptr ? QString::fromUtf8(reinterpret_cast<const char*>(s.ptr), qsizetype(s.len)) : QString(); }
    static QVector3D vec(const float* v) { return QVector3D(v[0], v[1], v[2]); }

    static Mesh3dPtr copyMesh(const Openbc3dMesh& m) {
        auto d = std::make_shared<Mesh3dData>();
        if (m.positions && m.vertexCount) d->positions.assign(m.positions, m.positions + m.vertexCount * 3);
        if (m.indices && m.indexCount) d->indices.assign(m.indices, m.indices + m.indexCount);
        if (m.edges && m.edgeCount) d->edges.assign(m.edges, m.edges + m.edgeCount);
        return d;
    }
    static SideCopy copySide(const Openbc3dSide& s) {
        SideCopy c;
        c.present = true;
        ViewInfo& i = c.info;
        i.path = str(s.info.path); i.format = str(s.info.format);
        i.size = qint64(s.info.sizeBytes); i.modifiedMs = s.info.modifiedUnixMs; i.hasModified = s.info.hasModified != 0;
        i.triangles = s.stats.triangles; i.vertices = s.stats.vertices; i.area = s.stats.area; i.volume = s.stats.volume;
        i.watertight = s.stats.watertight != 0; i.bmin = vec(s.stats.bmin); i.bmax = vec(s.stats.bmax); i.valid = true;
        c.mesh = copyMesh(s.mesh);
        return c;
    }

    // Runs on a Rust thread: deep-copy what we keep, then hop to the GUI thread.
    static void onNativeEvent(void* user, const Openbc3dEvent* ev) {
        auto* self = static_cast<Model3dCompareViewer*>(user);
        auto e = std::make_shared<ViewEvent>();
        e->kind = ev->kind; e->side = ev->side; e->path = str(ev->path); e->message = str(ev->message);
        if (ev->kind == OPENBC_3D_LOADED) {
            if (ev->leftPresent) e->left = copySide(ev->left);
            if (ev->rightPresent) e->right = copySide(ev->right);
        } else if (ev->kind == OPENBC_3D_RESULT) {
            Model3dOptions& o = e->options;
            o.tolerance = ev->options.tolerance; o.range = ev->options.range; o.alignment = ev->options.alignment;
            o.offset = vec(ev->options.offset); o.preferGpu = ev->options.preferGpu != 0; o.computeChanges = ev->options.computeChanges != 0;
            e->backend = ev->backend; e->shift = vec(ev->displayShift);
            if (ev->bPositions && ev->bVertexCount) { e->hasB = true; e->bPositions.assign(ev->bPositions, ev->bPositions + ev->bVertexCount * 3); }
            e->devToken = ev->devToken;
            if (ev->devToken != self->lastDevToken_.load()) {   // big arrays are copied only when they changed
                e->hasDev = true;
                e->devA.assign(ev->devA, ev->devA + ev->devACount);
                e->devB.assign(ev->devB, ev->devB + ev->devBCount);
            }
            e->maxDev = ev->maxDeviation; e->meanDev = ev->meanDeviation; e->changed = ev->surfaceChanged; e->tol = ev->tolerance; e->range = ev->range;
            if (ev->histogram) e->hist.assign(ev->histogram, ev->histogram + ev->histogramCount);
            for (std::size_t i = 0; i < ev->regionCount; ++i) {
                const Openbc3dRegion& r = ev->regions[i];
                e->regions.push_back({int(r.side), r.triangles, r.area, r.maxDeviation, vec(r.bmin), vec(r.bmax)});
            }
            e->regionsTotal = ev->regionsTotal;
            e->hasChanges = ev->hasChanges != 0; e->truncated = ev->changesTruncated != 0; e->changesToken = ev->changesToken;
            e->removed = ev->removedMm3; e->added = ev->addedMm3; e->cell = ev->cellSize;
            if (e->hasChanges && ev->changesToken != self->lastChangesToken_.load()) {
                e->hasChangeMeshes = true;
                if (ev->removed.positions) e->rmPos.assign(ev->removed.positions, ev->removed.positions + ev->removed.vertexCount * 3);
                if (ev->removed.indices) e->rmIdx.assign(ev->removed.indices, ev->removed.indices + ev->removed.indexCount);
                if (ev->added.positions) e->adPos.assign(ev->added.positions, ev->added.positions + ev->added.vertexCount * 3);
                if (ev->added.indices) e->adIdx.assign(ev->added.indices, ev->added.indices + ev->added.indexCount);
            }
            e->alignMs = ev->alignMs; e->devMs = ev->deviationMs; e->analysisMs = ev->analysisMs; e->changesMs = ev->changesMs;
        }
        QMetaObject::invokeMethod(self, [self, e]() { self->applyEvent(*e); }, Qt::QueuedConnection);
    }

    Openbc3dOptions nativeOptions() const {
        Openbc3dOptions o{};
        o.tolerance = options_.tolerance; o.range = options_.range;
        o.offset[0] = options_.offset.x(); o.offset[1] = options_.offset.y(); o.offset[2] = options_.offset.z();
        o.alignment = std::uint8_t(options_.alignment); o.preferGpu = options_.preferGpu ? 1 : 0; o.computeChanges = options_.computeChanges ? 1 : 0;
        return o;
    }

    void sendOptions() {
        if (!handle_) return;
        const Openbc3dOptions native = nativeOptions();
        openbc_model3d_set_options(handle_, &native);   // non-blocking, latest wins; supersedes a running pass
        if (loaded_ && !busy_) setStatus("Comparing...", false);
        refreshViews();                                  // tolerance / range are shader uniforms: instant feedback
    }

    // ===== applying events (GUI thread) ====================================================
    void applyEvent(const ViewEvent& e) {
        switch (e.kind) {
            case OPENBC_3D_LOADING:
                busy_ = true; timer_.start(); timerArmed_ = true;
                setStatus("Loading...", false);
                break;
            case OPENBC_3D_LOADED: applyLoaded(e); break;
            case OPENBC_3D_RESULT: applyResult(e); break;
            case OPENBC_3D_PATH_ERROR: {
                busy_ = false;
                const bool right = e.side == 1;
                (right ? rightInfoLabel_ : leftInfoLabel_)->setText(e.message);
                (right ? rightInfoLabel_ : leftInfoLabel_)->setStyleSheet("color:#e5534b;");
                (right ? rightInfo_ : leftInfo_) = ViewInfo();
                scene_.slot[right ? kSlotB : kSlotA].reset();
                loaded_ = false;
                setStatus("Cannot open " + QFileInfo(e.path).fileName(), true);
                refreshPanel(); refreshViews();
                break;
            }
            case OPENBC_3D_NOTICE: statusLabel_->setToolTip(e.message); setStatus(e.message, false); break;
            case OPENBC_3D_FAILED: busy_ = false; setStatus("Compare failed: " + e.message, true); break;
            default: break;
        }
    }

    void applyLoaded(const ViewEvent& e) {
        if (e.left.present) { leftInfo_ = e.left.info; scene_.slot[kSlotA] = e.left.mesh; }
        if (e.right.present) { rightInfo_ = e.right.info; scene_.slot[kSlotB] = e.right.mesh; }
        loaded_ = e.left.present && e.right.present;
        lastDevToken_ = 0; lastChangesToken_ = 0;          // the new meshes have no deviation yet
        scene_.slot[kSlotRemoved].reset(); scene_.slot[kSlotAdded].reset();
        regions_.clear(); selectedRegion_ = -1;
        leftInfoLabel_->setStyleSheet(QString()); rightInfoLabel_->setStyleSheet(QString());
        leftInfoLabel_->setText(infoText(leftInfo_)); rightInfoLabel_->setText(infoText(rightInfo_));
        recomputeBounds();
        if (fitPending_ && loaded_) { camera_.fit(scene_.boundsMin + scene_.shift, scene_.boundsMax + scene_.shift); fitPending_ = false; }
        updateSectionRange();
        refreshPanel(); refreshViews();
        if (onTitleChanged) onTitleChanged();
    }

    void applyResult(const ViewEvent& e) {
        busy_ = false;
        options_ = e.options;                                // the engine echoes what it used (sanitised, swap-negated offset)
        scene_.shift = e.shift;
        if (e.hasB && scene_.slot[kSlotB]) {
            scene_.slot[kSlotB]->positions = e.bPositions;   // aligned positions replace the local ones
            scene_.slot[kSlotB]->version++;
        }
        if (e.hasDev) {
            setDeviation(kSlotA, e.devA); setDeviation(kSlotB, e.devB);
            lastDevToken_ = e.devToken;
        }
        if (e.hasB || e.hasDev) recomputeBounds();
        summary_ = {e.maxDev, e.meanDev, e.changed, e.tol, e.range};
        hist_ = e.hist;
        regions_ = e.regions; regionsTotal_ = e.regionsTotal;
        selectedRegion_ = regions_.empty() ? -1 : std::clamp(selectedRegion_, 0, int(regions_.size()) - 1);
        if (e.hasChanges) {
            if (e.hasChangeMeshes) {
                auto rm = std::make_shared<Mesh3dData>(), ad = std::make_shared<Mesh3dData>();
                rm->positions = e.rmPos; rm->indices = e.rmIdx; ad->positions = e.adPos; ad->indices = e.adIdx;
                scene_.slot[kSlotRemoved] = rm; scene_.slot[kSlotAdded] = ad;
                lastChangesToken_ = e.changesToken;
            }
            removedMm3_ = e.removed; addedMm3_ = e.added; truncated_ = e.truncated; hasChanges_ = true;
        } else {
            hasChanges_ = false;
            scene_.slot[kSlotRemoved].reset(); scene_.slot[kSlotAdded].reset();
        }
        syncControlsFromOptions();
        refreshPanel(); refreshViews();

        const bool bad = !regions_.empty();
        setStatus(bad ? QString("%1 region%2 exceed%3 the tolerance").arg(regionsTotal_).arg(regionsTotal_ == 1 ? "" : "s").arg(regionsTotal_ == 1 ? "s" : "")
                      : "Within tolerance", bad);
        timeLabel_->setText(QString("%1 %2 ms").arg(e.backend == 1 ? "GPU" : "CPU").arg(int(e.devMs + e.analysisMs + e.changesMs + e.alignMs)));
        timeLabel_->setToolTip(QString("align %1 ms, deviation %2 ms, analysis %3 ms, changes %4 ms%5%6")
                                   .arg(int(e.alignMs)).arg(int(e.devMs)).arg(int(e.analysisMs)).arg(int(e.changesMs))
                                   .arg(gpuName_.isEmpty() ? "\nGPU: not available (CPU backend)" : "\nGPU: " + gpuName_)
                                   .arg(e.hasChanges ? QString("\nChanges grid cell: %1 mm").arg(e.cell, 0, 'g', 3) : QString()));
        if (timerArmed_) { timerArmed_ = false; statsLabel_->setText(QString("Load + compare: %1 s").arg(timer_.elapsed() / 1000.0, 0, 'f', 2)); }
        if (onTitleChanged) onTitleChanged();
    }

    void setDeviation(MeshSlot slot, const std::vector<float>& dev) {
        auto& s = scene_.slot[slot];
        if (!s) return;
        s->deviation = dev;                                  // GUI thread only: safe to mutate in place
        s->devVersion++;
    }

    void recomputeBounds() {
        QVector3D mn(1e30f, 1e30f, 1e30f), mx(-1e30f, -1e30f, -1e30f);
        bool any = false;
        for (int s : {int(kSlotA), int(kSlotB)}) {
            const Mesh3dPtr& m = scene_.slot[s];
            if (!m) continue;
            for (std::size_t i = 0; i + 2 < m->positions.size(); i += 3) {
                const QVector3D p(m->positions[i], m->positions[i + 1], m->positions[i + 2]);
                mn = QVector3D(std::min(mn.x(), p.x()), std::min(mn.y(), p.y()), std::min(mn.z(), p.z()));
                mx = QVector3D(std::max(mx.x(), p.x()), std::max(mx.y(), p.y()), std::max(mx.z(), p.z()));
                any = true;
            }
        }
        if (any) { scene_.boundsMin = mn; scene_.boundsMax = mx; }
        updateSectionRange();
    }

    // ===== views ==========================================================================
    static QString baseName(const QString& p) { return QFileInfo(p).fileName(); }

    void refreshViews() {
        const bool wire = wire_;
        auto solid = [&](int slot) { Layer l; l.slot = slot; l.wire = wire; return l; };
        vpB_->setVisible(mode_ == ViewMode::TwoUp);
        heatLegend_->setVisible(mode_ == ViewMode::Heatmap && !hist_.empty());
        changesLegend_->setVisible(mode_ == ViewMode::Changes && hasChanges_);
        overlayBox_->setVisible(mode_ == ViewMode::Overlay);
        sideBox_->setVisible(mode_ == ViewMode::OneUp || mode_ == ViewMode::Heatmap);
        sideLabel_->setText(mode_ == ViewMode::Heatmap ? "Deviation on" : "Show");
        vpA_->setSplit(false);
        vpA_->setBadge(QString());
        vpB_->setBadge(QString());

        switch (mode_) {
            case ViewMode::TwoUp:
                vpA_->setLayers({solid(kSlotA)}); vpB_->setLayers({solid(kSlotB)});
                vpA_->setBadge("A  " + baseName(leftSelector_->text())); vpB_->setBadge("B  " + baseName(rightSelector_->text()));
                break;
            case ViewMode::OneUp:
                vpA_->setLayers({solid(sideSel_ == 0 ? kSlotA : kSlotB)});
                vpA_->setBadge((sideSel_ == 0 ? "A  " : "B  ") + baseName(sideSel_ == 0 ? leftSelector_->text() : rightSelector_->text()));
                break;
            case ViewMode::Split:
                vpA_->setLayers({solid(kSlotA)});
                vpA_->setSplit(true, {solid(kSlotB)}, splitFrac_);
                vpA_->setBadge("A | B");
                break;
            case ViewMode::Overlay: {
                const float t = overlaySlider_->value() / 100.0f;
                Layer a = solid(kSlotA); a.alpha = 0.15f + 0.70f * (1.0f - t);
                Layer b = solid(kSlotB); b.alpha = 0.15f + 0.70f * t; b.shade = Shade::Heat;
                vpA_->setLayers({a, b});
                break;
            }
            case ViewMode::Heatmap: {
                Layer l = solid(sideSel_ == 0 ? kSlotA : kSlotB); l.shade = Shade::Heat;
                vpA_->setLayers({l});
                break;
            }
            case ViewMode::Changes: {
                Layer ghost = solid(kSlotA); ghost.alpha = 0.14f; ghost.wire = false;
                Layer rm; rm.slot = kSlotRemoved; rm.color = QColor(0xd0, 0x18, 0x20);
                Layer ad; ad.slot = kSlotAdded; ad.color = QColor(0x1c, 0x4c, 0xe0);
                vpA_->setLayers({ghost, rm, ad});
                break;
            }
        }
        for (auto* vp : {vpA_, vpB_}) {
            vp->setTolerance(options_.tolerance, options_.range);
            vp->setShowGrid(showGrid_);
            vp->setShowBoxes(showBoxes_ && mode_ != ViewMode::Changes);
            std::vector<RegionBox> boxes;
            for (const RegionInfo& r : regions_) boxes.push_back({r.bmin, r.bmax});
            vp->setRegionBoxes(boxes, selectedRegion_);
            vp->setSection(sectionCheck_->isChecked(), sectionAxis_->currentIndex(), sectionPos());
            vp->meshChanged();
        }
        if (heatLegend_->isVisible()) heatLegend_->set(hist_, options_.range, options_.tolerance, summary_.max, summary_.mean);
        if (changesLegend_->isVisible()) changesLegend_->set(removedMm3_, addedMm3_, truncated_);
        placeLegends();
    }

    void placeLegends() {
        for (QWidget* w : {static_cast<QWidget*>(heatLegend_), static_cast<QWidget*>(changesLegend_)})
            w->move(vpA_->width() - w->width() - 12, vpA_->height() - w->height() - 12);
    }

    void setViewMode(ViewMode m) {
        mode_ = m;
        if (modeActions_[int(m)] && !modeActions_[int(m)]->isChecked()) modeActions_[int(m)]->setChecked(true);
        refreshViews();
    }

    void cameraMoved() { vpA_->update(); vpB_->update(); }

    void fitView() {
        camera_.fit(scene_.boundsMin + scene_.shift, scene_.boundsMax + scene_.shift);
        cameraMoved();
    }

    void focusRegion(int i) {
        if (i < 0 || i >= int(regions_.size())) return;
        selectedRegion_ = i;
        const RegionInfo& r = regions_[i];
        camera_.target = (r.bmin + r.bmax) * 0.5f + scene_.shift;
        camera_.dist = std::clamp((r.bmax - r.bmin).length() * 2.5f, camera_.radius * 0.2f, camera_.radius * 6.0f);
        { const QSignalBlocker b(regionList_); regionList_->setCurrentRow(i); }
        updateRegionNav(); refreshViews();
    }

    void stepRegion(int d) {
        if (regions_.empty()) return;
        focusRegion((std::max(selectedRegion_, 0) + d + int(regions_.size())) % int(regions_.size()));
    }

    // ===== probe ===========================================================================
    void pickAt(int viewportSide, const QVector3D& origin, const QVector3D& dir) {
        if (!handle_ || !loaded_) return;
        int side = viewportSide;
        if (mode_ == ViewMode::OneUp || mode_ == ViewMode::Heatmap) side = sideSel_;
        if (mode_ == ViewMode::Split) side = (vpA_->mapFromGlobal(QCursor::pos()).x() > vpA_->width() * splitFrac_) ? 1 : 0;
        const float o[3] = {origin.x(), origin.y(), origin.z()}, d[3] = {dir.x(), dir.y(), dir.z()};
        Openbc3dProbe pr{};
        if (!openbc_model3d_probe(handle_, std::uint8_t(side), o, d, &pr)) { clearProbe(); return; }
        const QVector3D p = vec(pr.point) - scene_.shift;
        const QString label = QString("%1 %2 mm").arg(pr.side == 0 ? "A" : "B").arg(double(pr.deviation), 0, 'f', 3).replace("-", QString::fromUtf8("\xE2\x88\x92"));
        for (auto* vp : {vpA_, vpB_}) vp->setProbe(p, label);
    }
    void clearProbe() { for (auto* vp : {vpA_, vpB_}) vp->setProbe(std::nullopt, QString()); }

    // ===== options <-> controls ============================================================
    static double tolFromSlider(int v) { return 0.001 * std::pow(5000.0, v / 1000.0); }
    static int sliderFromTol(double t) { return int(std::lround(1000.0 * std::log(std::max(t, 0.001) / 0.001) / std::log(5000.0))); }

    void setAlignment(int mode) { options_.alignment = mode; syncControlsFromOptions(); sendOptions(); }

    void syncControlsFromOptions() {
        for (int i = 0; i < 4; ++i) if (alignActions_[i]) { const QSignalBlocker b(alignActions_[i]); alignActions_[i]->setChecked(i == options_.alignment); }
        { const QSignalBlocker b(tolSlider_); tolSlider_->setValue(sliderFromTol(options_.tolerance)); }
        tolLabel_->setText(QString("%1 mm").arg(options_.tolerance, 0, 'g', 3));
        QDoubleSpinBox* sp[3] = {offX_, offY_, offZ_};
        const float v[3] = {options_.offset.x(), options_.offset.y(), options_.offset.z()};
        for (int i = 0; i < 3; ++i) if (sp[i]) { const QSignalBlocker b(sp[i]); sp[i]->setValue(v[i]); }
        if (actGpu_) { const QSignalBlocker b(actGpu_); actGpu_->setChecked(options_.preferGpu); }
        if (actChangesVol_) { const QSignalBlocker b(actChangesVol_); actChangesVol_->setChecked(options_.computeChanges); }
        for (int i = 0; i < 4; ++i) if (rangeActions_[i]) { const QSignalBlocker b(rangeActions_[i]); rangeActions_[i]->setChecked(std::abs(options_.range - kRanges[i]) < 1e-4f); }
    }

    void loadPreferences() {
        if (!prefs_) return;
        options_.tolerance = std::clamp(prefs_->value("model3d/tolerance", 0.05).toFloat(), 0.001f, 5.0f);
        options_.range = std::clamp(prefs_->value("model3d/range", 1.0).toFloat(), 0.01f, 100.0f);
        options_.alignment = std::clamp(prefs_->value("model3d/alignment", 0).toInt(), 0, 3);
        options_.preferGpu = prefs_->value("model3d/preferGpu", true).toBool();
        options_.computeChanges = prefs_->value("model3d/changes", true).toBool();
        mode_ = ViewMode(std::clamp(prefs_->value("model3d/viewMode", 0).toInt(), 0, 5));
        wire_ = prefs_->value("model3d/wire", false).toBool();
        panelVisible_ = prefs_->value("model3d/panel", true).toBool();
        showGrid_ = prefs_->value("model3d/grid", true).toBool();
        showBoxes_ = prefs_->value("model3d/boxes", true).toBool();
    }
    void savePreferences() {
        if (!prefs_) return;
        prefs_->setValue("model3d/tolerance", options_.tolerance); prefs_->setValue("model3d/range", options_.range);
        prefs_->setValue("model3d/alignment", options_.alignment); prefs_->setValue("model3d/preferGpu", options_.preferGpu);
        prefs_->setValue("model3d/changes", options_.computeChanges); prefs_->setValue("model3d/viewMode", int(mode_));
        prefs_->setValue("model3d/wire", wire_); prefs_->setValue("model3d/panel", panelVisible_);
        prefs_->setValue("model3d/grid", showGrid_); prefs_->setValue("model3d/boxes", showBoxes_);
    }

    // ===== side panel ======================================================================
    static QString mm(double v, int prec = 3) { return QString::number(v, 'f', prec) + " mm"; }
    static QString infoText(const ViewInfo& i) {
        if (!i.valid) return QString();
        QStringList parts;
        if (i.hasModified) parts << QLocale().toString(QDateTime::fromMSecsSinceEpoch(i.modifiedMs), QLocale::ShortFormat);
        parts << QString("%1 tris").arg(QLocale().toString(qulonglong(i.triangles)));
        const QVector3D s = i.bmax - i.bmin;
        parts << QString("%1x%2x%3 mm").arg(s.x(), 0, 'f', 1).arg(s.y(), 0, 'f', 1).arg(s.z(), 0, 'f', 1);
        if (!i.format.isEmpty()) parts << i.format;
        return parts.join("    ");
    }
    static QString kb(qint64 b) { return QString::number(b / 1000.0, 'g', 3) + " kB"; }
    static QString signedNum(double d, int prec = 0) { return (d > 0 ? "+" : "") + QString::number(d, 'f', prec); }

    void refreshPanel() {
        const ViewInfo &a = leftInfo_, &b = rightInfo_;
        const bool both = a.valid && b.valid;
        auto setRow = [&](int r, const QString& av, const QString& bv, const QString& dv) {
            statA_[r]->setText(av); statB_[r]->setText(bv); statD_[r]->setText(dv);
        };
        if (!both) { for (int r = 0; r < 7; ++r) setRow(r, "-", "-", ""); }
        else {
            auto n = [](double v) { return QLocale().toString(qulonglong(v)); };
            setRow(0, n(a.triangles), n(b.triangles), signedNum(double(b.triangles) - double(a.triangles)));
            setRow(1, n(a.vertices), n(b.vertices), signedNum(double(b.vertices) - double(a.vertices)));
            setRow(2, n(a.area), n(b.area), signedNum(b.area - a.area));
            setRow(3, n(a.volume), n(b.volume), signedNum(b.volume - a.volume));
            setRow(4, kb(a.size), kb(b.size), (b.size >= a.size ? "+" : "") + QString::number((b.size - a.size) / 1000.0, 'g', 3) + " kB");
            setRow(5, a.watertight ? "yes" : "no", b.watertight ? "yes" : "no", "");
            auto bb = [](const ViewInfo& i) { const QVector3D s = i.bmax - i.bmin; return QString("%1x%2x%3").arg(s.x(), 0, 'f', 1).arg(s.y(), 0, 'f', 1).arg(s.z(), 0, 'f', 1); };
            setRow(6, bb(a), bb(b), "");
        }
        const bool have = both && !hist_.empty();
        // Beyond the heat-map range the exact value is not meaningful: show ">= range" like the reference UI.
        sumMax_->setText(!have ? QString("-")
                         : summary_.max >= options_.range ? QString::fromUtf8("\xE2\x89\xA5 ") + mm(options_.range, 3)
                                                          : mm(summary_.max, 3));
        sumMean_->setText(have ? mm(summary_.mean, 3) : "-");
        sumChanged_->setText(have ? QString::number(summary_.changed * 100.0, 'f', 1) + "%" : "-");
        sumTol_->setText(mm(options_.tolerance, options_.tolerance < 0.1 ? 3 : 2));
        regionsTitle_->setText(QString("REGIONS (%1)").arg(regionsTotal_));
        { const QSignalBlocker blk(regionList_);
          regionList_->clear();
          for (const RegionInfo& r : regions_)
              regionList_->addItem(QString("%1    %2 mm\xC2\xB2    %3 mm").arg(r.side == 0 ? "A" : "B").arg(r.area, 0, 'f', 1).arg(r.maxDev, 0, 'f', 2));
          if (selectedRegion_ >= 0) regionList_->setCurrentRow(selectedRegion_); }
        updateRegionNav();
    }

    void updateRegionNav() {
        const int n = int(regionsTotal_);
        regionNavLabel_->setText(n == 0 ? "no regions" : QString("%1 of %2 region%3").arg(std::max(selectedRegion_, 0) + 1).arg(n).arg(n == 1 ? "" : "s"));
        actPrevRegion_->setEnabled(!regions_.empty()); actNextRegion_->setEnabled(!regions_.empty());
    }

    // ===== section =========================================================================
    float sectionPos() const {
        const int ax = sectionAxis_->currentIndex();
        const float lo = scene_.boundsMin[ax], hi = scene_.boundsMax[ax];
        return lo + (hi - lo) * sectionSlider_->value() / 1000.0f;
    }
    void updateSectionRange() { if (vpA_) refreshSectionOnly(); }
    void refreshSectionOnly() { for (auto* vp : {vpA_, vpB_}) vp->setSection(sectionCheck_->isChecked(), sectionAxis_->currentIndex(), sectionPos()); }

    // ===== actions =========================================================================
    void setStatus(const QString& text, bool emphasise) {
        statusLabel_->setText(text);
        statusLabel_->setStyleSheet(emphasise ? "color:#e5534b; font-weight:600;" : QString());
    }

    void swapSides() {
        if (!handle_) return;
        const QString l = leftSelector_->text();
        leftSelector_->setText(rightSelector_->text()); rightSelector_->setText(l);
        std::swap(leftInfo_, rightInfo_);
        leftInfoLabel_->setText(infoText(leftInfo_)); rightInfoLabel_->setText(infoText(rightInfo_));
        openbc_model3d_swap(handle_);   // engine swaps, negates the offset, resends meshes and recompares
        if (onTitleChanged) onTitleChanged();
    }

    void createActions() {
        using Glyph = icons::Glyph;
        auto mk = [this](const QString& text, const QString& cap, const QIcon& icon, const QString& tip, bool checkable = false) {
            auto* a = new QAction(text, this);
            a->setIcon(icon); a->setIconText(cap); a->setToolTip(tip); a->setCheckable(checkable);
            return a;
        };
        // view modes (exclusive)
        modeGroup_ = new QActionGroup(this);
        struct M { const char* name; const char* cap; QIcon icon; const char* tip; };
        const M modes[6] = {
            {"2-Up", "2-Up", m3::twoUp(), "Two views side by side, cameras linked"},
            {"1-Up", "1-Up", m3::oneUp(), "Single view of A or B"},
            {"Split", "Split", m3::split(), "One view split by a draggable handle: A on the left, B on the right"},
            {"Overlay", "Overlay", m3::overlay(), "Both models translucent on top of each other (A-B slider below)"},
            {"Heatmap", "Heatmap", m3::heatmap(), "Colour the surface by signed deviation from the other model"},
            {"Changes", "Changes", m3::changes(), "Volume removed (red) and added (blue)"},
        };
        for (int i = 0; i < 6; ++i) {
            modeActions_[i] = mk(modes[i].name, modes[i].cap, modes[i].icon, modes[i].tip, true);
            modeActions_[i]->setIconVisibleInMenu(false);
            modeGroup_->addAction(modeActions_[i]);
            connect(modeActions_[i], &QAction::triggered, this, [this, i]() { setViewMode(ViewMode(i)); });
        }
        modeActions_[int(mode_)]->setChecked(true);

        actWire_ = mk("Wireframe", "Wire", m3::wireframe(), "Show mesh triangles", true);
        actWire_->setChecked(wire_);
        connect(actWire_, &QAction::toggled, this, [this](bool on) { wire_ = on; refreshViews(); });

        actSwap_ = mk("Swap Sides", "Swap", icons::glyph(Glyph::Swap), "Swap A and B");
        connect(actSwap_, &QAction::triggered, this, [this]() { swapSides(); });
        actReload_ = mk("Reload", "Reload", icons::glyph(Glyph::Refresh), "Reload both models (F5)");
        actReload_->setShortcut(QKeySequence("F5"));
        connect(actReload_, &QAction::triggered, this, [this]() { fitPending_ = false; if (handle_) openbc_model3d_reload(handle_); });
        actFit_ = mk("Fit View", "Fit", m3::fitModel(), "Frame both models (double-click the view)");
        actFit_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_0));
        connect(actFit_, &QAction::triggered, this, [this]() { fitView(); });

        actPanel_ = mk("Details Panel", "Panel", m3::sidePanel(), "Show / hide summary, stats and regions", true);
        actPanel_->setChecked(panelVisible_);
        connect(actPanel_, &QAction::toggled, this, [this](bool on) { panelVisible_ = on; if (panel_) panel_->setVisible(on); });

        actPrevRegion_ = mk("Previous Region", "", m3::prev(), "Previous region (Shift+F3)");
        actPrevRegion_->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F3));
        actNextRegion_ = mk("Next Region", "", m3::next(), "Next region (F3)");
        actNextRegion_->setShortcut(QKeySequence(Qt::Key_F3));
        connect(actPrevRegion_, &QAction::triggered, this, [this]() { stepRegion(-1); });
        connect(actNextRegion_, &QAction::triggered, this, [this]() { stepRegion(+1); });
    }

    QToolButton* menuButton(const QString& caption, const QIcon& icon, const QString& tip, QMenu* menu) {
        auto* b = new QToolButton(toolbar_);
        b->setText(caption); b->setIcon(icon); b->setToolTip(tip);
        b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        b->setPopupMode(QToolButton::InstantPopup);
        b->setMenu(menu);
        return b;
    }

    QMenu* buildAlignMenu() {
        auto* menu = new QMenu(this);
        menu->setToolTipsVisible(true);
        static const char* names[4] = {"Original coordinates", "Center both at origin", "Align bounding-box corners", "Align centroids"};
        static const char* tips[4] = {"Compare in file coordinates (same CAD origin)", "Move both bounding-box centers to the origin",
                                      "Move B so the bbox minimum corners coincide", "Move B so both vertex centroids coincide"};
        auto* group = new QActionGroup(this);
        for (int i = 0; i < 4; ++i) {
            alignActions_[i] = menu->addAction(names[i]);
            alignActions_[i]->setCheckable(true); alignActions_[i]->setToolTip(tips[i]);
            group->addAction(alignActions_[i]);
            connect(alignActions_[i], &QAction::triggered, this, [this, i]() { setAlignment(i); });
        }
        menu->addSeparator();
        auto* offsetBox = new QWidget(menu);
        auto* ol = new QGridLayout(offsetBox);
        ol->setContentsMargins(12, 4, 12, 4);
        ol->addWidget(new QLabel("Extra offset for B (mm) - the diff re-runs on change", offsetBox), 0, 0, 1, 6);
        QDoubleSpinBox** spins[3] = {&offX_, &offY_, &offZ_};
        const char* axes[3] = {"X", "Y", "Z"};
        for (int i = 0; i < 3; ++i) {
            *spins[i] = new QDoubleSpinBox(offsetBox);
            (*spins[i])->setRange(-100000, 100000); (*spins[i])->setDecimals(3); (*spins[i])->setSingleStep(0.1);
            ol->addWidget(new QLabel(axes[i], offsetBox), 1, i * 2); ol->addWidget(*spins[i], 1, i * 2 + 1);
            connect(*spins[i], &QDoubleSpinBox::valueChanged, this, [this](double) {
                options_.offset = QVector3D(float(offX_->value()), float(offY_->value()), float(offZ_->value()));
                sendOptions();
            });
        }
        auto* wa = new QWidgetAction(menu); wa->setDefaultWidget(offsetBox); menu->addAction(wa);
        menu->addSeparator();
        auto addCmd = [&](const QString& text, const QIcon& icon, const QString& tip, int cmd) {
            auto* btn = new QPushButton(icon, text, menu);
            btn->setToolTip(tip);
            connect(btn, &QPushButton::clicked, this, [this, menu, cmd]() { if (handle_) openbc_model3d_align(handle_, std::uint32_t(cmd)); menu->close(); });
            auto* holder = new QWidget(menu); auto* hl = new QVBoxLayout(holder); hl->setContentsMargins(12, 3, 12, 3);
            hl->addWidget(btn);
            auto* cap = new QLabel(tip, holder); cap->setWordWrap(true); cap->setEnabled(false); cap->setMaximumWidth(300); hl->addWidget(cap);
            auto* w = new QWidgetAction(menu); w->setDefaultWidget(holder); menu->addAction(w);
        };
        addCmd("Refine alignment (ICP)", m3::icp(), "Fits B onto A (rotation + translation) starting from the current alignment, then re-runs the diff.", 0);
        addCmd("Align parts individually", m3::parts(), "Matches each disconnected part across the two files and aligns them one by one - re-arranged plates diff as if nothing moved.", 1);
        addCmd("Reset refinement", m3::alignment(), "Drops ICP and per-part adjustments and returns to the selected mode + offset.", 2);
        return menu;
    }

    QMenu* buildSettingsMenu() {
        auto* menu = new QMenu(this);
        menu->setToolTipsVisible(true);
        actGpu_ = menu->addAction("Prefer GPU (when available and the files are large enough)");
        actGpu_->setCheckable(true); actGpu_->setChecked(options_.preferGpu);
        actGpu_->setToolTip("The GPU is used for files >= 4 MiB (never below 1 MiB); otherwise Rayon on the CPU");
        connect(actGpu_, &QAction::toggled, this, [this](bool on) { options_.preferGpu = on; sendOptions(); });
        actChangesVol_ = menu->addAction("Compute volumetric changes (Changes view)");
        actChangesVol_->setCheckable(true); actChangesVol_->setChecked(options_.computeChanges);
        connect(actChangesVol_, &QAction::toggled, this, [this](bool on) { options_.computeChanges = on; sendOptions(); });
        menu->addSeparator();
        auto* rangeMenu = menu->addMenu("Heat-map range");
        auto* group = new QActionGroup(this);
        for (int i = 0; i < 4; ++i) {
            rangeActions_[i] = rangeMenu->addAction(QString::fromUtf8("\xC2\xB1") + QString::number(kRanges[i]) + " mm");
            rangeActions_[i]->setCheckable(true); group->addAction(rangeActions_[i]);
            connect(rangeActions_[i], &QAction::triggered, this, [this, i]() { options_.range = kRanges[i]; sendOptions(); });
        }
        menu->addSeparator();
        auto* grid = menu->addAction("Show grid"); grid->setCheckable(true); grid->setChecked(showGrid_);
        connect(grid, &QAction::toggled, this, [this](bool on) { showGrid_ = on; refreshViews(); });
        auto* boxes = menu->addAction("Show region boxes"); boxes->setCheckable(true); boxes->setChecked(showBoxes_);
        connect(boxes, &QAction::toggled, this, [this](bool on) { showBoxes_ = on; refreshViews(); });
        return menu;
    }

    QMenu* buildMoreMenu() {
        auto* menu = new QMenu(this);
        menu->addAction(actFit_);
        menu->addAction("Clear probe", this, [this]() { clearProbe(); });
        menu->addSeparator();
        menu->addAction("Copy summary", this, [this]() {
            QApplication::clipboard()->setText(QString("%1 <-> %2\nMax deviation: %3\nMean deviation: %4\nSurface changed: %5\nTolerance: %6\nRegions: %7")
                .arg(leftSelector_->text(), rightSelector_->text(), sumMax_->text(), sumMean_->text(), sumChanged_->text(), sumTol_->text()).arg(regionsTotal_));
        });
        return menu;
    }

    // ===== widgets =========================================================================
    QWidget* buildToolbar() {
        toolbar_ = new QToolBar(this);
        toolbar_->setObjectName("textCompareToolbar");
        toolbar_->setMovable(false); toolbar_->setFloatable(false);
        toolbar_->setIconSize(QSize(20, 20));
        toolbar_->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        for (QAction* a : modeActions_) toolbar_->addAction(a);
        toolbar_->addSeparator();
        toolbar_->addAction(actWire_);
        toolbar_->addWidget(menuButton("Align", m3::alignment(), "Alignment: mode, offset, ICP, align parts", buildAlignMenu()));
        toolbar_->addAction(actSwap_);
        toolbar_->addAction(actReload_);
        toolbar_->addAction(actFit_);
        toolbar_->addSeparator();
        toolbar_->addAction(actPanel_);
        toolbar_->addWidget(menuButton("Options", m3::settings(), "GPU, heat-map range, grid and region boxes", buildSettingsMenu()));
        toolbar_->addWidget(menuButton("More", m3::more(), "More actions", buildMoreMenu()));
        return toolbar_;
    }

    QWidget* buildPathRows() {
        auto* box = new QWidget(this);
        auto* row = new QHBoxLayout(box);
        row->setContentsMargins(6, 0, 6, 4); row->setSpacing(8);
        auto side = [&](PathSelector*& selector, QLabel*& info, const QString& name) {
            auto* col = new QVBoxLayout; col->setSpacing(2);
            selector = new PathSelector(PathSelector::Mode::File, name, box, false, QString(),
                                        /*enableRemote=*/true, &sharedVfsBridge());
            selector->fileFilterProvider = [this]() { return model3dFileDialogFilter(handle_); };
            selector->onBrowsed = [this](QString) { compare(); };
            connect(selector->combo()->lineEdit(), &QLineEdit::returnPressed, this, [this]() { compare(); });
            connect(selector->combo(), QOverload<int>::of(&QComboBox::activated), this, [this](int) { compare(); });
            info = new QLabel(box); info->setTextInteractionFlags(Qt::TextSelectableByMouse);
            col->addWidget(selector); col->addWidget(info);
            row->addLayout(col, 1);
        };
        side(leftSelector_, leftInfoLabel_, "Left");
        side(rightSelector_, rightInfoLabel_, "Right");
        return box;
    }

    QWidget* buildBody() {
        auto* split = new QSplitter(Qt::Horizontal, this);
        auto* area = new QWidget(split);
        auto* al = new QHBoxLayout(area);
        al->setContentsMargins(0, 0, 0, 0); al->setSpacing(2);
        vpA_ = new Model3dViewport(&scene_, &camera_, area);
        vpB_ = new Model3dViewport(&scene_, &camera_, area);
        al->addWidget(vpA_, 1); al->addWidget(vpB_, 1);
        heatLegend_ = new M3HeatLegend(vpA_);
        changesLegend_ = new M3ChangesLegend(vpA_);
        heatLegend_->hide(); changesLegend_->hide();
        split->addWidget(area);
        panel_ = buildPanel(split);
        panel_->setVisible(panelVisible_);
        split->addWidget(panel_);
        split->setStretchFactor(0, 1); split->setStretchFactor(1, 0);
        split->setChildrenCollapsible(false);
        return split;
    }

    QWidget* buildPanel(QWidget* parent) {
        auto* scroll = new QScrollArea(parent);
        scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
        scroll->setMinimumWidth(300); scroll->setMaximumWidth(380);
        auto* body = new QWidget(scroll);
        auto* col = new QVBoxLayout(body);
        col->setContentsMargins(12, 10, 12, 10); col->setSpacing(6);
        auto heading = [&](const QString& t) { auto* l = new QLabel(t, body); QFont f = l->font(); f.setPointSizeF(f.pointSizeF() * 0.85); f.setLetterSpacing(QFont::AbsoluteSpacing, 0.8); l->setFont(f); l->setEnabled(false); return l; };

        col->addWidget(heading("SUMMARY"));
        auto* sg = new QGridLayout; sg->setHorizontalSpacing(8); sg->setVerticalSpacing(3);
        const char* sn[4] = {"Max deviation", "Mean deviation", "Surface changed", "Tolerance \xCF\x84"};
        QLabel** sv[4] = {&sumMax_, &sumMean_, &sumChanged_, &sumTol_};
        for (int i = 0; i < 4; ++i) {
            sg->addWidget(new QLabel(QString::fromUtf8(sn[i]), body), i, 0);
            *sv[i] = new QLabel("-", body); (*sv[i])->setAlignment(Qt::AlignRight);
            sg->addWidget(*sv[i], i, 1);
        }
        col->addLayout(sg);
        col->addSpacing(6);
        col->addWidget(heading("STATS"));
        auto* tg = new QGridLayout; tg->setHorizontalSpacing(10); tg->setVerticalSpacing(3);
        tg->addWidget(new QLabel("A", body), 0, 1); tg->addWidget(new QLabel("B", body), 0, 2);
        tg->addWidget(new QLabel(QString::fromUtf8("\xCE\x94"), body), 0, 3);
        const char* tn[7] = {"Triangles", "Vertices", "Area (mm\xC2\xB2)", "Volume (mm\xC2\xB3)", "Size", "Watertight", "Bounding box"};
        for (int r = 0; r < 7; ++r) {
            tg->addWidget(new QLabel(QString::fromUtf8(tn[r]), body), r + 1, 0);
            for (auto* arr : {&statA_, &statB_, &statD_}) {
                auto* l = new QLabel("-", body); l->setWordWrap(r == 6);
                if (arr == &statD_) l->setAlignment(Qt::AlignRight);
                (*arr)[r] = l;
            }
            tg->addWidget(statA_[r], r + 1, 1); tg->addWidget(statB_[r], r + 1, 2); tg->addWidget(statD_[r], r + 1, 3);
        }
        col->addLayout(tg);
        col->addSpacing(6);
        regionsTitle_ = heading("REGIONS (0)");
        col->addWidget(regionsTitle_);
        regionList_ = new QListWidget(body);
        regionList_->setMinimumHeight(120);
        connect(regionList_, &QListWidget::currentRowChanged, this, [this](int row) { focusRegion(row); });
        col->addWidget(regionList_, 1);
        scroll->setWidget(body);
        return scroll;
    }

    QWidget* buildBottomBar() {
        auto* bar = new QWidget(this);
        auto* row = new QHBoxLayout(bar);
        row->setContentsMargins(8, 3, 8, 3); row->setSpacing(8);

        row->addWidget(new QLabel(QString::fromUtf8("\xCF\x84"), bar));
        tolSlider_ = new QSlider(Qt::Horizontal, bar);
        tolSlider_->setRange(0, 1000); tolSlider_->setFixedWidth(140);
        tolSlider_->setToolTip("Tolerance: deviations below this count as unchanged");
        connect(tolSlider_, &QSlider::valueChanged, this, [this](int v) {
            options_.tolerance = float(tolFromSlider(v));
            tolLabel_->setText(QString("%1 mm").arg(options_.tolerance, 0, 'g', 3));
            sendOptions();
        });
        tolLabel_ = new QLabel(bar); tolLabel_->setMinimumWidth(64);
        row->addWidget(tolSlider_); row->addWidget(tolLabel_);

        row->addWidget(vline(bar));
        sectionCheck_ = new QCheckBox("Section", bar);
        sectionCheck_->setIcon(m3::section());
        sectionAxis_ = new QComboBox(bar); sectionAxis_->addItems({"X", "Y", "Z"}); sectionAxis_->setCurrentIndex(2); sectionAxis_->setVisible(false);
        sectionSlider_ = new QSlider(Qt::Horizontal, bar); sectionSlider_->setRange(0, 1000); sectionSlider_->setValue(500);
        sectionSlider_->setFixedWidth(110); sectionSlider_->setVisible(false);
        auto upd = [this]() { sectionAxis_->setVisible(sectionCheck_->isChecked()); sectionSlider_->setVisible(sectionCheck_->isChecked()); refreshSectionOnly(); };
        connect(sectionCheck_, &QCheckBox::toggled, this, upd);
        connect(sectionAxis_, &QComboBox::currentIndexChanged, this, upd);
        connect(sectionSlider_, &QSlider::valueChanged, this, [this](int) { refreshSectionOnly(); });
        row->addWidget(sectionCheck_); row->addWidget(sectionAxis_); row->addWidget(sectionSlider_);

        // Overlay: A <-> B mix
        overlayBox_ = new QWidget(bar);
        auto* ol = new QHBoxLayout(overlayBox_); ol->setContentsMargins(0, 0, 0, 0);
        ol->addWidget(vline(overlayBox_)); ol->addWidget(new QLabel("A", overlayBox_));
        overlaySlider_ = new QSlider(Qt::Horizontal, overlayBox_); overlaySlider_->setRange(0, 100); overlaySlider_->setValue(50); overlaySlider_->setFixedWidth(120);
        connect(overlaySlider_, &QSlider::valueChanged, this, [this](int) { refreshViews(); });
        ol->addWidget(overlaySlider_); ol->addWidget(new QLabel("B", overlayBox_));
        row->addWidget(overlayBox_);

        // 1-Up: which model / Heatmap: which model carries the colours
        sideBox_ = new QWidget(bar);
        auto* sl = new QHBoxLayout(sideBox_); sl->setContentsMargins(0, 0, 0, 0);
        sl->addWidget(vline(sideBox_));
        sideLabel_ = new QLabel("Show", sideBox_); sl->addWidget(sideLabel_);
        auto* grp = new QButtonGroup(sideBox_);
        for (int i = 0; i < 2; ++i) {
            auto* b = new QToolButton(sideBox_); b->setText(i == 0 ? "A" : "B"); b->setCheckable(true); b->setChecked(i == 0);
            grp->addButton(b, i); sl->addWidget(b);
        }
        connect(grp, &QButtonGroup::idClicked, this, [this](int id) { sideSel_ = id; refreshViews(); });
        row->addWidget(sideBox_);

        row->addStretch(1);
        statusLabel_ = new QLabel("Choose two 3D models to compare", bar);
        statsLabel_ = new QLabel(bar);
        timeLabel_ = new QLabel(bar);
        row->addWidget(statusLabel_); row->addWidget(statsLabel_); row->addWidget(timeLabel_);
        row->addWidget(vline(bar));
        auto* prev = new QToolButton(bar); prev->setDefaultAction(actPrevRegion_); prev->setToolButtonStyle(Qt::ToolButtonIconOnly);
        regionNavLabel_ = new QLabel("no regions", bar); regionNavLabel_->setMinimumWidth(96); regionNavLabel_->setAlignment(Qt::AlignCenter);
        auto* next = new QToolButton(bar); next->setDefaultAction(actNextRegion_); next->setToolButtonStyle(Qt::ToolButtonIconOnly);
        row->addWidget(prev); row->addWidget(regionNavLabel_); row->addWidget(next);
        return bar;
    }

    static QFrame* vline(QWidget* parent) { auto* f = new QFrame(parent); f->setFrameShape(QFrame::VLine); f->setFrameShadow(QFrame::Sunken); return f; }

    void wireViewports() {
        for (auto* vp : {vpA_, vpB_}) vp->onCameraChanged = [this]() { cameraMoved(); };
        vpA_->onPick = [this](const QVector3D& o, const QVector3D& d) { pickAt(0, o, d); };
        vpB_->onPick = [this](const QVector3D& o, const QVector3D& d) { pickAt(1, o, d); };
        vpA_->onSplitMoved = [this](float f) { splitFrac_ = f; };
        vpA_->onResized = [this]() { placeLegends(); };
    }

    // ===== state ===========================================================================
    static constexpr float kRanges[4] = {0.25f, 0.5f, 1.0f, 5.0f};

    QSettings* prefs_ = nullptr;
    Openbc3dCompare* handle_ = nullptr;
    QString gpuName_;
    Model3dOptions options_;
    ViewMode mode_ = ViewMode::TwoUp;
    bool wire_ = false, panelVisible_ = true, showGrid_ = true, showBoxes_ = true;
    bool busy_ = false, loaded_ = false, fitPending_ = false, timerArmed_ = false;
    int sideSel_ = 0;
    float splitFrac_ = 0.5f;
    QElapsedTimer timer_;
    std::atomic<quint64> lastDevToken_{0}, lastChangesToken_{0};

    Scene3D scene_;
    Camera3D camera_;
    ViewInfo leftInfo_, rightInfo_;
    struct Sum { float max = 0, mean = 0, changed = 0, tol = 0, range = 1; } summary_;
    std::vector<float> hist_;
    std::vector<RegionInfo> regions_;
    quint32 regionsTotal_ = 0;
    int selectedRegion_ = -1;
    double removedMm3_ = 0, addedMm3_ = 0;
    bool truncated_ = false, hasChanges_ = false;

    QToolBar* toolbar_ = nullptr;
    PathSelector *leftSelector_ = nullptr, *rightSelector_ = nullptr;
    QLabel *leftInfoLabel_ = nullptr, *rightInfoLabel_ = nullptr;
    Model3dViewport *vpA_ = nullptr, *vpB_ = nullptr;
    M3HeatLegend* heatLegend_ = nullptr;
    M3ChangesLegend* changesLegend_ = nullptr;
    QWidget* panel_ = nullptr;
    QLabel *sumMax_ = nullptr, *sumMean_ = nullptr, *sumChanged_ = nullptr, *sumTol_ = nullptr;
    QLabel *statA_[7] = {}, *statB_[7] = {}, *statD_[7] = {};
    QLabel* regionsTitle_ = nullptr;
    QListWidget* regionList_ = nullptr;
    QSlider *tolSlider_ = nullptr, *sectionSlider_ = nullptr, *overlaySlider_ = nullptr;
    QLabel *tolLabel_ = nullptr, *sideLabel_ = nullptr, *statusLabel_ = nullptr, *statsLabel_ = nullptr, *timeLabel_ = nullptr, *regionNavLabel_ = nullptr;
    QCheckBox* sectionCheck_ = nullptr;
    QComboBox* sectionAxis_ = nullptr;
    QWidget *overlayBox_ = nullptr, *sideBox_ = nullptr;
    QDoubleSpinBox *offX_ = nullptr, *offY_ = nullptr, *offZ_ = nullptr;

    QActionGroup* modeGroup_ = nullptr;
    QAction* modeActions_[6] = {};
    QAction* alignActions_[4] = {};
    QAction* rangeActions_[4] = {};
    QAction *actWire_ = nullptr, *actSwap_ = nullptr, *actReload_ = nullptr, *actFit_ = nullptr, *actPanel_ = nullptr;
    QAction *actPrevRegion_ = nullptr, *actNextRegion_ = nullptr, *actGpu_ = nullptr, *actChangesVol_ = nullptr;
};

}  // namespace openbc::app
