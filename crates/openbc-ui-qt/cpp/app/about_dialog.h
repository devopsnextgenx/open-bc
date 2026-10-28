// about_dialog.h
// ---------------------------------------------------------------------------
// Help > About: version details, links to the git repository and its issues
// page, and the credits. Definition lives in about_dialog.cpp.
// ---------------------------------------------------------------------------
#pragma once

class QWidget;

namespace openbc::app {

// Modal; returns when the user closes it.
void showAboutDialog(QWidget* parent);

}  // namespace openbc::app
