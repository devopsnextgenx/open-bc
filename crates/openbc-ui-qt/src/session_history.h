#pragma once

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QStringList>

namespace openbc::app {

struct SessionHistoryEntry {
    QString kind;
    QString left;
    QString right;
    QString label;
    QDateTime openedAt;
    QString savedNode;
    int savedIndex = -1;
};

struct SavedSessionNode {
    QString name;
    QList<SessionHistoryEntry> sessions;
};

class SessionHistory {
public:
    static QString rootPath() {
        return QDir::homePath() + "/.config/open-bc";
    }

    static void record(const QString& kind, const QString& left, const QString& right) {
        const QString directory = rootPath() + "/sessions";
        QDir().mkpath(directory);
        const QString path = directory + "/history.json";
        QJsonArray entries;
        QFile file(path);
        if (file.open(QIODevice::ReadOnly)) {
            entries = QJsonDocument::fromJson(file.readAll()).array();
            file.close();
        }
        const QDate today = QDate::currentDate();
        for (int index = entries.size() - 1; index >= 0; --index) {
            const auto existing = entries[index].toObject();
            if (existing["kind"].toString() != kind || existing["left"].toString() != left ||
                existing["right"].toString() != right) {
                continue;
            }
            const QDate existingDate = QDateTime::fromString(existing["openedAt"].toString(), Qt::ISODate)
                                           .date();
            if (existingDate == today) {
                entries.removeAt(index);
            }
        }
        QJsonObject entry;
        entry["kind"] = kind;
        entry["left"] = left;
        entry["right"] = right;
        entry["label"] = defaultLabel(kind, left, right);
        entry["openedAt"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        entries.prepend(entry);
        while (entries.size() > 200) entries.removeLast();
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write(QJsonDocument(entries).toJson(QJsonDocument::Indented));
        }
    }

    static QString defaultLabel(const QString& kind, const QString& left, const QString& right) {
        const QString leftName = left.isEmpty() ? QString("(missing)") : QFileInfo(left).fileName();
        const QString rightName = right.isEmpty() ? QString("(missing)") : QFileInfo(right).fileName();
        return kind == "folder" ? leftName + " <-> " + rightName
                                 : leftName + " <-> " + rightName;
    }

    static QString savedRootPath() { return rootPath() + "/saved"; }

    static QList<SavedSessionNode> loadSavedNodes() {
        QList<SavedSessionNode> result;
        QDir root(savedRootPath());
        for (const auto& name : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            SavedSessionNode node;
            node.name = name;
            const auto sessions = loadSaved(name);
            node.sessions = sessions;
            result.push_back(node);
        }
        return result;
    }

