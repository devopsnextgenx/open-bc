// ---------------------------------------------------------------------------
// Single source of truth for the application's identity strings: used by
// `--version`, Help > About and the usage tracker. Qt only for QString, so it
// stays trivial; keep it free of behaviour.
// ---------------------------------------------------------------------------
#pragma once

#include <QString>

namespace openbc::app {

inline QString appVersion() { return QStringLiteral("1.0.19"); }
inline QString appAuthor() { return QStringLiteral("Amit Kshirsagar (devopsnextgenx)"); }
inline QString repositoryUrl() { return QStringLiteral("https://github.com/devopsnextgenx/open-bc"); }
inline QString issuesUrl() { return repositoryUrl() + QStringLiteral("/issues"); }

}  // namespace openbc::app
