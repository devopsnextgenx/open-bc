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
    QDateTime openedAt;
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
        QJsonObject entry;
        entry["kind"] = kind;
        entry["left"] = left;
        entry["right"] = right;
        entry["openedAt"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        entries.prepend(entry);
        while (entries.size() > 200) entries.removeLast();
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
            entry.openedAt = QDateTime::fromString(object["openedAt"].toString(), Qt::ISODate);
            result.push_back(entry);
        }
        return result;
    }
};

}  // namespace openbc::app