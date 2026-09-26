#pragma once

#include <QColor>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRandomGenerator>
#include <QRegularExpression>
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
    QString path;
    QList<SessionHistoryEntry> sessions;
    QList<SavedSessionNode> children;
};

// Buckets used to group "Recent sessions" by how long ago they were opened.
// Ordinal values double as display order (Today first, Older last).
enum class RecentBucket { Today = 0, Yesterday, ThisWeek, LastWeek, Older };
constexpr int kRecentBucketCount = 5;

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
        // Only treat this as a duplicate (and skip adding a new entry) when
        // the exact same session was already opened earlier today. Reopening
        // it from Yesterday/This week/Last week/Older, or from a saved node,
        // should still create a fresh entry so each day's sessions -- and
        // new sessions opened today -- stay visible under "Today".
        for (const auto& value : entries) {
            const auto existing = value.toObject();
            if (existing["kind"].toString() == kind && existing["left"].toString() == left &&
                existing["right"].toString() == right) {
                const QDateTime openedAt = QDateTime::fromString(existing["openedAt"].toString(), Qt::ISODate);
                if (bucketFor(openedAt) == RecentBucket::Today) return;
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

    // Buckets a "Recent sessions" entry by how long ago it was opened. Days
    // are counted against the calendar date (not a rolling 24h window), so
    // an entry opened at 12:01am today is still "Today".
    static RecentBucket bucketFor(const QDateTime& openedAt) {
        const QDate date = openedAt.date();
        if (!date.isValid()) return RecentBucket::Older;
        const qint64 daysAgo = date.daysTo(QDate::currentDate());
        if (daysAgo <= 0) return RecentBucket::Today;
        if (daysAgo == 1) return RecentBucket::Yesterday;
        if (daysAgo <= 7) return RecentBucket::ThisWeek;
        if (daysAgo <= 14) return RecentBucket::LastWeek;
        return RecentBucket::Older;
    }

    static QString bucketLabel(RecentBucket bucket) {
        switch (bucket) {
        case RecentBucket::Today: return "Today";
        case RecentBucket::Yesterday: return "Yesterday";
        case RecentBucket::ThisWeek: return "This week";
        case RecentBucket::LastWeek: return "Last week";
        case RecentBucket::Older:
        default: return "Older";
        }
    }

    static QString savedRootPath() { return rootPath() + "/saved"; }

    static QList<SavedSessionNode> loadSavedNodes() {
        return loadSavedNodes(QString());
    }

    static bool createNode(const QString& parentNode, const QString& name) {
        const QString node = appendNode(parentNode, name);
        return !node.isEmpty() && QDir().mkpath(savedRootPath() + "/" + node);
    }

    static bool renameNode(const QString& nodeName, const QString& newName) {
        const QString source = cleanNodeName(nodeName);
        const QString replacement = appendNode(QFileInfo(source).path() == "."
                                                   ? QString()
                                                   : QFileInfo(source).path(),
                                               newName);
        if (source.isEmpty() || replacement.isEmpty() || source == replacement ||
            QDir(savedRootPath()).exists(replacement)) {
            return false;
        }
        QDir root(savedRootPath());
        if (!root.rename(source, replacement)) return false;
        renameNodeColors(source, replacement);
        return true;
    }

    static bool moveSession(bool sourceSaved, const QString& sourceNode, int sourceIndex,
                            const QString& targetNode) {
        const QString target = cleanNodeName(targetNode);
        if (target.isEmpty()) return false;
        const auto entries = sourceSaved ? loadSaved(sourceNode) : load();
        if (sourceIndex < 0 || sourceIndex >= entries.size()) return false;
        if (sourceSaved && cleanNodeName(sourceNode) == target) return false;
        save(entries[sourceIndex], target, entries[sourceIndex].label);
        if (sourceSaved) removeSaved(sourceNode, sourceIndex); else removeAt(sourceIndex);
        return true;
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
        const QString node = cleanNodeName(nodeName);
        QDir(savedRootPath() + "/" + node).removeRecursively();
        removeNodeColors(node);
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

    // Colour for a named node in the saved-sessions tree. Nodes added under
    // "Personal" (at any depth) get a colour assigned the first time they're
    // seen, picked at random from a small palette and then persisted so it
    // stays stable on later refreshes. Every other node (including
    // "Personal" itself, which the UI colours separately) returns an
    // invalid QColor so the caller can fall back to a shared default.
    static QColor colorForNode(const QString& nodeName) {
        const QString node = cleanNodeName(nodeName);
        const QStringList parts = node.split('/', Qt::SkipEmptyParts);
        if (parts.size() < 2 || parts.first() != "Personal") return QColor();
        QMap<QString, QString> colors = readNodeColors();
        const auto found = colors.constFind(node);
        if (found != colors.constEnd() && QColor::isValidColor(found.value())) {
            return QColor(found.value());
        }
        return assignNodeColor(node, colors);
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
    static QList<SavedSessionNode> loadSavedNodes(const QString& parentNode) {
        QList<SavedSessionNode> result;
        const QString directoryPath = parentNode.isEmpty() ? savedRootPath()
                                                            : savedRootPath() + "/" + parentNode;
        QDir root(directoryPath);
        for (const auto& name : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            SavedSessionNode node;
            node.name = name;
            node.path = parentNode.isEmpty() ? name : parentNode + "/" + name;
            node.sessions = loadSaved(node.path);
            node.children = loadSavedNodes(node.path);
            result.push_back(node);
        }
        return result;
    }

    static QString savedPath(const QString& nodeName) {
        return savedRootPath() + "/" + cleanNodeName(nodeName) + "/sessions.json";
    }

    static QString cleanNodeName(const QString& value) {
        QStringList parts = value.trimmed().split(QRegularExpression("[/\\\\]"), Qt::SkipEmptyParts);
        QStringList clean;
        for (QString part : parts) {
            part = part.trimmed();
            if (part.isEmpty() || part == "." || part == "..") continue;
            clean.append(part.replace('/', '_').replace('\\', '_'));
        }
        return clean.join('/');
    }

    static QString appendNode(const QString& parent, const QString& name) {
        const QString child = name.trimmed();
        if (child.isEmpty() || child.contains('/') || child.contains('\\') || child == "." ||
            child == "..") {
            return {};
        }
        const QString parentPath = cleanNodeName(parent);
        return parentPath.isEmpty() ? child : parentPath + "/" + child;
    }

    static QString nodeColorsPath() { return savedRootPath() + "/node_colors.json"; }

    static QMap<QString, QString> readNodeColors() {
        QMap<QString, QString> result;
        QFile file(nodeColorsPath());
        if (!file.open(QIODevice::ReadOnly)) return result;
        const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            result.insert(it.key(), it.value().toString());
        }
        return result;
    }

    static void writeNodeColors(const QMap<QString, QString>& colors) {
        QJsonObject object;
        for (auto it = colors.constBegin(); it != colors.constEnd(); ++it) {
            object.insert(it.key(), it.value());
        }
        QDir().mkpath(savedRootPath());
        QFile file(nodeColorsPath());
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
        }
    }

    // Picks a random colour for `node`, preferring one not already used by
    // any other node so siblings stay visually distinct, then persists it.
    static QColor assignNodeColor(const QString& node, QMap<QString, QString> colors) {
        static const QStringList kPalette = {
            "#6ca0f5", "#4cd6c0", "#f28a6a", "#e07ad6", "#a8dc4a",
            "#5ad6e8", "#f2d04a", "#9a8af2", "#ff8fc8", "#7de89a",
        };
        QStringList used;
        for (auto it = colors.constBegin(); it != colors.constEnd(); ++it) used << it.value().toLower();
        QStringList available;
        for (const auto& hex : kPalette) {
            if (!used.contains(hex.toLower())) available << hex;
        }
        if (available.isEmpty()) available = kPalette;
        const QString chosen = available.at(QRandomGenerator::global()->bounded(available.size()));
        colors.insert(node, chosen);
        writeNodeColors(colors);
        return QColor(chosen);
    }

    // Keeps node_colors.json in step with a node rename, remapping the
    // renamed node's own key plus every descendant's key.
    static void renameNodeColors(const QString& source, const QString& replacement) {
        QMap<QString, QString> colors = readNodeColors();
        QMap<QString, QString> updated;
        bool changed = false;
        for (auto it = colors.constBegin(); it != colors.constEnd(); ++it) {
            QString key = it.key();
            if (key == source) {
                key = replacement;
                changed = true;
            } else if (key.startsWith(source + "/")) {
                key = replacement + key.mid(source.length());
                changed = true;
            }
            updated.insert(key, it.value());
        }
        if (changed) writeNodeColors(updated);
    }

    // Drops node_colors.json entries for a removed node and its descendants.
    static void removeNodeColors(const QString& node) {
        QMap<QString, QString> colors = readNodeColors();
        bool changed = false;
        for (auto it = colors.begin(); it != colors.end();) {
            if (it.key() == node || it.key().startsWith(node + "/")) {
                it = colors.erase(it);
                changed = true;
            } else {
                ++it;
            }
        }
        if (changed) writeNodeColors(colors);
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