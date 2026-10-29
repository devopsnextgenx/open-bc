// update_notice.cpp - see update_notice.h
#include "home/update_notice.h"

#include <QApplication>
#include <QDesktopServices>
#include <QEvent>
#include <QFont>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QTextBrowser>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>

#include "app/app_info.h"

namespace openbc::app {

namespace {

constexpr int kCardWidth = 560;
constexpr int kCardMargin = 16;
constexpr int kMaxNotesChars = 200000;  // a runaway release body must not freeze the text widget

// Only github.com release pages are opened from the card (the text comes from the network).
bool isReleaseUrl(const QUrl& url) {
    return url.scheme() == QLatin1String("https") && url.host() == QLatin1String("github.com");
}

}  // namespace

// ------------------------------------------------------------------------------------- card

UpdateNoticeCard::UpdateNoticeCard(QWidget* host) : QFrame(host), host_(host) {
    setObjectName("updateNoticeCard");
    setFrameShape(QFrame::NoFrame);
    setAutoFillBackground(true);
    setStyleSheet(
        "QFrame#updateNoticeCard { background: palette(window); border: 2px solid #2f6db5; border-radius: 6px; }"
        " QFrame#updateNoticeCard QLabel { background: transparent; }");
    auto* shadow = new QGraphicsDropShadowEffect(this);
    shadow->setBlurRadius(18);
    shadow->setOffset(0, 3);
    shadow->setColor(QColor(0, 0, 0, 110));
    setGraphicsEffect(shadow);

    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(12, 10, 10, 10);
    column->setSpacing(8);

    auto* top = new QHBoxLayout();
    auto* titles = new QVBoxLayout();
    titles->setSpacing(2);
    headline_ = new QLabel(this);
    QFont bold = headline_->font();
    bold.setBold(true);
    bold.setPointSizeF(bold.pointSizeF() * 1.15);
    headline_->setFont(bold);
    headline_->setWordWrap(true);
    subtitle_ = new QLabel(this);
    subtitle_->setWordWrap(true);
    titles->addWidget(headline_);
    titles->addWidget(subtitle_);
    dismiss_ = new QToolButton(this);
    dismiss_->setText(QString::fromUtf8("\xC3\x97"));  // multiplication sign
    dismiss_->setAutoRaise(true);
    dismiss_->setToolTip("Close (shown again next time OpenBC starts)");
    top->addLayout(titles, 1);
    top->addWidget(dismiss_, 0, Qt::AlignTop);
    column->addLayout(top);

    notes_ = new QTextBrowser(this);
    notes_->setOpenLinks(false);
    notes_->setReadOnly(true);
    notes_->setMinimumHeight(140);
    notes_->setMaximumHeight(240);
    notes_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    column->addWidget(notes_);

    auto* buttons = new QHBoxLayout();
    buttons->setSpacing(8);
    skip_ = new QPushButton("Don't notify for this release", this);
    view_ = new QPushButton("View release", this);
    close_ = new QPushButton("Close", this);
    close_->setDefault(true);
    for (auto* b : {skip_, view_, close_}) b->setFocusPolicy(Qt::NoFocus);  // never steals typing focus
    buttons->addWidget(skip_);
    buttons->addStretch(1);
    buttons->addWidget(view_);
    buttons->addWidget(close_);
    column->addLayout(buttons);

    auto closeCard = [this]() {
        hide();
        if (onClosed) onClosed();
    };
    connect(dismiss_, &QToolButton::clicked, this, closeCard);
    connect(close_, &QPushButton::clicked, this, closeCard);
    connect(view_, &QPushButton::clicked, this, [this]() {
        const QUrl url(url_);
        if (isReleaseUrl(url)) QDesktopServices::openUrl(url);
    });
    connect(skip_, &QPushButton::clicked, this, [this]() {
        const QString tag = skipTag_;
        hide();
        if (onSkipped) onSkipped(tag);
        if (onClosed) onClosed();
    });
    connect(notes_, &QTextBrowser::anchorClicked, this, [](const QUrl& url) {
        if (url.scheme() == QLatin1String("https") || url.scheme() == QLatin1String("http"))
            QDesktopServices::openUrl(url);
    });
    if (host_) host_->installEventFilter(this);  // follow the Home tab's size
    hide();
}

void UpdateNoticeCard::present(const Content& content) {
    headline_->setText(content.headline);
    subtitle_->setText(content.subtitle);
    subtitle_->setVisible(!content.subtitle.isEmpty());
    QString markdown = content.markdown.trimmed();
    if (markdown.isEmpty()) markdown = "_No change log was published for this release._";
    if (markdown.size() > kMaxNotesChars) markdown = markdown.left(kMaxNotesChars) + "\n\n_(change log truncated)_";
    notes_->setMarkdown(markdown);
    notes_->moveCursor(QTextCursor::Start);
    url_ = content.url;
    skipTag_ = content.skipTag;
    view_->setVisible(isReleaseUrl(QUrl(url_)));
    skip_->setVisible(!skipTag_.isEmpty());
    show();
    reposition();
}

void UpdateNoticeCard::reposition() {
    if (!host_ || repositioning_) return;  // our own geometry changes must not re-enter
    repositioning_ = true;
    const int width = std::max(300, std::min(kCardWidth, host_->width() - 2 * kCardMargin));
    setFixedWidth(width);
    QLayout* lay = layout();
    const int height = std::max(lay->hasHeightForWidth() ? lay->heightForWidth(width) : lay->sizeHint().height(),
                                lay->minimumSize().height());
    setFixedHeight(height);
    const int inset = bottomInset_ ? bottomInset_() : 0;
    const int x = std::max(kCardMargin, host_->width() - width - kCardMargin);
    const int y = std::max(kCardMargin, host_->height() - inset - height - kCardMargin);
    if (pos() != QPoint(x, y)) move(x, y);
    raise();
    repositioning_ = false;
}

bool UpdateNoticeCard::eventFilter(QObject* watched, QEvent* event) {
    // Resize/Show only: our own setFixedSize makes the host post LayoutRequest, which would loop.
    if (watched == host_ && isVisibleTo(host_) && (event->type() == QEvent::Resize || event->type() == QEvent::Show))
        reposition();
    return false;
}

// ---------------------------------------------------------------------------------- notifier

UpdateNotifier::UpdateNotifier(QWidget* host, QSettings* preferences, std::function<int()> bottomInset)
    : host_(host), prefs_(preferences) {
    card_ = new UpdateNoticeCard(host);
    card_->setBottomInset(std::move(bottomInset));
    card_->onClosed = [this]() { showNext(); };
    card_->onSkipped = [this](const QString& tag) {
        if (prefs_) prefs_->setValue("updates/skippedTag", tag);
    };
    network_ = new QNetworkAccessManager(host);
}

UpdateNotifier::~UpdateNotifier() {
    if (card_) card_->deleteLater();
}

void UpdateNotifier::start() {
    if (started_ || !prefs_ || !host_ || !prefs_->value("updates/check", true).toBool()) return;
    started_ = true;
    QNetworkRequest request(QUrl("https://api.github.com/repos/devopsnextgenx/open-bc/releases?per_page=30"));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("User-Agent", ("OpenBC/" + appVersion()).toUtf8());
    request.setTransferTimeout(15000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = network_->get(request);
    QObject::connect(reply, &QNetworkReply::finished, host_.data(), [this, reply]() {
        const QByteArray body = reply->error() == QNetworkReply::NoError ? reply->readAll() : QByteArray();
        reply->deleteLater();
        if (!body.isEmpty() && body.size() < 8 * 1024 * 1024) onReleases(body);  // offline / rate-limited: silent
    });
}

void UpdateNotifier::onReleases(const QByteArray& json) {
    const QJsonDocument document = QJsonDocument::fromJson(json);
    if (!document.isArray()) return;
    std::vector<updates::Release> releases;
    for (const QJsonValue& value : document.array()) {
        const QJsonObject object = value.toObject();
        if (object.value("draft").toBool() || object.value("prerelease").toBool()) continue;
        updates::Release release;
        release.tag = object.value("tag_name").toString().toStdString();
        release.title = object.value("name").toString().toStdString();
        release.body = object.value("body").toString().toStdString();
        release.url = object.value("html_url").toString().toStdString();
        release.published = object.value("published_at").toString().left(10).toStdString();
        releases.push_back(std::move(release));
    }
    const QString current = appVersion();
    const updates::Plan plan = updates::plan(std::move(releases), current.toStdString(),
                                             prefs_->value("updates/lastSeen").toString().toStdString(),
                                             prefs_->value("updates/skippedTag").toString().toStdString());

    // What's new first (it describes what the user is already running), then the update offer.
    if (plan.showWhatsNew) {
        const auto& r = plan.whatsNew;
        UpdateNoticeCard::Content c;
        c.headline = "What's new in OpenBC " + current;
        c.subtitle = QString::fromStdString(r.published.empty() ? std::string() : "Released " + r.published);
        c.markdown = QString::fromStdString(r.body);
        c.url = QString::fromStdString(r.url);
        queue_.push_back(c);
        // Shown once: remembered as soon as the notice is queued for display.
        prefs_->setValue("updates/lastSeen", current);
    }
    if (plan.showUpdate) {
        QString markdown;
        for (const updates::Release& r : plan.newer) {
            const QString title = QString::fromStdString(r.title.empty() ? r.tag : r.title);
            markdown += "## " + title + "\n";
            if (!r.published.empty()) markdown += "_" + QString::fromStdString(r.published) + "_\n\n";
            const QString body = QString::fromStdString(r.body).trimmed();
            markdown += (body.isEmpty() ? QString("_No change log published._") : body) + "\n\n---\n\n";
        }
        const updates::Release& latest = plan.newer.front();
        UpdateNoticeCard::Content c;
        c.headline = "Update available: " + QString::fromStdString(latest.tag);
        c.subtitle = "You are running " + current +
                     (plan.newer.size() > 1 ? QString(" (%1 newer releases)").arg(plan.newer.size()) : QString());
        c.markdown = markdown;
        c.url = QString::fromStdString(latest.url);
        c.skipTag = QString::fromStdString(latest.tag);
        queue_.push_back(c);
    }
    showNext();
}

void UpdateNotifier::showNext() {
    if (!card_ || queue_.empty() || card_->isVisibleTo(host_)) return;  // one notice at a time
    const UpdateNoticeCard::Content next = queue_.front();
    queue_.erase(queue_.begin());
    card_->present(next);
}

}  // namespace openbc::app
