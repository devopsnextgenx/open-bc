#include "common/java_decompiler_adapter.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>

#include <algorithm>

#include "home/session_history.h"

namespace openbc::app {
namespace {

enum class DecompilerKind { Cfr, Vineflower };

struct ResolvedDecompiler {
    DecompilerKind kind = DecompilerKind::Cfr;
    QString program;
    QString jarPath;
    bool isJar = false;
};

QString environmentPath(const QString& primary, const QString& fallback = {}) {
    const QString configured = qEnvironmentVariable(primary.toLocal8Bit().constData()).trimmed();
    if (!configured.isEmpty()) return configured;
    return fallback.isEmpty()
               ? QString()
               : qEnvironmentVariable(fallback.toLocal8Bit().constData()).trimmed();
}

QString packagedCfrPath() {
    const QDir applicationDirectory(QCoreApplication::applicationDirPath());
#ifdef Q_OS_WIN
    const QString candidate = applicationDirectory.filePath(QStringLiteral("java/cfr.jar"));
    if (QFileInfo(candidate).isFile()) return QFileInfo(candidate).absoluteFilePath();
#else
    const QString localCandidate = applicationDirectory.filePath(QStringLiteral("java/cfr.jar"));
    if (QFileInfo(localCandidate).isFile())
        return QFileInfo(localCandidate).absoluteFilePath();
    const QString candidate =
        applicationDirectory.filePath(QStringLiteral("../share/openbc/java/cfr.jar"));
#endif
    return QFileInfo(candidate).absoluteFilePath();
}

QString packagedJavaPath() {
    const QString flatpakPath = QStringLiteral("/app/lib/openbc-java/bin/java");
    if (QFileInfo(flatpakPath).isFile()) return flatpakPath;

    const QDir applicationDirectory(QCoreApplication::applicationDirPath());
    const QString candidate =
        applicationDirectory.filePath(QStringLiteral("../lib/openbc-java/bin/java"));
    return QFileInfo(candidate).isFile() ? QFileInfo(candidate).absoluteFilePath() : QString();
}

QString executablePath(const QString& configured) {
    const QString requested = configured.trimmed();
    if (requested.isEmpty()) return {};

    const QFileInfo info(requested);
    if (info.isAbsolute() || requested.contains(QLatin1Char('/')) ||
        requested.contains(QLatin1Char('\\'))) {
        return info.isFile() ? info.absoluteFilePath() : QString();
    }

    const QString javaName =
#ifdef Q_OS_WIN
        requested.compare(QStringLiteral("java"), Qt::CaseInsensitive) == 0
            ? QStringLiteral("java.exe")
            : requested;
#else
        requested;
#endif
    if (javaName.compare(QStringLiteral("java"), Qt::CaseInsensitive) == 0 ||
        javaName.compare(QStringLiteral("java.exe"), Qt::CaseInsensitive) == 0) {
        const QString javaHome = qEnvironmentVariable("JAVA_HOME").trimmed();
        if (!javaHome.isEmpty()) {
            const QString candidate = QDir(javaHome).filePath(QStringLiteral("bin/") + javaName);
            if (QFileInfo(candidate).isFile()) return candidate;
        }
    }
    return QStandardPaths::findExecutable(javaName);
}

QString configuredJava(const JavaDecompilerConfiguration& configuration) {
    const QString configured = configuration.javaBinPath.trimmed();
    const QString executable =
        executablePath(configured.isEmpty() ? QStringLiteral("java") : configured);
    return executable.isEmpty() ? packagedJavaPath() : executable;
}

QString compactError(const QByteArray& text) {
    const QString message = QString::fromUtf8(text).trimmed();
    return message.size() > 2000 ? message.left(2000) + QStringLiteral("…") : message;
}

bool resolveDecompiler(const JavaDecompilerConfiguration& configuration,
                       ResolvedDecompiler* resolved, QString* error) {
    if (!resolved || !error) return false;

    QString configured = configuration.decompilerPath.trimmed();
    if (configured.isEmpty()) {
        configured = environmentPath(QStringLiteral("OPENBC_CFR_JAR"),
                                     QStringLiteral("CFR_JAR"));
    }

    if (!configured.isEmpty()) {
        const QFileInfo info(configured);
        if (info.suffix().compare(QStringLiteral("jar"), Qt::CaseInsensitive) == 0) {
            if (!info.isFile()) {
                *error = QStringLiteral("Decompiler JAR was not found at ") +
                         QDir::toNativeSeparators(configured);
                return false;
            }
            resolved->kind = DecompilerKind::Cfr;
            resolved->jarPath = info.absoluteFilePath();
            resolved->isJar = true;
            return true;
        }
        const QString executable = executablePath(configured);
        if (executable.isEmpty()) {
            *error = QStringLiteral("Decompiler executable was not found at ") +
                     QDir::toNativeSeparators(configured);
            return false;
        }
        resolved->kind = DecompilerKind::Cfr;
        resolved->program = executable;
        return true;
    }

    const QString bundled = packagedCfrPath();
    if (QFileInfo(bundled).isFile()) {
        resolved->kind = DecompilerKind::Cfr;
        resolved->jarPath = bundled;
        resolved->isJar = true;
        return true;
    }

    const QString cfrCommand = QStandardPaths::findExecutable(
#ifdef Q_OS_WIN
        QStringLiteral("cfr.exe")
#else
        QStringLiteral("cfr")
#endif
    );
    if (!cfrCommand.isEmpty()) {
        resolved->kind = DecompilerKind::Cfr;
        resolved->program = cfrCommand;
        return true;
    }

    const QString vineflowerJar =
        environmentPath(QStringLiteral("OPENBC_VINEFLOWER_JAR"),
                        QStringLiteral("VINEFLOWER_JAR"));
    if (!vineflowerJar.isEmpty()) {
        if (!QFileInfo(vineflowerJar).isFile()) {
            *error = QStringLiteral("Vineflower JAR was not found at ") +
                     QDir::toNativeSeparators(vineflowerJar);
            return false;
        }
        resolved->kind = DecompilerKind::Vineflower;
        resolved->jarPath = QFileInfo(vineflowerJar).absoluteFilePath();
        resolved->isJar = true;
        return true;
    }
    const QString vineflowerCommand = QStandardPaths::findExecutable(
#ifdef Q_OS_WIN
        QStringLiteral("vineflower.exe")
#else
        QStringLiteral("vineflower")
#endif
    );
    if (!vineflowerCommand.isEmpty()) {
        resolved->kind = DecompilerKind::Vineflower;
        resolved->program = vineflowerCommand;
        return true;
    }

    *error = QStringLiteral(
        "No Java decompiler was found. Install CFR or configure a CFR JAR in Tools > Options > "
        "Archive Types. Java must be available on PATH or selected there.");
    return false;
}

bool runProcess(const QString& program, const QStringList& arguments, int timeoutMs,
                QByteArray* standardOutput, QString* error, const QString& operation) {
    QProcess process;
    process.setProgram(program);
    process.setArguments(arguments);
    process.start();
    if (!process.waitForStarted(5000)) {
        *error = QStringLiteral("Could not start ") + operation + QStringLiteral(": ") +
                 process.errorString();
        return false;
    }
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished();
        *error = operation + QStringLiteral(" timed out.");
        return false;
    }

