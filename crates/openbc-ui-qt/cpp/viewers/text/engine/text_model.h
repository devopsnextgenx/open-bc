// text_model.h
// ---------------------------------------------------------------------------
// C++ face of the Rust text comparison engine. TextModel is the ONE object both text editors
// (the QPlainTextEdit one and VirtualTextEditor) render from. It owns an opaque handle, never any
// file content, and is safe to read from the GUI thread while no operation is running.
//
//   * TextModel::openAsync()  builds the model on a Rust worker thread; the result is posted back
//     to the GUI thread through a queued QObject (see TextModelLoader).
//   * Text is fetched lazily per line / per window; nothing here caches whole files.
//   * SyntaxSession wraps the stateful, checkpointed highlighter (bounded caches live in Rust).
//
// This header is Qt Core only (QString/QObject); it does not touch QtWidgets or QtGui.
// ---------------------------------------------------------------------------
#pragma once

#include <QByteArray>
#include <QCoreApplication>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QString>
#include <QVector>
#include <cstdint>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>

#include "viewers/text/engine/text_engine_api.h"

namespace openbc::app {

enum class Side : std::uint8_t { Left = 0, Right = 1 };

enum class RowKind : std::uint8_t { Equal = 0, Insert = 1, Delete = 2, Replace = 3 };

struct ModelRow {
    qint64 left = -1;    // zero-based, -1 = filler
    qint64 right = -1;
    RowKind kind = RowKind::Equal;
    bool changed() const { return kind != RowKind::Equal; }
};

struct LineWindow {
    QString text;
    quint64 startChar = 0;
    bool moreAfter = false;
    quint32 lineBytes = 0;
};

// Character range inside a decoded line; byte offsets (UTF-8) converted to UTF-16 units.
struct InlineRange {
    int start = 0;     // QString index
    int length = 0;
    bool whitespaceOnly = false;
};

struct StyledSpan {
    int start = 0;   // QString index
    int length = 0;
    quint32 foreground = 0;   // 0xRRGGBBAA
    bool bold = false;
    bool italic = false;
};

struct FindHit {
    quint64 line = 0;
    int start = 0;    // QString index in the decoded line
    int length = 0;
};

struct ModelOptions {
    bool ignoreWhitespaceEdges = false;
    bool ignoreAllWhitespace = false;
    bool ignoreCase = false;
    bool ignoreLineEndings = true;
    bool bufferedIo = false;
};

class TextModel {
public:
    ~TextModel() { if (handle_) openbc_tm_destroy(handle_); }
    TextModel(const TextModel&) = delete;
    TextModel& operator=(const TextModel&) = delete;

    // Blocking; for tests and tiny inputs. Returns null and sets `error` on failure.
    static std::shared_ptr<TextModel> openSync(const QString& left, const QString& right,
                                               const ModelOptions& options = {}, QString* error = nullptr) {
        const QByteArray l = left.toUtf8(), r = right.toUtf8();
        const OpenBcTmOptions o = toC(options);
        OpenBcTmModel* h = openbc_tm_open_sync(l.isEmpty() ? nullptr : l.constData(),
                                               r.isEmpty() ? nullptr : r.constData(), &o);
        if (!h) {
            if (error) {
                std::size_t n = 0;
                const std::uint8_t* p = openbc_tm_last_error(&n);
                *error = QString::fromUtf8(reinterpret_cast<const char*>(p), static_cast<int>(n));
            }
            return nullptr;
        }
        return std::shared_ptr<TextModel>(new TextModel(h));
    }

    // ---- structure -------------------------------------------------------------------------
    quint64 rowCount() const { return openbc_tm_row_count(handle_); }
    quint64 lineCount(Side s) const { return openbc_tm_line_count(handle_, static_cast<std::uint8_t>(s)); }

    ModelRow row(quint64 r) const {
        OpenBcTmRow c{};
        ModelRow out;
        if (openbc_tm_row(handle_, r, &c)) { out.left = c.left; out.right = c.right; out.kind = static_cast<RowKind>(c.kind); }
        return out;
    }

