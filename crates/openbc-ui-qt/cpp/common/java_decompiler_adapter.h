#pragma once

#include <QByteArray>
#include <QString>

namespace openbc::app {

struct JavaDecompilerConfiguration {
    bool enabled = true;
    QString decompilerPath;
    QString javaBinPath = QStringLiteral("java");
};

class JavaDecompilerAdapter {
public:
    static JavaDecompilerConfiguration configuration();
    static QString decompileClassFile(const QString& classFilePath, QString* error);
    static QString decompileClassBytes(const QByteArray& bytes, const QString& sourcePath,
                                       QString* error);
    static bool testConfiguration(const JavaDecompilerConfiguration& configuration,
                                  QString* error);
};

}  // namespace openbc::app
