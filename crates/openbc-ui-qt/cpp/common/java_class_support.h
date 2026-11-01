#pragma once

#include <QByteArray>
#include <QString>

namespace openbc::app {

bool isJavaClassFilePath(const QString& path);
QString javaClassByteDump(const QByteArray& bytes);
bool decompileJavaClass(const QByteArray& bytes, const QString& sourcePath,
                        QString* javaSource, QString* error);

}  // namespace openbc::app
