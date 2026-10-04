// model3d_viewport.h
// ---------------------------------------------------------------------------
// OpenGL viewport for the 3D Compare tab. It only *draws*: meshes, per-vertex deviation and the
// camera are handed in; all geometry/compare work happens in the Rust backend.
//
//  * QOpenGLWidget + QOpenGLExtraFunctions (GL 3.3 core), header-only, no Q_OBJECT
//    (needs Qt6::OpenGLWidgets; QtGui provides the functions class).
//  * Flat shading from screen-space derivatives (meshes carry no normals).
//  * Layers: Solid / Heat (deviation ramp, tolerance band stays neutral) / Flat colour,
//    optional alpha and wireframe per layer. Opaque layers first, then blended ones.
//  * Each viewport uploads only the mesh slots it draws (contexts are not shared).
//  * One Camera3D is shared by all viewports of the tab so 2-Up / Split stay in lock-step.
// ---------------------------------------------------------------------------
#pragma once

#include <QColor>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QOpenGLExtraFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>
#include <QPainter>
#include <QSurfaceFormat>
#include <QVector3D>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace openbc::app {

enum MeshSlot { kSlotA = 0, kSlotB = 1, kSlotRemoved = 2, kSlotAdded = 3, kSlotCount = 4 };

// CPU copy of one mesh, shared between the viewports of a tab. Bump the versions on change.
struct Mesh3dData {
    std::vector<float> positions;          // xyz
    std::vector<std::uint32_t> indices;    // triangles
    std::vector<std::uint32_t> edges;      // index pairs (wireframe)
    std::vector<float> deviation;          // per vertex, optional
    quint64 version = 1;
    quint64 devVersion = 1;
};
using Mesh3dPtr = std::shared_ptr<Mesh3dData>;

struct Scene3D {
    Mesh3dPtr slot[kSlotCount];
    QVector3D shift;                       // display shift applied to every mesh
    QVector3D boundsMin{-1, -1, -1}, boundsMax{1, 1, 1};
};

struct Camera3D {
    float yaw = -0.9f, pitch = 0.45f, dist = 4.0f;
    QVector3D target;
    float radius = 1.0f;                   // scene radius, drives clip planes / zoom limits
    void fit(const QVector3D& mn, const QVector3D& mx) {
        target = (mn + mx) * 0.5f;
        radius = std::max(1e-3f, (mx - mn).length() * 0.5f);
        dist = radius * 3.0f;
    }
};

enum class Shade { Solid, Heat, Flat };
struct Layer {
    int slot = kSlotA;
    Shade shade = Shade::Solid;
    QColor color{0x9a, 0x9a, 0x9a};
    float alpha = 1.0f;
    bool wire = false;                     // also draw this layer's edges
};

struct RegionBox { QVector3D min, max; };

class Model3dViewport : public QOpenGLWidget, protected QOpenGLExtraFunctions {
public:
    std::function<void()> onCameraChanged;
    std::function<void(const QVector3D& origin, const QVector3D& dir)> onPick;   // display space, dir normalised
    std::function<void(float fraction)> onSplitMoved;
    std::function<void()> onResized;

    Model3dViewport(Scene3D* scene, Camera3D* camera, QWidget* parent = nullptr) : QOpenGLWidget(parent), scene_(scene), cam_(camera) {
        QSurfaceFormat f;
        f.setVersion(3, 3);
        f.setProfile(QSurfaceFormat::CoreProfile);
        f.setDepthBufferSize(24);
        f.setSamples(4);
        setFormat(f);
        setMinimumSize(240, 200);
        setMouseTracking(true);
    }
    ~Model3dViewport() override {
        makeCurrent();
        for (auto& g : gl_) destroyMesh(g);
        if (gridVbo_) glDeleteBuffers(1, &gridVbo_);
        if (gridVao_) glDeleteVertexArrays(1, &gridVao_);
        doneCurrent();
    }