    const QByteArray output = process.readAllStandardOutput();
    const QByteArray standardError = process.readAllStandardError();
    if (standardOutput) *standardOutput = output;
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        *error = operation + QStringLiteral(" failed.");
        const QString details = compactError(standardError);
        if (!details.isEmpty()) *error += QStringLiteral(": ") + details;
        return false;
    }
    return true;
}

QStringList sourceFiles(const QString& outputDirectory) {
    QStringList paths;
    QDirIterator iterator(outputDirectory, {QStringLiteral("*.java")}, QDir::Files,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) paths.append(iterator.next());
    std::sort(paths.begin(), paths.end());
    return paths;
}

bool readGeneratedSources(const QString& outputDirectory, QString* javaSource, QString* error) {
    const QStringList paths = sourceFiles(outputDirectory);
    if (paths.isEmpty()) {
        *error = QStringLiteral("The decompiler produced no Java source.");
        return false;
    }

    QStringList sources;
    sources.reserve(paths.size());
    for (const QString& path : paths) {
        QFile source(path);
        if (!source.open(QIODevice::ReadOnly)) {
            *error = QStringLiteral("Could not read decompiler output: ") + source.errorString();
            return false;
        }
        const QString relativePath = QDir(outputDirectory).relativeFilePath(path);
        sources.append(QStringLiteral("// %1\n%2")
                           .arg(QDir::fromNativeSeparators(relativePath),
                                QString::fromUtf8(source.readAll())));
    }
    *javaSource = sources.join(QStringLiteral("\n\n"));
    if (javaSource->trimmed().isEmpty()) {
        *error = QStringLiteral("The decompiler produced empty Java source.");
        return false;
    }
    return true;
}