    static void save(const SessionHistoryEntry& source, const QString& nodeName,
                     const QString& label) {
        const QString node = cleanNodeName(nodeName);
        if (node.isEmpty()) return;
        const QString directory = savedRootPath() + "/" + node;
        QDir().mkpath(directory);
        const QString path = directory + "/sessions.json";
        QJsonArray entries = readArray(path);
        QJsonObject entry = toJson(source);
        entry["label"] = label.trimmed().isEmpty() ? defaultLabel(source.kind, source.left, source.right)
                                                     : label.trimmed();
        entry["openedAt"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        entries.append(entry);
        writeArray(path, entries);
    }

    static void renameSaved(const QString& nodeName, int index, const QString& label) {
        const QString path = savedPath(nodeName);
        QJsonArray entries = readArray(path);
        if (index < 0 || index >= entries.size() || label.trimmed().isEmpty()) return;
        QJsonObject entry = entries[index].toObject();
        entry.insert("label", label.trimmed());
        entries[index] = entry;
        writeArray(path, entries);
    }

    static void updateSaved(const QString& nodeName, int index, const SessionHistoryEntry& source,
                            const QString& label) {
        const QString path = savedPath(nodeName);
        QJsonArray entries = readArray(path);
        if (index < 0 || index >= entries.size()) return;
        QJsonObject entry = toJson(source);
        entry["label"] = label.trimmed().isEmpty() ? defaultLabel(source.kind, source.left, source.right)
                                                     : label.trimmed();
        entry["openedAt"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        entries[index] = entry;
        writeArray(path, entries);
    }

    static void removeSaved(const QString& nodeName, int index) {
        const QString path = savedPath(nodeName);
        QJsonArray entries = readArray(path);
        if (index < 0 || index >= entries.size()) return;
        entries.removeAt(index);
        writeArray(path, entries);
    }

    static void removeNode(const QString& nodeName) {
        QDir(savedRootPath() + "/" + cleanNodeName(nodeName)).removeRecursively();
    }

    static void removeAt(int index) {
        const QString path = rootPath() + "/sessions/history.json";
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return;
        QJsonArray entries = QJsonDocument::fromJson(file.readAll()).array();
        file.close();
        if (index < 0 || index >= entries.size()) return;
        entries.removeAt(index);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write(QJsonDocument(entries).toJson(QJsonDocument::Indented));
        }
    }

    static void renameRecent(int index, const QString& label) {
        const QString path = rootPath() + "/sessions/history.json";
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return;
        QJsonArray entries = QJsonDocument::fromJson(file.readAll()).array();
        file.close();
        if (index < 0 || index >= entries.size() || label.trimmed().isEmpty()) return;
        QJsonObject entry = entries[index].toObject();
        entry.insert("label", label.trimmed());
        entries[index] = entry;
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write(QJsonDocument(entries).toJson(QJsonDocument::Indented));
        }
    }

    static QList<SessionHistoryEntry> load() {
        QList<SessionHistoryEntry> result;
        QFile file(rootPath() + "/sessions/history.json");
        if (!file.open(QIODevice::ReadOnly)) return result;
        for (const auto& value : QJsonDocument::fromJson(file.readAll()).array()) {
            const auto object = value.toObject();
            SessionHistoryEntry entry;
            entry.kind = object["kind"].toString();
            entry.left = object["left"].toString();
            entry.right = object["right"].toString();
            entry.label = object["label"].toString();
            entry.openedAt = QDateTime::fromString(object["openedAt"].toString(), Qt::ISODate);
            result.push_back(entry);
        }
        return result;
    }

    static QList<SessionHistoryEntry> loadSaved(const QString& nodeName) {
        QList<SessionHistoryEntry> result;
        const QJsonArray entries = readArray(savedPath(nodeName));
        for (int index = 0; index < entries.size(); ++index) {
            SessionHistoryEntry entry = fromJson(entries[index].toObject());
            entry.savedNode = cleanNodeName(nodeName);
            entry.savedIndex = index;
            result.push_back(entry);
        }
        return result;
    }

private:
    static QString savedPath(const QString& nodeName) {
        return savedRootPath() + "/" + cleanNodeName(nodeName) + "/sessions.json";
    }

    static QString cleanNodeName(const QString& value) {
        QString result = value.trimmed();
        result.replace('/', '_');
        result.replace('\\', '_');
        return result;
    }

    static QJsonArray readArray(const QString& path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return {};
        return QJsonDocument::fromJson(file.readAll()).array();
    }

    static void writeArray(const QString& path, const QJsonArray& entries) {
        QFile file(path);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write(QJsonDocument(entries).toJson(QJsonDocument::Indented));
        }
    }

    static QJsonObject toJson(const SessionHistoryEntry& entry) {
        return QJsonObject{{"kind", entry.kind}, {"left", entry.left}, {"right", entry.right}};
    }

    static SessionHistoryEntry fromJson(const QJsonObject& object) {
        SessionHistoryEntry entry;
        entry.kind = object["kind"].toString();
        entry.left = object["left"].toString();
        entry.right = object["right"].toString();
        entry.label = object["label"].toString();
        entry.openedAt = QDateTime::fromString(object["openedAt"].toString(), Qt::ISODate);
        return entry;
    }
};

}  // namespace openbc::app