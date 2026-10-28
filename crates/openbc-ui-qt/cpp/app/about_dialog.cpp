// about_dialog.cpp - see about_dialog.h
#include "app/about_dialog.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QSysInfo>
#include <QVBoxLayout>
#include <QtGlobal>

#include "app/app_info.h"

namespace openbc::app {

void showAboutDialog(QWidget* parent) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("About OpenBC"));
    dialog.setMinimumWidth(420);

    const QString link = QStringLiteral("<a style=\"color:#58a6ff\" href=\"%1\">%1</a>");
    const QString html =
        QStringLiteral(
            "<h2 style=\"margin:0\">OpenBC</h2>"
            "<p style=\"margin-top:2px\">Version %1</p>"
            "<table cellspacing=\"4\">"
            "<tr><td><b>Author</b></td><td>%2</td></tr>"
            "<tr><td><b>Source code</b></td><td>%3</td></tr>"
            "<tr><td><b>Report an issue</b></td><td>%4</td></tr>"
            "<tr><td><b>Built with</b></td><td>Qt %5</td></tr>"
            "<tr><td><b>Platform</b></td><td>%6</td></tr>"
            "</table>")
            .arg(appVersion().toHtmlEscaped(), appAuthor().toHtmlEscaped(),
                 link.arg(repositoryUrl()), link.arg(issuesUrl()),
                 QString::fromLatin1(qVersion()).toHtmlEscaped(),
                 QSysInfo::prettyProductName().toHtmlEscaped());

    auto* text = new QLabel(html, &dialog);
    text->setTextFormat(Qt::RichText);
    text->setOpenExternalLinks(true);  // opens in the system browser
    text->setTextInteractionFlags(Qt::TextBrowserInteraction);
    text->setWordWrap(true);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(text);
    layout->addWidget(buttons);
    dialog.exec();
}

}  // namespace openbc::app
