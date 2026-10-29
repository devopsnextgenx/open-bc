// update_notice.h
// ---------------------------------------------------------------------------
// Release notices on the Home tab, bottom-right corner (floating card, not part of the layout):
//
//   "Update available"  a newer release exists on GitHub. Shows the full change log of every
//                       release between the running version and the latest in a scrolling box.
//                       [View release] [Don't notify for this release]  (x) closes until next launch.
//   "What's new"        shown once, the first time a newly installed version starts: that version's
//                       change log. Never shown again until a different version is installed.
//
// Source: https://github.com/devopsnextgenx/open-bc/releases (public API, one request per start,
// no token, failures are silent). Decisions live in the Qt-free home/release_logic.h.
//
// Settings (QSettings passed in, same file as the other preferences):
//   updates/check          bool,   default true   - master switch (Tools > Options can expose it)
//   updates/lastSeen       string                 - version whose change log was already shown
//   updates/skippedTag     string                 - release the user asked not to hear about
//
// Usage: HomeView::enableUpdateNotices(&preferences) once after construction.
// ---------------------------------------------------------------------------
#pragma once

#include <QFrame>
#include <QPointer>
#include <QSettings>
#include <QString>
#include <functional>
#include <memory>

#include "home/release_logic.h"

class QLabel;
class QNetworkAccessManager;
class QPushButton;
class QTextBrowser;
class QToolButton;

namespace openbc::app {

class UpdateNoticeCard : public QFrame {
public:
    explicit UpdateNoticeCard(QWidget* host);

    struct Content {
        QString headline;
        QString subtitle;
        QString markdown;     // change log
        QString url;          // "View release" target (empty hides the button)
        QString skipTag;      // non-empty shows "Don't notify for this release" for this tag
    };

    std::function<void()> onClosed;                       // (x) / Close
    std::function<void(const QString& tag)> onSkipped;    // "Don't notify for this release"

    void present(const Content& content);
    // Pixels at the bottom of the host that must stay free (the Home footer).
    void setBottomInset(std::function<int()> inset) { bottomInset_ = std::move(inset); }
    void reposition();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QWidget* host_ = nullptr;
    QLabel* headline_ = nullptr;
    QLabel* subtitle_ = nullptr;
    QTextBrowser* notes_ = nullptr;
    QPushButton* view_ = nullptr;
    QPushButton* skip_ = nullptr;
    QPushButton* close_ = nullptr;
    QToolButton* dismiss_ = nullptr;
    QString url_;
    QString skipTag_;
    bool repositioning_ = false;
    std::function<int()> bottomInset_;
};

// Fetches the releases once and drives the card (one notice at a time; a second one waits).
class UpdateNotifier {
public:
    UpdateNotifier(QWidget* host, QSettings* preferences, std::function<int()> bottomInset);
    ~UpdateNotifier();
    UpdateNotifier(const UpdateNotifier&) = delete;
    UpdateNotifier& operator=(const UpdateNotifier&) = delete;

    void start();  // no-op when updates/check is false

private:
    void onReleases(const QByteArray& json);
    void showNext();

    QPointer<QWidget> host_;
    QSettings* prefs_;
    QPointer<UpdateNoticeCard> card_;
    QNetworkAccessManager* network_ = nullptr;
    std::vector<UpdateNoticeCard::Content> queue_;
    bool started_ = false;
};

}  // namespace openbc::app
