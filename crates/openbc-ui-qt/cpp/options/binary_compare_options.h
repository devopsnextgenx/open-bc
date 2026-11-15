// binary_compare_options.h
// ---------------------------------------------------------------------------
// Persisted settings of the Binary Compare tab, shared by Tools > Options (OptionsStore) and the
// viewer's toolbar so both read and write the same preferences.ini keys:
//
//   binaryCompare/viewMode   "summary" (Status / Change Summary view) | "split" (Split Compare Viewer)
//   binaryCompare/demangle   bool, show C++ names demangled
//   binaryCompare/filters    int bit set: 1 symtab/strtab, 2 debug+notes+identity, 4 padding, 8 local symbols
//
// The toolbar writes a key the moment the user flips it, so the choice is the next tab's default
// as well (persistence across comparison sessions). Qt-light on purpose: QSettings only.
// ---------------------------------------------------------------------------
#pragma once

#include <QSettings>
#include <QString>

namespace openbc::app::binaryopts {

enum class ViewMode { Summary = 0, Split = 1 };

inline constexpr unsigned kHideSymtabStrtab = 1;
inline constexpr unsigned kHideDebugNotes = 2;
inline constexpr unsigned kHidePadding = 4;
inline constexpr unsigned kHideLocalSymbols = 8;
inline constexpr unsigned kHideAll = 15;

inline QString viewModeKey() { return QStringLiteral("binaryCompare/viewMode"); }
inline QString demangleKey() { return QStringLiteral("binaryCompare/demangle"); }
inline QString filtersKey() { return QStringLiteral("binaryCompare/filters"); }

inline ViewMode viewMode(const QSettings* s) {
    return s && s->value(viewModeKey(), QStringLiteral("summary")).toString() == QLatin1String("split")
               ? ViewMode::Split : ViewMode::Summary;
}
inline void setViewMode(QSettings* s, ViewMode mode) {
    if (s) s->setValue(viewModeKey(), mode == ViewMode::Split ? QStringLiteral("split") : QStringLiteral("summary"));
}
inline bool demangle(const QSettings* s) { return !s || s->value(demangleKey(), true).toBool(); }
inline void setDemangle(QSettings* s, bool on) { if (s) s->setValue(demangleKey(), on); }
inline unsigned filters(const QSettings* s) {
    return s ? (s->value(filtersKey(), 0).toUInt() & kHideAll) : 0u;
}
inline void setFilters(QSettings* s, unsigned mask) { if (s) s->setValue(filtersKey(), mask & kHideAll); }

}  // namespace openbc::app::binaryopts