    std::optional<quint64> nextDifference(quint64 row) const {
        std::uint64_t v = 0;
        if (openbc_tm_next_diff(handle_, row, &v)) return v;
        return std::nullopt;
    }
    std::optional<quint64> prevDifference(quint64 row) const {
        std::uint64_t v = 0;
        if (openbc_tm_prev_diff(handle_, row, &v)) return v;
        return std::nullopt;
    }
    std::optional<quint64> rowForLine(Side s, quint64 line) const {
        std::uint64_t v = 0;
        if (openbc_tm_row_for_line(handle_, static_cast<std::uint8_t>(s), line, &v)) return v;
        return std::nullopt;
    }
    OpenBcTmStats stats() const { OpenBcTmStats s{}; openbc_tm_stats(handle_, &s); return s; }
    // Change summary per overview pixel: bit0 inserted, bit1 deleted, bit2 replaced.
    QByteArray overview(int buckets) const {
        QByteArray out(qMax(0, buckets), '\0');
        if (buckets > 0) openbc_tm_overview(handle_, static_cast<std::size_t>(buckets), reinterpret_cast<std::uint8_t*>(out.data()));
        return out;
    }
    OpenBcTmMemory memory() const { OpenBcTmMemory m{}; openbc_tm_memory(handle_, &m); return m; }
    bool verifyUnchanged() const { return openbc_tm_verify(handle_) != 0; }
    // 0 UTF-8, 1 UTF-16 LE, 2 UTF-16 BE; |0x80 when the file looks binary.
    int encodingCode(Side s) const { return openbc_tm_encoding(handle_, static_cast<std::uint8_t>(s)); }
    qint64 lineBytes(Side s, quint64 line) const { return openbc_tm_line_bytes(handle_, static_cast<std::uint8_t>(s), line); }

    // ---- text ------------------------------------------------------------------------------
    // At most `maxChars` characters starting at character `startChar`. A QByteArray scratch is
    // reused by the caller-provided buffer to avoid per-line allocation churn.
    LineWindow window(Side s, quint64 line, quint64 startChar, quint32 maxChars) const {
        LineWindow w;
        QByteArray buf;
        buf.resize(static_cast<int>(qMin<quint64>(static_cast<quint64>(maxChars) * 4u + 8u, 64u << 20)));
        OpenBcTmWindow c{};
        if (!openbc_tm_line_window(handle_, static_cast<std::uint8_t>(s), line, startChar, maxChars,
                                   reinterpret_cast<std::uint8_t*>(buf.data()), static_cast<std::size_t>(buf.size()), &c))
            return w;
        w.text = QString::fromUtf8(buf.constData(), static_cast<int>(c.written));
        w.startChar = c.startChar;
        w.moreAfter = c.moreAfter != 0;
        w.lineBytes = c.lineBytes;
        return w;
    }

    // Character-level differences for one side of a replace row, as QString index ranges relative
    // to the FULL decoded line (`fullText` must be that line, i.e. a window starting at 0).
    QVector<InlineRange> inlineRanges(quint64 row, Side s, const QString& fullText) const {
        QVector<InlineRange> out;
        OpenBcTmRange tmp[64];
        const std::int32_t n = openbc_tm_inline(handle_, row, static_cast<std::uint8_t>(s), tmp, 64);
        if (n <= 0) return out;
        QVector<OpenBcTmRange> all(n);
        if (n <= 64) std::copy(tmp, tmp + n, all.begin());
        else openbc_tm_inline(handle_, row, static_cast<std::uint8_t>(s), all.data(), static_cast<std::size_t>(n));
        const QByteArray utf8 = fullText.toUtf8();
        for (const OpenBcTmRange& r : all) {
            if (r.kind == 0) continue;
            const int a = QString::fromUtf8(utf8.constData(), static_cast<int>(qMin<quint32>(r.start, utf8.size()))).size();
            const int b = QString::fromUtf8(utf8.constData(), static_cast<int>(qMin<quint32>(r.start + r.len, utf8.size()))).size();
            out.push_back({a, b - a, r.kind == 2});
        }
        return out;
    }

    // Search one side; start line < 0 begins at the file start (forward) or end (backward).
    std::optional<FindHit> find(Side s, const QString& needle, bool caseSensitive, qint64 startLine,
                                int startCol, bool forward, const OpenBcTmCancel* cancel = nullptr,
                                bool* failed = nullptr) const {
        const QByteArray n = needle.toUtf8();
        OpenBcTmMatch m{};
        // `startCol` is a QString index; the engine wants a UTF-8 byte offset in the decoded line.
        std::uint64_t byteCol = 0;
        if (startLine >= 0 && startCol > 0) {
            const LineWindow w = window(s, static_cast<quint64>(startLine), 0, 1u << 20);
            byteCol = static_cast<std::uint64_t>(w.text.left(startCol).toUtf8().size());
        }
        const std::int32_t rc = openbc_tm_find(handle_, static_cast<std::uint8_t>(s),
                                               reinterpret_cast<const std::uint8_t*>(n.constData()),
                                               static_cast<std::size_t>(n.size()), caseSensitive, startLine,
                                               byteCol, forward, cancel, &m);
        if (failed) *failed = rc < 0;
        if (rc != 1) return std::nullopt;
        const LineWindow w = window(s, m.line, 0, 1u << 20);
        const QByteArray lineUtf8 = w.text.toUtf8();
        FindHit hit;
        hit.line = m.line;
        hit.start = QString::fromUtf8(lineUtf8.constData(), static_cast<int>(qMin<std::uint64_t>(m.start, lineUtf8.size()))).size();
        hit.length = QString::fromUtf8(lineUtf8.constData() + qMin<std::uint64_t>(m.start, lineUtf8.size()),
                                       static_cast<int>(qMin<std::uint64_t>(m.len, lineUtf8.size() - qMin<std::uint64_t>(m.start, lineUtf8.size())))).size();
        return hit;
    }

