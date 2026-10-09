// model3d_extensions.h - file-dialog filter for the 3D Compare tab. The list comes from the Rust
// loader registry (so new loaders show up automatically); a static list is the fallback.
#pragma once
#include <QString>
#include <QStringList>
#include "viewers/model3d/model3d_compare_api.h"

namespace openbc::app {

inline QStringList model3dExtensions(Openbc3dCompare* handle = nullptr) {
    QStringList list;
    if (handle) {
        QByteArray buf(512, '\0');
        const std::size_t n = openbc_model3d_extensions(handle, reinterpret_cast<std::uint8_t*>(buf.data()), std::size_t(buf.size()));
        if (n > 0 && n <= std::size_t(buf.size())) list = QString::fromUtf8(buf.constData(), qsizetype(n)).split(';', Qt::SkipEmptyParts);
    }
    if (list.isEmpty()) list = {"stl", "obj", "ply", "off", "step", "stp", "iges", "igs", "glb", "gltf", "3mf"};
    return list;
}

inline QString model3dFileDialogFilter(Openbc3dCompare* handle = nullptr) {
    QStringList globs;
    for (const QString& e : model3dExtensions(handle)) globs << "*." + e;
    return "3D models (" + globs.join(' ') + ");;All files (*)";
}

}  // namespace openbc::app