    // ----- scene description (cheap setters; each schedules a repaint) -----------------------
    void setLayers(std::vector<Layer> layers) { layers_ = std::move(layers); update(); }
    void setSplit(bool on, std::vector<Layer> rightLayers = {}, float fraction = 0.5f) {
        split_ = on; rightLayers_ = std::move(rightLayers); splitFrac_ = fraction; update();
    }
    void setBadge(const QString& text) { badge_ = text; update(); }
    void setTolerance(float tol, float range) { tol_ = tol; range_ = range; update(); }
    void setSection(bool on, int axis, float pos) { sectionOn_ = on; sectionAxis_ = axis; sectionPos_ = pos; update(); }
    void setRegionBoxes(std::vector<RegionBox> boxes, int selected) { boxes_ = std::move(boxes); selectedBox_ = selected; update(); }
    void setShowGrid(bool on) { showGrid_ = on; update(); }
    void setShowBoxes(bool on) { showBoxes_ = on; update(); }
    void setProbe(const std::optional<QVector3D>& point, const QString& label) { probe_ = point; probeLabel_ = label; update(); }
    void meshChanged() { update(); }

protected:
    // ----- GL lifecycle ------------------------------------------------------------------------
    void initializeGL() override {
        initializeOpenGLFunctions();
        prog_ = std::make_unique<QOpenGLShaderProgram>();
        prog_->addShaderFromSourceCode(QOpenGLShader::Vertex, R"(#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in float aDev;
uniform mat4 uVP;
uniform vec3 uShift;
out vec3 vWorld;
out float vDev;
void main() { vec3 w = aPos + uShift; vWorld = w; vDev = aDev; gl_Position = uVP * vec4(w, 1.0); }
)");
        prog_->addShaderFromSourceCode(QOpenGLShader::Fragment, R"(#version 330 core
in vec3 vWorld;
in float vDev;
uniform int uMode;          // 0 solid, 1 heat, 2 flat
uniform vec4 uColor;
uniform vec3 uEye;
uniform float uTol;
uniform float uRange;
uniform vec4 uClip;
uniform int uClipOn;
out vec4 frag;
void main() {
    if (uClipOn != 0 && dot(vec4(vWorld, 1.0), uClip) < 0.0) discard;
    if (uMode == 2) { frag = uColor; return; }
    vec3 n = normalize(cross(dFdx(vWorld), dFdy(vWorld)));
    vec3 v = normalize(uEye - vWorld);
    if (dot(n, v) < 0.0) n = -n;
    float l = 0.30 + 0.70 * max(dot(n, normalize(v + vec3(0.25, 0.35, 0.6))), 0.0);
    vec3 base = uColor.rgb;
    if (uMode == 1 && abs(vDev) > uTol) {
        float t = 0.30 + 0.70 * clamp(abs(vDev) / uRange, 0.0, 1.0);
        vec3 hot = vDev > 0.0 ? vec3(0.90, 0.18, 0.14) : vec3(0.12, 0.32, 0.92);
        base = mix(uColor.rgb, hot, t);
    }
    frag = vec4(base * l, uColor.a);
}
)");
        prog_->link();
        glGenVertexArrays(1, &gridVao_);
        glGenBuffers(1, &gridVbo_);
        glBindVertexArray(gridVao_);
        glBindBuffer(GL_ARRAY_BUFFER, gridVbo_);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
        glDisableVertexAttribArray(1);
        glVertexAttrib1f(1, 0.0f);
        glBindVertexArray(0);
    }

    void paintGL() override {
        const qreal dpr = devicePixelRatioF();
        const int W = int(width() * dpr), H = int(height() * dpr);
        glViewport(0, 0, W, H);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0.15f, 0.155f, 0.18f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        view_ = viewMatrix();
        proj_ = projMatrix();
        const QMatrix4x4 vp = proj_ * view_;
        const QVector3D eye = eyePosition();

        prog_->bind();
        prog_->setUniformValue("uVP", vp);
        prog_->setUniformValue("uEye", eye);
        prog_->setUniformValue("uTol", tol_);
        prog_->setUniformValue("uRange", std::max(range_, 1e-6f));
        setClip();

        if (split_) {
            const int x = int(W * splitFrac_);
            glEnable(GL_SCISSOR_TEST);
            glScissor(0, 0, x, H);
            clearDepth();
            drawLayers(layers_);
            glScissor(x, 0, W - x, H);
            clearDepth();
            drawLayers(rightLayers_);
            glDisable(GL_SCISSOR_TEST);
        } else {
            drawLayers(layers_);
        }
        if (showBoxes_) drawBoxes();
        prog_->release();
        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
        paintOverlay(vp);
    }