    const OpenBcTmModel* handle() const { return handle_; }

private:
    explicit TextModel(OpenBcTmModel* h) : handle_(h) {}
    friend class TextModelLoader;

    static OpenBcTmOptions toC(const ModelOptions& o) {
        OpenBcTmOptions c{};
        c.ignoreWhitespaceEdges = o.ignoreWhitespaceEdges;
        c.ignoreAllWhitespace = o.ignoreAllWhitespace;
        c.ignoreCase = o.ignoreCase;
        c.ignoreLineEndings = o.ignoreLineEndings;
        c.bufferedIo = o.bufferedIo;
        return c;
    }
    static OpenBcTmOptions optionsToC(const ModelOptions& o) { return toC(o); }

    OpenBcTmModel* handle_ = nullptr;
};

// Cancellation handle (RAII).
class Cancellation {
public:
    Cancellation() : h_(openbc_tm_cancel_new()) {}
    ~Cancellation() { openbc_tm_cancel_destroy(h_); }
    Cancellation(const Cancellation&) = delete;
    Cancellation& operator=(const Cancellation&) = delete;
    void cancel() const { openbc_tm_cancel(h_); }
    const OpenBcTmCancel* get() const { return h_; }
private:
    OpenBcTmCancel* h_;
};

// Builds a TextModel on a Rust worker thread and delivers the result on the GUI thread.
//
// Thread-safety design: the worker thread NEVER touches a QObject. It only (a) copies data into a
// std::function and (b) posts it to QCoreApplication::instance() (always alive, post is thread-safe).
// The posted function runs on the GUI thread, where it checks the shared `State` (alive flag +
// generation) before touching the loader. Destroying the loader (or starting a new load) therefore
// makes late results harmless: they are discarded and their model handle is freed.
class TextModelLoader : public QObject {
public:
    explicit TextModelLoader(QObject* parent = nullptr) : QObject(parent), state_(std::make_shared<State>()) {
        state_->self = this;
    }
    ~TextModelLoader() override {
        cancel();
        state_->alive = false;      // GUI thread only; late callbacks see this and bail out
    }

    // Callbacks (GUI thread). Assign before start(); each start() supersedes the previous one.
    std::function<void(const QString& phase, quint64 done, quint64 total)> onProgress;
    std::function<void(std::shared_ptr<TextModel> model)> onFinished;
    std::function<void(const QString& message)> onFailed;

    void start(const QString& left, const QString& right, const ModelOptions& options) {
        cancel();
        cancellation_ = std::make_shared<Cancellation>();
        ++state_->generation;
        auto* ctx = new Context{state_, cancellation_, state_->generation};
        const QByteArray l = left.toUtf8(), r = right.toUtf8();
        const OpenBcTmOptions o = TextModel::optionsToC(options);
        openbc_tm_open_async(l.isEmpty() ? nullptr : l.constData(), r.isEmpty() ? nullptr : r.constData(), &o,
                             cancellation_->get(), &TextModelLoader::onEvent, ctx);
    }

    void cancel() { if (cancellation_) cancellation_->cancel(); }

private:
    struct State {
        bool alive = true;          // read/written on the GUI thread only
        int generation = 0;         // ditto
        TextModelLoader* self = nullptr;
    };
    struct Context {
        std::shared_ptr<State> state;
        std::shared_ptr<Cancellation> keepAlive;   // keeps the cancel handle valid for the worker
        int generation;
    };

