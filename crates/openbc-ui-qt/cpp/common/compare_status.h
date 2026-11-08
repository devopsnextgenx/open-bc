// compare_status.h
// ---------------------------------------------------------------------------
// Text shared by the status bars of TextCompareViewer and VirtualTextCompareViewer so both read
// the same: "N difference section(s) | M line(s) | K changed row(s)" plus an EDITABLE / READ ONLY badge.
// ---------------------------------------------------------------------------
#pragma once

#include <QColor>
#include <QString>

namespace openbc::app::compare_status {

inline QString summaryText(quint64 sections, quint64 lines, quint64 changedRows) {
    return QStringLiteral("%1 difference section(s)   |   %2 line(s)   |   %3 changed row(s)")
        .arg(sections)
        .arg(lines)
        .arg(changedRows);
}

inline QString badgeText(bool editable) {
    return editable ? QStringLiteral("EDITABLE") : QStringLiteral("READ ONLY");
}

// `filled` is the "on" phase of the flash shown when editability changes.
inline QString badgeStyle(bool editable, bool filled) {
    const QColor color = editable ? QColor(0x3d, 0x8b, 0xe8) : QColor(0xc8, 0x4c, 0x4c);
    return QStringLiteral("QLabel { color: #ffffff; background: %1; border: 1px solid %2; "
                          "border-radius: 3px; padding: 1px 5px; font-weight: bold; }")
        .arg(filled ? color.name() : QStringLiteral("transparent"), color.name());
}

}  // namespace openbc::app::compare_status
