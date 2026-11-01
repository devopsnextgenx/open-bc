#pragma once

#include <QFont>
#include <QFontDatabase>
#include <QString>

namespace openbc::app {

inline QFont defaultCompareFont() {
    const QString preferred = QStringLiteral("DejaVu Sans Mono");
    for (const QString& family : QFontDatabase::families()) {
        if (family.compare(preferred, Qt::CaseInsensitive) == 0) {
            QFont font(family);
            font.setPointSize(10);
            font.setStyleHint(QFont::Monospace);
            return font;
        }
    }
    return QFontDatabase::systemFont(QFontDatabase::FixedFont);
}

}  // namespace openbc::app