    // Runs on the Rust worker thread.
    static void onEvent(void* user, std::int32_t kind, const char* message, void* handle) {
        auto* ctx = static_cast<Context*>(user);
        const bool last = kind == kOpenBcTmEvResult || kind == kOpenBcTmEvError;
        std::shared_ptr<State> state = ctx->state;
        const int generation = ctx->generation;
        std::function<void()> deliver;
        if (kind == kOpenBcTmEvProgress) {
            const QStringList parts = QString::fromUtf8(message).split('|');
            if (parts.size() == 3)
                deliver = [state, generation, parts]() {
                    if (state->alive && state->generation == generation && state->self->onProgress)
                        state->self->onProgress(parts[0], parts[1].toULongLong(), parts[2].toULongLong());
                };
        } else if (kind == kOpenBcTmEvResult) {
            auto model = std::shared_ptr<TextModel>(new TextModel(static_cast<OpenBcTmModel*>(handle)));
            deliver = [state, generation, model]() {
                if (state->alive && state->generation == generation && state->self->onFinished)
                    state->self->onFinished(model);
            };   // when discarded, `model` is released with the std::function -> handle destroyed
        } else if (kind == kOpenBcTmEvError) {
            const QString text = QString::fromUtf8(message);
            deliver = [state, generation, text]() {
                if (state->alive && state->generation == generation && state->self->onFailed)
                    state->self->onFailed(text);
            };
        }
        if (deliver) {
            QCoreApplication* app = QCoreApplication::instance();
            if (app) QMetaObject::invokeMethod(app, std::move(deliver), Qt::QueuedConnection);
        }
        if (last) delete ctx;
    }

    std::shared_ptr<State> state_;
    std::shared_ptr<Cancellation> cancellation_;
};

// Stateful syntax highlighting with bounded caches (all state lives in Rust).
class SyntaxSession {
public:
    SyntaxSession(const QString& language, bool dark, quint32 spanCacheLines = 0) {
        const QByteArray lang = language.toUtf8();
        h_ = openbc_syn_new(reinterpret_cast<const std::uint8_t*>(lang.constData()), static_cast<std::size_t>(lang.size()),
                            dark, spanCacheLines);
    }
    ~SyntaxSession() { openbc_syn_destroy(h_); }
    SyntaxSession(const SyntaxSession&) = delete;
    SyntaxSession& operator=(const SyntaxSession&) = delete;

    void invalidateFrom(quint64 line) { openbc_syn_invalidate(h_, line); }

    // Spans for `count` lines starting at `first`, as QString-indexed spans per line (via the
    // decoded line text provided by `textOf`, needed to convert UTF-8 byte offsets).
    QVector<QVector<StyledSpan>> highlight(const TextModel& model, Side side, quint64 first, quint64 count,
                                           const std::function<QString(quint64)>& textOf) const {
        QVector<OpenBcTmSpan> buf(256);
        std::int64_t n = openbc_syn_range(h_, model.handle(), static_cast<std::uint8_t>(side), first, count,
                                          buf.data(), static_cast<std::size_t>(buf.size()));
        if (n > buf.size()) {
            buf.resize(static_cast<int>(n));
            n = openbc_syn_range(h_, model.handle(), static_cast<std::uint8_t>(side), first, count, buf.data(),
                                 static_cast<std::size_t>(buf.size()));
        }
        return convert(buf, n, first, count, textOf);
    }

    // For documents that are not backed by a TextModel (the QPlainTextEdit editor).
    QVector<QVector<StyledSpan>> highlight(quint64 first, quint64 count, OpenBcTmLineProvider provider, void* user,
                                           const std::function<QString(quint64)>& textOf) const {
        QVector<OpenBcTmSpan> buf(256);
        std::int64_t n = openbc_syn_range_cb(h_, first, count, provider, user, buf.data(), static_cast<std::size_t>(buf.size()));
        if (n > buf.size()) {
            buf.resize(static_cast<int>(n));
            n = openbc_syn_range_cb(h_, first, count, provider, user, buf.data(), static_cast<std::size_t>(buf.size()));
        }
        return convert(buf, n, first, count, textOf);
    }

private:
    static QVector<QVector<StyledSpan>> convert(const QVector<OpenBcTmSpan>& buf, std::int64_t n, quint64 first,
                                                quint64 count, const std::function<QString(quint64)>& textOf) {
        QVector<QVector<StyledSpan>> lines(static_cast<int>(count));
        QVector<QByteArray> utf8(static_cast<int>(count));
        QVector<bool> have(static_cast<int>(count), false);
        for (std::int64_t i = 0; i < n && i < buf.size(); ++i) {
            const OpenBcTmSpan& s = buf[static_cast<int>(i)];
            if (s.line < first || s.line >= first + count) continue;
            const int li = static_cast<int>(s.line - first);
            if (!have[li]) { utf8[li] = textOf(s.line).toUtf8(); have[li] = true; }
            const QByteArray& u = utf8[li];
            const int a = QString::fromUtf8(u.constData(), static_cast<int>(qMin<quint32>(s.start, u.size()))).size();
            const int b = QString::fromUtf8(u.constData(), static_cast<int>(qMin<quint32>(s.start + s.length, u.size()))).size();
            lines[li].push_back({a, b - a, s.foreground, (s.flags & 1) != 0, (s.flags & 2) != 0});
        }
        return lines;
    }

    OpenBcTmSyntax* h_ = nullptr;
};

}  // namespace openbc::app

