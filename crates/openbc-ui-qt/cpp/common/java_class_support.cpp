#include "common/java_class_support.h"

#include <QFileInfo>
#include <QStringList>

#include "common/java_decompiler_adapter.h"

namespace openbc::app {

bool isJavaClassFilePath(const QString& path) {
    return !path.isEmpty() &&
           QFileInfo(path).suffix().compare(QStringLiteral("class"), Qt::CaseInsensitive) == 0;
}

QString javaClassByteDump(const QByteArray& bytes) {
    QStringList lines;
    lines.reserve((bytes.size() + 15) / 16);
    for (int offset = 0; offset < bytes.size(); offset += 16) {
        QString hex;
        QString text;
        const int count = qMin(16, bytes.size() - offset);
        for (int i = 0; i < count; ++i) {
            const auto value = static_cast<unsigned char>(bytes.at(offset + i));
            if (i) hex += QLatin1Char(' ');
            hex += QStringLiteral("%1").arg(value, 2, 16, QLatin1Char('0')).toUpper();
            text += value >= 32 && value < 127 ? QChar(value) : QLatin1Char('.');
        }
        lines << QStringLiteral("%1  %2  |%3|")
                     .arg(offset, 8, 16, QLatin1Char('0'))
                     .arg(hex.leftJustified(47, QLatin1Char(' ')))
                     .arg(text);
    }
    return lines.join(QLatin1Char('\n'));
}

bool decompileJavaClass(const QByteArray& bytes, const QString& sourcePath,
                        QString* javaSource, QString* error) {
    if (!javaSource || !error) return false;
    *javaSource = JavaDecompilerAdapter::decompileClassBytes(bytes, sourcePath, error);
    return !javaSource->isNull();
}

}  // namespace openbc::app
