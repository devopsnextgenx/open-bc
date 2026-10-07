// picture_metadata.h
// ---------------------------------------------------------------------------
// Metadata side of Picture Compare:
//   * PictureMetadata / collectPictureMetadata() - reads the extras that the
//     Rust backend does not report (EXIF, GPS, embedded text chunks, frame
//     count). Header-only reads, no pixel decoding; safe to run on a worker.
//   * PictureMetadataPanel - a three-column Property | Left | Right table that
//     highlights rows whose values differ.
// Header-only and free of Q_OBJECT, like the rest of the app.
// ---------------------------------------------------------------------------
#pragma once

#include <QCheckBox>
#include <QClipboard>
#include <QFile>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QImageReader>
#include <QLabel>
#include <QList>
#include <QSignalBlocker>
#include <QPushButton>
#include <QString>
#include <QStringList>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>

#include "picture_exif.h"

namespace openbc::app {

struct PictureMetaRow {
    QString group;  // "File", "Image", "EXIF", "GPS", "Text"
    QString key;
    QString value;
};
using PictureMetadata = QList<PictureMetaRow>;

// Reads everything that is cheap to get from the file header. `path` must be a
// local file (remote images are downloaded to a temp file before they reach
// Picture Compare).
inline PictureMetadata collectPictureMetadata(const QString& path) {
    PictureMetadata out;

    QImageReader reader(path);
    reader.setDecideFormatFromContent(true);
    if (reader.canRead()) {
        const int frames = reader.imageCount();
        if (frames > 1) out.push_back({"Image", "Frames", QString::number(frames)});
        const QStringList keys = reader.textKeys();  // PNG tEXt/iTXt etc.
        for (const QString& key : keys) {
            QString value = reader.text(key).simplified();
            if (value.size() > 300) value = value.left(300) + QString::fromUtf8("\xE2\x80\xA6");
            if (!value.isEmpty()) out.push_back({"Text", key, value});
        }
    }

    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
        // The Exif segment of a JPEG sits before the image data, so the head of
        // the file is enough.
        const QByteArray head = file.read(256 * 1024);
        const auto entries = exif::parse(reinterpret_cast<const std::uint8_t*>(head.constData()),
                                         std::size_t(head.size()));
        for (const auto& entry : entries) {
            out.push_back({QString::fromStdString(entry.group), QString::fromStdString(entry.key),
                           QString::fromUtf8(entry.value.data(), qsizetype(entry.value.size()))});
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
class PictureMetadataPanel : public QWidget {
public:
    explicit PictureMetadataPanel(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumWidth(300);
        auto* column = new QVBoxLayout(this);
        column->setContentsMargins(4, 4, 4, 4);
        column->setSpacing(4);

        auto* header = new QHBoxLayout;
        summary_ = new QLabel("Metadata", this);
        summary_->setWordWrap(true);
        onlyDifferences_ = new QCheckBox("Differences only", this);
        auto* copy = new QPushButton("Copy", this);
        copy->setToolTip("Copy the visible rows to the clipboard (tab separated)");
        header->addWidget(summary_, 1);
        header->addWidget(onlyDifferences_);
        header->addWidget(copy);
        column->addLayout(header);

        tree_ = new QTreeWidget(this);
        tree_->setColumnCount(3);
        tree_->setHeaderLabels({"Property", "Left", "Right"});
        tree_->setRootIsDecorated(true);
        tree_->setUniformRowHeights(true);
        tree_->setAlternatingRowColors(true);
        tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
        tree_->setTextElideMode(Qt::ElideMiddle);
        tree_->header()->setStretchLastSection(false);
        tree_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        tree_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
        tree_->header()->setSectionResizeMode(2, QHeaderView::Stretch);
        column->addWidget(tree_, 1);

        connect(onlyDifferences_, &QCheckBox::toggled, this, [this](bool) { rebuild(); });
        connect(copy, &QPushButton::clicked, this, [this]() {
            QGuiApplication::clipboard()->setText(plainText_);
        });
    }

    bool onlyDifferences() const { return onlyDifferences_->isChecked(); }
    void setOnlyDifferences(bool on) {
        const QSignalBlocker blocker(onlyDifferences_);
        onlyDifferences_->setChecked(on);
        rebuild();
    }
    // Lets the owner persist the checkbox.
    QCheckBox* onlyDifferencesCheck() const { return onlyDifferences_; }

    void setMetadata(const PictureMetadata& left, const PictureMetadata& right) {
        left_ = left;
        right_ = right;
        rebuild();
    }

private:
    static QString cellKey(const QString& group, const QString& key) { return group + QChar(0x1f) + key; }

    static int groupRank(const QString& group) {
        static const QStringList order = {"File", "Image", "EXIF", "GPS", "Text"};
        const int index = order.indexOf(group);
        return index < 0 ? order.size() : index;
    }

    void rebuild() {
        tree_->clear();
        plainText_.clear();

        QHash<QString, QString> leftValues, rightValues;
        QStringList groups;
        QHash<QString, QStringList> keysOf;  // group -> keys in first-seen order
        auto absorb = [&](const PictureMetadata& rows, QHash<QString, QString>& values) {
            for (const auto& row : rows) {
                const QString id = cellKey(row.group, row.key);
                if (!values.contains(id)) values.insert(id, row.value);
                if (!groups.contains(row.group)) groups << row.group;
                if (!keysOf[row.group].contains(row.key)) keysOf[row.group] << row.key;
            }
        };
        absorb(left_, leftValues);
        absorb(right_, rightValues);
        std::stable_sort(groups.begin(), groups.end(), [](const QString& a, const QString& b) {
            return groupRank(a) < groupRank(b);
        });

        if (groups.isEmpty()) {
            summary_->setText("No images loaded");
            return;
        }

        const QString dash = QStringLiteral("-");
        const QBrush differBrush(QColor(0xe5, 0x53, 0x4b, 56));
        int compared = 0, differing = 0;
        QStringList lines{"Property\tLeft\tRight"};
        for (const QString& group : groups) {
            const bool informational = group == "File";  // name/size/date always differ
            QList<QTreeWidgetItem*> rows;
            for (const QString& key : keysOf.value(group)) {
                const QString id = cellKey(group, key);
                const bool hasLeft = leftValues.contains(id);
                const bool hasRight = rightValues.contains(id);
                const QString l = hasLeft ? leftValues.value(id) : QString();
                const QString r = hasRight ? rightValues.value(id) : QString();
                const bool differs = hasLeft != hasRight || l != r;
                if (!informational) {
                    ++compared;
                    if (differs) ++differing;
                }
                if (onlyDifferences_->isChecked() && !(differs && !informational)) continue;

                auto* item = new QTreeWidgetItem({key, hasLeft ? l : dash, hasRight ? r : dash});
                item->setToolTip(1, l);
                item->setToolTip(2, r);
                if (differs && !informational) {
                    QFont bold = item->font(0);
                    bold.setBold(true);
                    item->setFont(0, bold);
                    for (int column = 0; column < 3; ++column) item->setBackground(column, differBrush);
                }
                rows << item;
                lines << key + '\t' + (hasLeft ? l : dash) + '\t' + (hasRight ? r : dash);
            }
            if (rows.isEmpty()) continue;
            auto* top = new QTreeWidgetItem(tree_, {group});
            QFont bold = top->font(0);
            bold.setBold(true);
            top->setFont(0, bold);
            top->setFirstColumnSpanned(true);
            top->addChildren(rows);
            top->setExpanded(true);
        }
        plainText_ = lines.join('\n');
        summary_->setText(compared == 0
                              ? QString("Metadata")
                              : QString("%1 of %2 properties differ").arg(differing).arg(compared));
    }

    QLabel* summary_ = nullptr;
    QCheckBox* onlyDifferences_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    PictureMetadata left_, right_;
    QString plainText_;
};

}  // namespace openbc::app