    void resizeGL(int, int) override { if (onResized) onResized(); }

    // ----- interaction -------------------------------------------------------------------------
    void mousePressEvent(QMouseEvent* e) override {
        last_ = e->position();
        pressed_ = e->position();
        moved_ = false;
        if (split_ && e->button() == Qt::LeftButton && std::abs(e->position().x() - splitFrac_ * width()) < 6) draggingSplit_ = true;
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        const QPointF d = e->position() - last_;
        last_ = e->position();
        if ((e->position() - pressed_).manhattanLength() > 4) moved_ = true;
        if (draggingSplit_) {
            splitFrac_ = std::clamp(float(e->position().x() / width()), 0.05f, 0.95f);
            if (onSplitMoved) onSplitMoved(splitFrac_);
            update();
            return;
        }
        if (split_) setCursor(std::abs(e->position().x() - splitFrac_ * width()) < 6 ? Qt::SplitHCursor : Qt::ArrowCursor);
        if (e->buttons() & Qt::LeftButton) {
            cam_->yaw -= float(d.x()) * 0.008f;
            cam_->pitch = std::clamp(cam_->pitch + float(d.y()) * 0.008f, -1.5f, 1.5f);
            changed();
        } else if (e->buttons() & (Qt::MiddleButton | Qt::RightButton)) {
            const QVector3D fwd = (cam_->target - eyePosition()).normalized();
            const QVector3D right = QVector3D::crossProduct(fwd, QVector3D(0, 0, 1)).normalized();
            const QVector3D up = QVector3D::crossProduct(right, fwd);
            const float k = 2.0f * cam_->dist * std::tan(kFov * 0.5f) / std::max(1, height());
            cam_->target += (-right * float(d.x()) + up * float(d.y())) * k;
            changed();
        }
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        if (draggingSplit_) { draggingSplit_ = false; return; }
        if (e->button() == Qt::LeftButton && !moved_ && onPick) {
            const float nx = float(2.0 * e->position().x() / width() - 1.0);
            const float ny = float(1.0 - 2.0 * e->position().y() / height());
            bool ok = false;
            const QMatrix4x4 inv = (proj_ * view_).inverted(&ok);
            if (!ok) return;
            const QVector3D a = inv * QVector3D(nx, ny, -1.0f), b = inv * QVector3D(nx, ny, 1.0f);
            onPick(a, (b - a).normalized());
        }
    }
    void mouseDoubleClickEvent(QMouseEvent*) override {
        cam_->fit(scene_->boundsMin + scene_->shift, scene_->boundsMax + scene_->shift);
        changed();
    }
    void wheelEvent(QWheelEvent* e) override {
        const float steps = float(e->angleDelta().y()) / 120.0f;
        cam_->dist = std::clamp(cam_->dist * std::pow(0.9f, steps), cam_->radius * 0.05f, cam_->radius * 40.0f);
        changed();
        e->accept();
    }

private:
    static constexpr float kFov = 0.61f;   // ~35 degrees

    struct GlMesh {
        GLuint vbo = 0, dev = 0, ibo = 0, ebo = 0, vaoTri = 0, vaoLine = 0;
        GLsizei triCount = 0, edgeCount = 0;
        quint64 ver = 0, devVer = 0;
        bool hasDev = false;
    };

    void changed() { if (onCameraChanged) onCameraChanged(); update(); }
    void clearDepth() { glClear(GL_DEPTH_BUFFER_BIT); }

    QVector3D eyePosition() const {
        return cam_->target + cam_->dist * QVector3D(std::cos(cam_->pitch) * std::cos(cam_->yaw), std::cos(cam_->pitch) * std::sin(cam_->yaw), std::sin(cam_->pitch));
    }
    QMatrix4x4 viewMatrix() const { QMatrix4x4 m; m.lookAt(eyePosition(), cam_->target, QVector3D(0, 0, 1)); return m; }
    QMatrix4x4 projMatrix() const {
        QMatrix4x4 m;
        const float nearP = std::max(cam_->dist * 0.02f, cam_->radius * 0.002f);
        m.perspective(kFov * 57.29578f, float(width()) / std::max(1, height()), nearP, cam_->dist + cam_->radius * 6.0f);
        return m;
    }

