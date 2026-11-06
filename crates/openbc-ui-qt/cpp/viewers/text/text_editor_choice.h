// text_editor_choice.h
// ---------------------------------------------------------------------------
// Decides which text-compare editor a NEW comparison uses, BEFORE any file content is read.
// Pure policy on top of options/text_editor_selection.h (which is Qt-free and unit-tested); this
// header only adds the file-system facts (QFileInfo) the decision needs.
//
// The Virtual editor works on local regular files (it memory-maps them). A side that is remote,
// archive-backed or clipboard text exists only as preloaded text, so it can only be shown by the
// Existing editor; that is reported in `note` when the user explicitly asked for Virtual.
// ---------------------------------------------------------------------------
#pragma once

#include <QFileInfo>
#include <QString>

#include "options/text_editor_selection.h"

namespace openbc::app {

struct EditorChoice {
    text_editor::Editor editor = text_editor::Editor::Existing;
    QString note;                 // non-empty when the choice differs from what the settings asked for
    qint64 leftBytes = 0;         // text_editor::kUnknownSize when it could not be determined
    qint64 rightBytes = 0;
};

namespace detail {
struct SideFacts {
    bool usableByVirtual = true;   // empty side, new file, or a local regular file
    qint64 bytes = 0;              // kUnknownSize: cannot be sized reliably
};

inline SideFacts sideFacts(const QString& path, const QString& preloadedText) {
    SideFacts f;
    if (path.isEmpty()) {
        // No file: either truly empty, or clipboard/preloaded text that only the Existing editor can show.
        f.usableByVirtual = preloadedText.isEmpty();
        f.bytes = preloadedText.isEmpty() ? 0 : preloadedText.toUtf8().size();
        return f;
    }
    if (path.contains(QStringLiteral("://"))) {   // remote or virtual location
        f.usableByVirtual = false;
        f.bytes = preloadedText.isEmpty() ? text_editor::kUnknownSize : preloadedText.toUtf8().size();
        return f;
    }
    const QFileInfo info(path);
    if (!info.exists()) { f.bytes = 0; return f; }          // new file: behaves as empty
    if (!info.isFile()) { f.bytes = text_editor::kUnknownSize; return f; }   // device/FIFO/...: unknown size
    f.bytes = info.size();
    if (f.bytes < 0) f.bytes = text_editor::kUnknownSize;
    return f;
}
}  // namespace detail

inline EditorChoice chooseTextEditor(const text_editor::Settings& settings, const QString& leftPath,
                                     const QString& rightPath, const QString& leftText, const QString& rightText) {
    const detail::SideFacts l = detail::sideFacts(leftPath, leftText);
    const detail::SideFacts r = detail::sideFacts(rightPath, rightText);
    EditorChoice out;
    out.leftBytes = l.bytes;
    out.rightBytes = r.bytes;
    out.editor = text_editor::chooseEditor(settings, l.bytes, r.bytes);
    if (out.editor == text_editor::Editor::Virtual && !(l.usableByVirtual && r.usableByVirtual)) {
        out.editor = text_editor::Editor::Existing;
        out.note = QStringLiteral(
            "The Virtual editor needs local files; this comparison includes remote or in-memory text, "
            "so the Existing editor is used.");
    }
    return out;
}

}  // namespace openbc::app