bool runDecompiler(const QByteArray& bytes, const QString& sourcePath,
                   const JavaDecompilerConfiguration& configuration,
                   QString* javaSource, QString* error) {
    if (!javaSource || !error) return false;
    javaSource->clear();
    error->clear();
    if (!configuration.enabled) {
        *error = QStringLiteral("Java decompilation is disabled in Tools > Options > Archive Types.");
        return false;
    }

    ResolvedDecompiler decompiler;
    if (!resolveDecompiler(configuration, &decompiler, error)) return false;

    QString java;
    if (decompiler.isJar) {
        java = configuredJava(configuration);
        if (java.isEmpty()) {
            *error = QStringLiteral("Java was not found. Set the Java Runtime path in Tools > "
                                    "Options > Archive Types or add java to PATH.");
            return false;
        }
    }

    QTemporaryDir temporaryDirectory;
    if (!temporaryDirectory.isValid()) {
        *error = QStringLiteral("Could not create temporary storage for Java decompilation.");
        return false;
    }
    const QString extension =
        QFileInfo(sourcePath).suffix().compare(QStringLiteral("jar"), Qt::CaseInsensitive) == 0
            ? QStringLiteral(".jar")
            : QStringLiteral(".class");
    const QString stagedPath =
        temporaryDirectory.filePath(QFileInfo(sourcePath).completeBaseName() + extension);
    QFile stagedFile(stagedPath);
    if (!stagedFile.open(QIODevice::WriteOnly) || stagedFile.write(bytes) != bytes.size()) {
        *error = QStringLiteral("Could not stage Java input for decompilation: ") +
                 stagedFile.errorString();
        return false;
    }
    stagedFile.close();

    const bool isJarInput = extension == QLatin1String(".jar");
    const QString outputDirectory = temporaryDirectory.filePath(QStringLiteral("source"));
    QStringList arguments;
    if (decompiler.kind == DecompilerKind::Cfr) {
        if (isJarInput && !QDir().mkpath(outputDirectory)) {
            *error = QStringLiteral("Could not create temporary output for JAR decompilation.");
            return false;
        }
        if (decompiler.isJar) {
            arguments << QStringLiteral("-jar") << decompiler.jarPath << stagedPath;
        } else {
            arguments << stagedPath;
        }
        arguments << QStringLiteral("--silent") << QStringLiteral("true");
        if (isJarInput) arguments << QStringLiteral("--outputdir") << outputDirectory;
    } else {
        if (!QDir().mkpath(outputDirectory)) {
            *error = QStringLiteral("Could not create temporary output for Vineflower.");
            return false;
        }
        if (decompiler.isJar)
            arguments << QStringLiteral("-jar") << decompiler.jarPath;
        arguments << stagedPath << outputDirectory;
    }

    QByteArray standardOutput;
    const QString decompilerName =
        decompiler.kind == DecompilerKind::Cfr ? QStringLiteral("CFR")
                                                : QStringLiteral("Vineflower");
    const QString program = decompiler.isJar ? java : decompiler.program;
    if (!runProcess(program, arguments, 120000, &standardOutput, error, decompilerName))
        return false;

    if (isJarInput || decompiler.kind == DecompilerKind::Vineflower) {
        if (!readGeneratedSources(outputDirectory, javaSource, error)) return false;
    } else {
        *javaSource = QString::fromUtf8(standardOutput);
        if (javaSource->trimmed().isEmpty()) {
            *error = QStringLiteral("%1 produced no Java source for %2.")
                         .arg(decompilerName, QFileInfo(sourcePath).fileName());
            javaSource->clear();
            return false;
        }
    }
    return true;
}

}  // namespace

JavaDecompilerConfiguration JavaDecompilerAdapter::configuration() {
    QSettings settings(SessionHistory::rootPath() + QStringLiteral("/preferences.ini"),
                       QSettings::IniFormat);
    JavaDecompilerConfiguration result;
    result.enabled = settings.value(QStringLiteral("ArchiveTypes/JavaDecompilerEnabled"), true)
                         .toBool();
    result.decompilerPath =
        settings.value(QStringLiteral("ArchiveTypes/DecompilerJarPath")).toString().trimmed();
    result.javaBinPath =
        settings.value(QStringLiteral("ArchiveTypes/JavaBinPath"), QStringLiteral("java"))
            .toString()
            .trimmed();
    return result;
}

QString JavaDecompilerAdapter::decompileClassFile(const QString& classFilePath, QString* error) {
    QFile file(classFilePath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("Could not read Java class file: ") + file.errorString();
        return {};
    }
    return decompileClassBytes(file.readAll(), classFilePath, error);
}

QString JavaDecompilerAdapter::decompileClassBytes(const QByteArray& bytes,
                                                   const QString& sourcePath, QString* error) {
    QString javaSource;
    if (!runDecompiler(bytes, sourcePath, configuration(), &javaSource, error)) return {};
    return javaSource;
}

bool JavaDecompilerAdapter::testConfiguration(
    const JavaDecompilerConfiguration& configuration, QString* error) {
    if (!error) return false;
    error->clear();

    ResolvedDecompiler decompiler;
    if (!resolveDecompiler(configuration, &decompiler, error)) return false;

    QString program = decompiler.program;
    QStringList arguments{QStringLiteral("--help")};
    if (decompiler.isJar) {
        program = configuredJava(configuration);
        if (program.isEmpty()) {
            *error = QStringLiteral("Java was not found. Set the Java Runtime path in Tools > "
                                    "Options > Archive Types or add java to PATH.");
            return false;
        }
        arguments = {QStringLiteral("-jar"), decompiler.jarPath, QStringLiteral("--help")};
    }

    QByteArray output;
    if (!runProcess(program, arguments, 15000, &output, error, QStringLiteral("Decompiler test")))
        return false;
    if (output.trimmed().isEmpty()) {
        *error = QStringLiteral("The decompiler started but returned no test output.");
        return false;
    }
    return true;
}

}  // namespace openbc::app