    void setClip() {
        QVector4D c(0, 0, 0, 0);
        if (sectionOn_) {                                   // keep coordinate <= pos
            const float pos = sectionPos_ + scene_->shift[sectionAxis_];
            c[sectionAxis_] = -1.0f;
            c[3] = pos;
        }
        prog_->setUniformValue("uClip", c);
        prog_->setUniformValue("uClipOn", sectionOn_ ? 1 : 0);
    }

    void destroyMesh(GlMesh& g) {
        GLuint bufs[4] = {g.vbo, g.dev, g.ibo, g.ebo};
        glDeleteBuffers(4, bufs);
        if (g.vaoTri) glDeleteVertexArrays(1, &g.vaoTri);
        if (g.vaoLine) glDeleteVertexArrays(1, &g.vaoLine);
        g = GlMesh();
    }

    // Uploads lazily: a slot nobody draws in this viewport never touches GPU memory.
    bool ensureMesh(int slot) {
        GlMesh& g = gl_[slot];
        const Mesh3dPtr& d = scene_->slot[slot];
        if (!d || d->positions.empty() || d->indices.empty()) return false;
        if (g.ver != d->version) {
            destroyMesh(g);
            glGenBuffers(1, &g.vbo); glGenBuffers(1, &g.dev); glGenBuffers(1, &g.ibo); glGenBuffers(1, &g.ebo);
            glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
            glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(d->positions.size() * sizeof(float)), d->positions.data(), GL_STATIC_DRAW);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.ibo);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(d->indices.size() * 4), d->indices.data(), GL_STATIC_DRAW);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.ebo);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(d->edges.size() * 4), d->edges.data(), GL_STATIC_DRAW);
            g.triCount = GLsizei(d->indices.size());
            g.edgeCount = GLsizei(d->edges.size());
            g.ver = d->version;
            g.devVer = 0;
            glGenVertexArrays(1, &g.vaoTri);
            glGenVertexArrays(1, &g.vaoLine);
        }
        if (g.devVer != d->devVersion) {
            g.hasDev = d->deviation.size() * 3 == d->positions.size();
            glBindBuffer(GL_ARRAY_BUFFER, g.dev);
            if (g.hasDev) glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(d->deviation.size() * 4), d->deviation.data(), GL_STATIC_DRAW);
            g.devVer = d->devVersion;
            // VAOs capture the attribute layout, so rebuild them whenever the dev buffer toggles.
            for (int pass = 0; pass < 2; ++pass) {
                glBindVertexArray(pass == 0 ? g.vaoTri : g.vaoLine);
                glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
                glEnableVertexAttribArray(0);
                glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
                if (g.hasDev) {
                    glBindBuffer(GL_ARRAY_BUFFER, g.dev);
                    glEnableVertexAttribArray(1);
                    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, 0, nullptr);
                } else {
                    glDisableVertexAttribArray(1);
                    glVertexAttrib1f(1, 0.0f);
                }
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, pass == 0 ? g.ibo : g.ebo);
            }
            glBindVertexArray(0);
        }
        return true;
    }

    void setColor(const QColor& c, float alpha) { prog_->setUniformValue("uColor", QVector4D(float(c.redF()), float(c.greenF()), float(c.blueF()), alpha)); }

    void drawLayers(const std::vector<Layer>& layers) {
        prog_->setUniformValue("uShift", scene_->shift);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        if (showGrid_) drawGrid();
        for (int pass = 0; pass < 2; ++pass) {                 // 0 opaque, 1 blended
            if (pass == 1) { glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glDepthMask(GL_FALSE); }
            for (const Layer& l : layers) {
                if ((l.alpha < 0.999f) != (pass == 1)) continue;
                if (!ensureMesh(l.slot)) continue;
                GlMesh& g = gl_[l.slot];
                prog_->setUniformValue("uMode", l.shade == Shade::Heat ? 1 : l.shade == Shade::Flat ? 2 : 0);
                setColor(l.color, l.alpha);
                glEnable(GL_POLYGON_OFFSET_FILL);
                glPolygonOffsetCompat(1.0f, 1.0f);
                glBindVertexArray(g.vaoTri);
                glDrawElements(GL_TRIANGLES, g.triCount, GL_UNSIGNED_INT, nullptr);
                glDisable(GL_POLYGON_OFFSET_FILL);
                if (l.wire && g.edgeCount > 0) {
                    prog_->setUniformValue("uMode", 2);
                    setColor(QColor(0x30, 0x32, 0x3c), std::min(1.0f, l.alpha + 0.35f));
                    glBindVertexArray(g.vaoLine);
                    glDrawElements(GL_LINES, g.edgeCount, GL_UNSIGNED_INT, nullptr);
                }
            }
        }
        glBindVertexArray(0);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }

    void glPolygonOffsetCompat(float f, float u) { glPolygonOffset(f, u); }

    void drawLines(const std::vector<float>& pts, const QColor& c, bool depth) {
        if (pts.empty()) return;
        prog_->setUniformValue("uMode", 2);
        prog_->setUniformValue("uShift", QVector3D(0, 0, 0));
        setColor(c, float(c.alphaF()));
        if (!depth) glDisable(GL_DEPTH_TEST);
        glBindVertexArray(gridVao_);
        glBindBuffer(GL_ARRAY_BUFFER, gridVbo_);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(pts.size() * sizeof(float)), pts.data(), GL_STREAM_DRAW);
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDrawArrays(GL_LINES, 0, GLsizei(pts.size() / 3));
        glBindVertexArray(0);
        if (!depth) glEnable(GL_DEPTH_TEST);
        prog_->setUniformValue("uShift", scene_->shift);
    }

    static double niceStep(double raw) {
        const double p = std::pow(10.0, std::floor(std::log10(std::max(raw, 1e-9))));
        const double f = raw / p;
        return (f < 1.5 ? 1.0 : f < 3.5 ? 2.0 : f < 7.5 ? 5.0 : 10.0) * p;
    }

    void drawGrid() {
        const QVector3D mn = scene_->boundsMin + scene_->shift, mx = scene_->boundsMax + scene_->shift;
        const double half = cam_->radius * 3.0, step = niceStep(cam_->radius / 4.0);
        const QVector3D c = (mn + mx) * 0.5f;
        const double x0 = std::floor((c.x() - half) / step) * step, y0 = std::floor((c.y() - half) / step) * step;
        std::vector<float> v;
        const float z = mn.z();
        for (double x = x0; x <= c.x() + half; x += step) { v.insert(v.end(), {float(x), float(y0), z, float(x), float(c.y() + half), z}); }
        for (double y = y0; y <= c.y() + half; y += step) { v.insert(v.end(), {float(x0), float(y), z, float(c.x() + half), float(y), z}); }
        // grid is already in display space: draw with zero shift (drawLines resets it)
        drawLines(v, QColor(255, 255, 255, 28), true);
    }

    void drawBoxes() {
        glEnable(GL_BLEND);
        for (int pass = 0; pass < 2; ++pass) {
            std::vector<float> v;
            for (int i = 0; i < int(boxes_.size()); ++i) {
                if ((i == selectedBox_) != (pass == 1)) continue;
                const QVector3D a = boxes_[i].min + scene_->shift, b = boxes_[i].max + scene_->shift;
                const QVector3D p[8] = {{a.x(), a.y(), a.z()}, {b.x(), a.y(), a.z()}, {b.x(), b.y(), a.z()}, {a.x(), b.y(), a.z()},
                                        {a.x(), a.y(), b.z()}, {b.x(), a.y(), b.z()}, {b.x(), b.y(), b.z()}, {a.x(), b.y(), b.z()}};
                static const int e[12][2] = {{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
                for (auto& ed : e) for (int k : {0, 1}) v.insert(v.end(), {p[ed[k]].x(), p[ed[k]].y(), p[ed[k]].z()});
            }
            drawLines(v, pass == 1 ? QColor(0xff, 0xc0, 0x30, 255) : QColor(0xf0, 0xa8, 0x20, 150), false);
        }
    }

    // ---- 2D overlay: axis gizmo, badge, split handle, probe -----------------------------------
    QPointF project(const QMatrix4x4& vp, const QVector3D& w) const {
        const QVector4D c = vp * QVector4D(w, 1.0f);
        if (c.w() <= 1e-6f) return QPointF(-1e4, -1e4);
        return QPointF((c.x() / c.w() * 0.5 + 0.5) * width(), (1.0 - (c.y() / c.w() * 0.5 + 0.5)) * height());
    }

    void paintOverlay(const QMatrix4x4& vp) {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        QFont f = font(); f.setPointSizeF(f.pointSizeF() * 0.9); p.setFont(f);

        if (split_) {
            const int x = int(width() * splitFrac_);
            p.setPen(QPen(QColor(0x4e, 0x9c, 0xff), 2)); p.drawLine(x, 0, x, height());
            p.setBrush(QColor(0x4e, 0x9c, 0xff)); p.setPen(Qt::NoPen); p.drawRoundedRect(QRectF(x - 5, height() / 2.0 - 14, 10, 28), 4, 4);
        }
        if (!badge_.isEmpty()) {
            const QFontMetrics fm(p.font());
            const QRect r(8, 8, fm.horizontalAdvance(badge_) + 18, fm.height() + 8);
            p.setPen(Qt::NoPen); p.setBrush(QColor(0, 0, 0, 150)); p.drawRoundedRect(r, 4, 4);
            p.setPen(Qt::white); p.drawText(r, Qt::AlignCenter, badge_);
        }
        // axis gizmo (Z up), drawn from the rotation part of the view matrix
        {
            const QPointF o(46, height() - 52);
            struct Ax { QVector3D v; QColor c; const char* n; };
            Ax axes[3] = {{{1, 0, 0}, QColor(0xf0, 0x30, 0x58), "X"}, {{0, 1, 0}, QColor(0x30, 0xd0, 0x78), "Y"}, {{0, 0, 1}, QColor(0x30, 0x80, 0xf8), "Z"}};
            std::sort(std::begin(axes), std::end(axes), [&](const Ax& a, const Ax& b) { return view_.mapVector(a.v).z() < view_.mapVector(b.v).z(); });
            for (const Ax& a : axes) {
                const QVector3D s = view_.mapVector(a.v);
                const QPointF t = o + QPointF(s.x() * 26, -s.y() * 26);
                p.setPen(QPen(a.c, 2)); p.drawLine(o, t);
                p.setBrush(a.c); p.setPen(Qt::NoPen); p.drawEllipse(t, 8, 8);
                p.setPen(Qt::white); p.drawText(QRectF(t.x() - 8, t.y() - 8, 16, 16), Qt::AlignCenter, a.n);
            }
        }
        if (probe_) {
            const QPointF s = project(vp, *probe_ + scene_->shift);
            p.setPen(QPen(QColor(0xff, 0xff, 0xff), 1.5)); p.setBrush(Qt::NoBrush);
            p.drawEllipse(s, 6, 6); p.drawLine(s - QPointF(10, 0), s + QPointF(10, 0)); p.drawLine(s - QPointF(0, 10), s + QPointF(0, 10));
            if (!probeLabel_.isEmpty()) {
                const QFontMetrics fm(p.font());
                const QRectF r(s.x() + 12, s.y() - 30, fm.horizontalAdvance(probeLabel_) + 14, fm.height() + 6);
                p.setPen(Qt::NoPen); p.setBrush(QColor(0, 0, 0, 190)); p.drawRoundedRect(r, 4, 4);
                p.setPen(Qt::white); p.drawText(r, Qt::AlignCenter, probeLabel_);
            }
        }
    }

    Scene3D* scene_;
    Camera3D* cam_;
    std::unique_ptr<QOpenGLShaderProgram> prog_;
    GlMesh gl_[kSlotCount];
    GLuint gridVao_ = 0, gridVbo_ = 0;
    QMatrix4x4 view_, proj_;
    std::vector<Layer> layers_, rightLayers_;
    bool split_ = false; float splitFrac_ = 0.5f; bool draggingSplit_ = false;
    QString badge_;
    float tol_ = 0.05f, range_ = 1.0f;
    bool sectionOn_ = false; int sectionAxis_ = 2; float sectionPos_ = 0.0f;
    std::vector<RegionBox> boxes_; int selectedBox_ = -1;
    bool showGrid_ = true, showBoxes_ = true;
    std::optional<QVector3D> probe_; QString probeLabel_;
    QPointF last_, pressed_; bool moved_ = false;
};

}  // namespace openbc::app
