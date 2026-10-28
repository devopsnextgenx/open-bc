// first_run_dialog.cpp - see first_run_dialog.h
#include "app/first_run_dialog.h"

#include <QCheckBox>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "backend/backend_api.h"

namespace openbc::app {

namespace {

// Masked form of `email` as it would be sent, or an empty string if it is not a valid address.
QString maskedPreview(const QString& email) {
    const QByteArray bytes = email.trimmed().toUtf8();
    std::size_t length = 0;
    char* masked = openbc_mask_email(reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                                     static_cast<std::size_t>(bytes.size()), &length);
    if (!masked) return QString();
    const QString result = QString::fromUtf8(masked, static_cast<int>(length));
    openbc_instrumentation_report_destroy(masked);
    return result;
}

// Returns false (and stores nothing) if the address is invalid.
bool submitEmail(const QString& email) {
    const QByteArray bytes = email.trimmed().toUtf8();
    std::size_t length = 0;
    char* masked = openbc_first_run_submit_email(reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                                                 static_cast<std::size_t>(bytes.size()), &length);
    if (!masked) return false;
    openbc_instrumentation_report_destroy(masked);
    return true;
}

}  // namespace

void maybeShowFirstRunPrompt(QWidget* parent) {
    if (!openbc_first_run_prompt_pending()) return;
    // Persist "shown" first: whatever happens next, the popup is never offered again.
    openbc_first_run_prompt_mark_shown();

    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("Help improve OpenBC"));
    dialog.setMinimumWidth(480);

    auto* intro = new QLabel(
        QStringLiteral(
            "<h3 style=\"margin:0\">Help improve OpenBC</h3>"
            "<p>OpenBC keeps anonymous usage statistics so we can see which tabs and features are "
            "used. Usage is tied only to a random installation ID generated on this computer.</p>"
            "<ul style=\"margin-left:-20px\">"
            "<li><b>Collected:</b> application version, operating system, and which application "
            "tabs and features are used and for how long.</li>"
            "<li><b>Not collected:</b> file names, file paths, file contents or any other user data. "
            "No personally identifiable information (PII) is collected.</li>"
            "</ul>"),
        &dialog);
    intro->setTextFormat(Qt::RichText);
    intro->setWordWrap(true);

    auto* emailInfo = new QLabel(
        QStringLiteral(
            "<p><b>Email (optional).</b> You may share your email address. It is partially masked "
            "on your computer before anything is sent &ndash; only the first 4 and last 4 letters before "
            "the @ stay visible and the domain is kept, e.g. "
            "<i>jonathan.smith@example.com</i> becomes <i>jonaxxxxxxmith@example.com</i>. "
            "It will not be used for marketing or anything similar.</p>"),
        &dialog);
    emailInfo->setTextFormat(Qt::RichText);
    emailInfo->setWordWrap(true);

    auto* email = new QLineEdit(&dialog);
    email->setPlaceholderText(QStringLiteral("you@example.com (optional)"));
    email->setClearButtonEnabled(true);

    auto* preview = new QLabel(&dialog);
    preview->setWordWrap(true);
    auto* error = new QLabel(&dialog);
    error->setStyleSheet(QStringLiteral("color:#f85149"));
    error->setWordWrap(true);
    error->hide();

    auto* optOut = new QCheckBox(QStringLiteral("Don't share my email (opt out)"), &dialog);

    auto* note = new QLabel(
        QStringLiteral("OpenBC works the same either way. This question is only asked once."), &dialog);
    note->setWordWrap(true);

    auto* continueButton = new QPushButton(QStringLiteral("Continue"), &dialog);
    continueButton->setDefault(true);

    auto refresh = [=]() {
        error->hide();
        email->setEnabled(!optOut->isChecked());
        const QString text = email->text().trimmed();
        if (optOut->isChecked() || text.isEmpty()) {
            preview->setText(optOut->isChecked()
                                 ? QStringLiteral("No email will be shared.")
                                 : QString());
            return;
        }
        const QString masked = maskedPreview(text);
        preview->setText(masked.isEmpty() ? QString()
                                          : QStringLiteral("Will be sent as: <b>%1</b>")
                                                .arg(masked.toHtmlEscaped()));
    };
    QObject::connect(optOut, &QCheckBox::toggled, &dialog, refresh);
    QObject::connect(email, &QLineEdit::textChanged, &dialog, refresh);

    QObject::connect(continueButton, &QPushButton::clicked, &dialog, [&dialog, email, optOut, error]() {
        const QString text = email->text().trimmed();
        if (optOut->isChecked() || text.isEmpty()) {
            dialog.accept();  // nothing shared
            return;
        }
        if (!submitEmail(text)) {
            error->setText(QStringLiteral(
                "That doesn't look like a valid email address. Fix it, clear the field, or tick "
                "\"Don't share my email\"."));
            error->show();
            return;
        }
        dialog.accept();
    });

    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(intro);
    layout->addWidget(emailInfo);
    layout->addWidget(email);
    layout->addWidget(preview);
    layout->addWidget(error);
    layout->addWidget(optOut);
    layout->addWidget(note);
    layout->addWidget(continueButton, 0, Qt::AlignRight);

    // Closing with Esc / the window's close button counts as "don't share".
    dialog.exec();
}

}  // namespace openbc::app
