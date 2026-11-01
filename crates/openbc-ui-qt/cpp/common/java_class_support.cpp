#include "common/java_class_support.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>

namespace openbc::app {
namespace {

QString configuredJar() {
    const QString openBcJar = qEnvironmentVariable("OPENBC_CFR_JAR").trimmed();
    return openBcJar.isEmpty() ? qEnvironmentVariable("CFR_JAR").trimmed() : openBcJar;
}

QString configuredVineflowerJar() {
    const QString openBcJar = qEnvironmentVariable("OPENBC_VINEFLOWER_JAR").trimmed();
    return openBcJar.isEmpty() ? qEnvironmentVariable("VINEFLOWER_JAR").trimmed() : openBcJar;
}

QString javaExecutable() {
    const QString javaName =
#ifdef Q_OS_WIN
        QStringLiteral("java.exe");
#else
        QStringLiteral("java");
#endif
    const QString javaHome = qEnvironmentVariable("JAVA_HOME").trimmed();
    if (!javaHome.isEmpty()) {
        const QString candidate = QDir(javaHome).filePath(QStringLiteral("bin/") + javaName);
        if (QFileInfo::exists(candidate)) return candidate;
    }
    return QStandardPaths::findExecutable(javaName);
}

QString compactError(const QByteArray& text) {
    const QString message = QString::fromUtf8(text).trimmed();
    return message.size() > 2000 ? message.left(2000) + QStringLiteral("…") : message;
}

}  // namespace

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
    javaSource->clear();
    error->clear();

    QTemporaryDir temporaryDirectory;
    if (!temporaryDirectory.isValid()) {
        *error = QStringLiteral("Could not create temporary storage for class-file decompilation.");
        return false;
    }
    const QString classFilePath = temporaryDirectory.filePath(
        QFileInfo(sourcePath).completeBaseName() + QStringLiteral(".class"));
    QFile classFile(classFilePath);
    if (!classFile.open(QIODevice::WriteOnly) || classFile.write(bytes) != bytes.size()) {
        *error = QStringLiteral("Could not stage class file for decompilation: ") + classFile.errorString();
        return false;
    }
    classFile.close();

    enum class Decompiler { Cfr, Vineflower };
    Decompiler decompiler = Decompiler::Cfr;
    QString executable;
    QStringList arguments;
    const QString jarPath = configuredJar();
    const QString vineflowerJarPath = configuredVineflowerJar();
    const QString selectedJar = !jarPath.isEmpty() ? jarPath : vineflowerJarPath;
    if (!selectedJar.isEmpty()) {
        if (!QFileInfo(selectedJar).isFile()) {
            *error = QStringLiteral("Decompiler JAR was not found at ") + selectedJar;
            return false;
        }
        executable = javaExecutable();
        if (executable.isEmpty()) {
            *error = QStringLiteral("Java was not found. Set JAVA_HOME or add java to PATH.");
            return false;
        }
        if (!jarPath.isEmpty()) {
            arguments = {QStringLiteral("-jar"), jarPath, classFilePath,
                         QStringLiteral("--silent"), QStringLiteral("true")};
        } else {
            decompiler = Decompiler::Vineflower;
            const QString outputDirectory = temporaryDirectory.filePath(QStringLiteral("source"));
            if (!QDir().mkpath(outputDirectory)) {
                *error = QStringLiteral("Could not create temporary output for Vineflower.");
                return false;
            }
            arguments = {QStringLiteral("-jar"), vineflowerJarPath, classFilePath,
                         outputDirectory};
        }
    } else {
        executable = QStandardPaths::findExecutable(QStringLiteral("cfr"));
        if (!executable.isEmpty()) {
            arguments = {classFilePath, QStringLiteral("--silent"), QStringLiteral("true")};
        } else {
            executable = QStandardPaths::findExecutable(QStringLiteral("vineflower"));
            if (executable.isEmpty()) {
                *error = QStringLiteral(
                    "No Java decompiler was found. Install cfr/vineflower, set CFR_JAR or "
                    "VINEFLOWER_JAR, and make Java available through PATH or JAVA_HOME.");
                return false;
            }
            decompiler = Decompiler::Vineflower;
            const QString outputDirectory = temporaryDirectory.filePath(QStringLiteral("source"));
            if (!QDir().mkpath(outputDirectory)) {
                *error = QStringLiteral("Could not create temporary output for Vineflower.");
                return false;
            }
            arguments = {classFilePath, outputDirectory};
        }
    }

    QProcess process;
    process.setProgram(executable);
    process.setArguments(arguments);
    const QString decompilerName =
        decompiler == Decompiler::Cfr ? QStringLiteral("CFR") : QStringLiteral("Vineflower");
    process.start();
    if (!process.waitForStarted(5000)) {
        *error = QStringLiteral("Could not start ") + decompilerName +
                 QStringLiteral(": ") + process.errorString();
        return false;
    }
    if (!process.waitForFinished(60000)) {
        process.kill();
        process.waitForFinished();
        *error = decompilerName + QStringLiteral(" timed out while decompiling ") +
                 QFileInfo(sourcePath).fileName();
        return false;
    }
    const QByteArray output = process.readAllStandardOutput();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 ||
        (decompiler == Decompiler::Cfr && output.trimmed().isEmpty())) {
        const QString details = compactError(process.readAllStandardError());
        *error = QStringLiteral("%1 failed to decompile %2")
                     .arg(decompilerName, QFileInfo(sourcePath).fileName());
        if (!details.isEmpty()) *error += QStringLiteral(": ") + details;
        return false;
    }

    if (decompiler == Decompiler::Cfr) {
        *javaSource = QString::fromUtf8(output);
        return true;
    }

    QDirIterator outputFiles(temporaryDirectory.filePath(QStringLiteral("source")),
                             {QStringLiteral("*.java")}, QDir::Files,
                             QDirIterator::Subdirectories);
    QString fallbackSourcePath;
    const QString expectedBaseName = QFileInfo(sourcePath).completeBaseName();
    while (outputFiles.hasNext()) {
        const QString path = outputFiles.next();
        if (fallbackSourcePath.isEmpty()) fallbackSourcePath = path;
        if (QFileInfo(path).completeBaseName() == expectedBaseName) {
            fallbackSourcePath = path;
            break;
        }
    }
    if (fallbackSourcePath.isEmpty()) {
        *error = QStringLiteral("Vineflower produced no Java source for ") +
                 QFileInfo(sourcePath).fileName();
        return false;
    }
    QFile sourceFile(fallbackSourcePath);
    if (!sourceFile.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Could not read Vineflower output: ") + sourceFile.errorString();
        return false;
    }
    *javaSource = QString::fromUtf8(sourceFile.readAll());
    if (javaSource->trimmed().isEmpty()) {
        *error = QStringLiteral("Vineflower produced an empty source file for ") +
                 QFileInfo(sourcePath).fileName();
        javaSource->clear();
        return false;
    }
    return true;
}

}  // namespace openbc::app
