// first_run_dialog.h
// ---------------------------------------------------------------------------
// One-time "help improve OpenBC" popup: explains what anonymous usage tracking
// covers and lets the user optionally share an email (masked before sending)
// or opt out of sharing it. Definition lives in first_run_dialog.cpp.
// ---------------------------------------------------------------------------
#pragma once

class QWidget;

namespace openbc::app {

// Shows the popup at most once per installation: the "shown" flag is stored
// before the dialog appears, so closing it any way (or a crash) never brings
// it back. Does nothing when tracking is off or the popup was already shown.
void maybeShowFirstRunPrompt(QWidget* parent);

}  // namespace openbc::app
