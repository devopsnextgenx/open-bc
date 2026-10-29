// options_status.h
// ---------------------------------------------------------------------------
// ONE place that says how finished each Tools > Options page is. Qt-free on purpose
// (like drop_router.h) so the dialog, tests and docs can share it.
//
//   Implemented : the page is persisted AND applied (its values really change app behaviour).
//   InProgress  : the controls exist and edit the draft, but the values are only kept in
//                 memory (OptionsStore is a mock for them): not saved, not applied yet.
//   Pending     : placeholder page, no design yet.
//
// Keep this table and crates/openbc-ui-qt/options.md in step: when you finish wiring a page
// (load + persist + a listener that applies it), move its row here AND in options.md.
// OptionsDialog groups its page list by this status and shows a banner on unfinished pages.
// ---------------------------------------------------------------------------
#pragma once

#include <string>

namespace openbc::app::options_status {

enum class Status { Implemented, InProgress, Pending };

struct PageStatus {
    const char* page;    // must match the title passed to OptionsDialog::addPage
    Status status;
    const char* note;    // one line shown in the banner / tooltip of unfinished pages
};

inline constexpr PageStatus kPages[] = {
    {"Syntax Highlighting", Status::Implemented, "Saved to preferences.ini and applied to open comparisons."},
    {"Startup", Status::InProgress, "Work in progress: changes are not saved or applied yet."},
    {"Tabs", Status::InProgress, "Work in progress: changes are not saved or applied yet."},
    {"Appearance", Status::InProgress, "Work in progress: changes are not saved or applied yet."},
    {"Text Editing", Status::InProgress, "Work in progress: changes are not saved or applied yet."},
    {"Next Difference", Status::InProgress, "Work in progress: changes are not saved or applied yet."},
    {"Backups", Status::InProgress, "Work in progress: changes are not saved or applied yet."},
    {"File Operations", Status::InProgress, "Work in progress: changes are not saved or applied yet."},
    {"Commands", Status::InProgress, "Work in progress: changes are not saved or applied yet."},
    {"Tweaks", Status::InProgress, "Work in progress: changes are not saved or applied yet."},
    {"Archive Types", Status::Pending, "Pending: not designed yet."},
    {"Open With", Status::Pending, "Pending: not designed yet."},
};

// Pages not listed above count as Pending, so a new page can never look finished by accident.
inline const PageStatus* find(const std::string& page) {
    for (const PageStatus& p : kPages)
        if (page == p.page) return &p;
    return nullptr;
}

inline Status statusOf(const std::string& page) {
    const PageStatus* p = find(page);
    return p ? p->status : Status::Pending;
}

inline const char* groupLabel(Status s) {
    switch (s) {
    case Status::Implemented: return "IMPLEMENTED";
    case Status::InProgress: return "IN PROGRESS";
    case Status::Pending: return "PENDING";
    }
    return "PENDING";
}

}  // namespace openbc::app::options_status
