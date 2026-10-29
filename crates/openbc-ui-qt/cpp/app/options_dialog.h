// options_dialog.h
// ---------------------------------------------------------------------------
// OptionsDialog: Tools > Options. A searchable page list on the left and a
// stacked settings page on the right, with OK / Cancel / Apply.
//
// Pages: Startup, Tabs, Appearance, Text Editing, Syntax Highlighting, Next Difference, Backups,
// File Operations, Archive Types, Commands, Open With, Tweaks.
// The page list is grouped by completion status (app/options_status.h): IMPLEMENTED, IN PROGRESS
// and PENDING, with a separator row per group and a banner on every unfinished page. Archive Types
// and Open With have no design yet - they are placeholders. Progress is documented in
// crates/openbc-ui-qt/options.md.
//
// Editing model: the dialog works on a private copy of AppOptions (`draft_`).
// Every control writes straight into that copy through the small bind helpers
// below. OK/Apply pass the copy to OptionsStore::commit(); Cancel just drops
// it. So the committed options are never half-edited. Today commit() only
// keeps the values in memory (see app_options.h); wiring real persistence and
// live reload happens in OptionsStore, not here.
//
// Like the other dialogs in this tree it is header-only and uses lambdas
// instead of Q_OBJECT slots.
// ---------------------------------------------------------------------------
#pragma once

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QButtonGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QCompleter>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFont>
#include <QFontDialog>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequence>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPalette>
#include <QVariant>
#include <QPixmap>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSet>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTableWidget>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

#include "app/app_options.h"
#include "app/options_status.h"
#include "app/options_icons.h"

namespace openbc::app {

// A button that shows a color swatch + hex and opens QColorDialog on click.
class OptionsColorButton : public QPushButton {
public:
    std::function<void(const QColor&)> onChanged;

    explicit OptionsColorButton(QWidget* parent = nullptr) : QPushButton(parent) {
        setMinimumWidth(150);
        connect(this, &QPushButton::clicked, this, [this]() {
            const QColor picked = QColorDialog::getColor(color_, this, "Select Color");
            if (!picked.isValid()) return;
            setColor(picked);
            if (onChanged) onChanged(picked);
        });
    }

    void setColor(const QColor& color) {
        color_ = color;
        QPixmap swatch(96, 12);
        swatch.fill(color);
        setIcon(QIcon(swatch));
        setIconSize(swatch.size());
        setText(color.name().toUpper());
    }

private:
    QColor color_;
};

class OptionsDialog : public QDialog {
public:
    explicit OptionsDialog(OptionsStore& store, QWidget* parent = nullptr)
        : QDialog(parent), store_(store), draft_(store.options()) {
        setWindowTitle("Options");
        resize(780, 640);

        auto* root = new QVBoxLayout(this);
        auto* body = new QHBoxLayout;
        root->addLayout(body, 1);

        auto* nav = new QVBoxLayout;
        search_ = new QLineEdit(this);
        search_->setPlaceholderText("Search options");
        search_->setClearButtonEnabled(true);
        nav->addWidget(search_);
        pages_ = new QListWidget(this);
        pages_->setObjectName("optionsNav");
        pages_->setFixedWidth(200);
        pages_->setIconSize(QSize(20, 20));
        pages_->setFrameShape(QFrame::NoFrame);
        pages_->setStyleSheet(
            "QListWidget#optionsNav { background: palette(window); outline: 0; }"
            "QListWidget#optionsNav::item { padding: 7px 10px; }"
            "QListWidget#optionsNav::item:selected { background: palette(highlight);"
            " color: palette(highlighted-text); }");
        nav->addWidget(pages_, 1);
        body->addLayout(nav);

        stack_ = new QStackedWidget(this);
        body->addWidget(stack_, 1);

        // Build every page, then list them grouped by completion status (options_status.h):
        // IMPLEMENTED, IN PROGRESS, PENDING - each group starts with a separator row.
        const std::vector<std::pair<QString, QWidget*>> built = {
            {"Startup", buildStartup()},
            {"Tabs", buildTabs()},
            {"Appearance", buildAppearance()},
            {"Text Editing", buildTextEditing()},
            {"Syntax Highlighting", buildSyntaxHighlighting()},
            {"Next Difference", buildNextDifference()},
            {"Backups", buildBackups()},
            {"File Operations", buildFileOperations()},
            {"Archive Types", buildPlaceholder("Archive Types")},
            {"Commands", buildCommands()},
            {"Open With", buildPlaceholder("Open With")},
            {"Tweaks", buildTweaks()},
        };
        using options_status::Status;
        for (const Status group : {Status::Implemented, Status::InProgress, Status::Pending}) {
            bool headerAdded = false;
            for (const auto& [title, content] : built) {
                if (options_status::statusOf(title.toStdString()) != group) continue;
                if (!headerAdded) {
                    addGroupHeader(group);
                    headerAdded = true;
                }
                addPage(title, content);
            }
        }

        buttons_ = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply, this);
        apply_ = buttons_->button(QDialogButtonBox::Apply);
        apply_->setEnabled(false);
        root->addWidget(buttons_);

        // Rows are not stack indexes any more (group headers sit in between): each page row
        // carries its stack index in Qt::UserRole, header rows carry none.
        connect(pages_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* current, QListWidgetItem*) {
            if (!current) return;
            const QVariant index = current->data(Qt::UserRole);
            if (index.isValid()) stack_->setCurrentIndex(index.toInt());
        });
        connect(search_, &QLineEdit::textChanged, this, [this](const QString& t) { filterPages(t); });
        connect(buttons_, &QDialogButtonBox::accepted, this, [this]() { commit(); accept(); });
        connect(buttons_, &QDialogButtonBox::rejected, this, [this]() { reject(); });
        connect(apply_, &QPushButton::clicked, this, [this]() { commit(); });

        for (int i = 0; i < pages_->count(); ++i) {  // first real page, never a group header
            if (pages_->item(i)->data(Qt::UserRole).isValid()) {
                pages_->setCurrentRow(i);
                break;
            }
        }
    }

    // Opens the dialog modally; true when the user pressed OK.
    static bool edit(OptionsStore& store, QWidget* parent) {
        OptionsDialog dialog(store, parent);
        return dialog.exec() == QDialog::Accepted;
    }

private:
    // ---- plumbing ---------------------------------------------------------
    void markDirty() { apply_->setEnabled(true); }

    void commit() {
        store_.commit(draft_);
        apply_->setEnabled(false);
    }

    // Separator row that starts a status group. Not selectable, so keyboard and mouse skip it.
    void addGroupHeader(options_status::Status status) {
        auto* header = new QListWidgetItem(QString::fromLatin1(options_status::groupLabel(status)));
        header->setFlags(Qt::ItemIsEnabled);
        QFont f = header->font();
        f.setBold(true);
        if (f.pointSizeF() > 0) f.setPointSizeF(f.pointSizeF() * 0.85);
        header->setFont(f);
        header->setForeground(palette().color(QPalette::PlaceholderText));
        header->setSizeHint(QSize(0, 30));
        pages_->addItem(header);
    }

    void addPage(const QString& title, QWidget* content) {
        // Everything the search box can match on this page (collected before the status banner
        // is added, so searching "progress" does not match every unfinished page).
        QStringList words{title};
        for (auto* b : content->findChildren<QAbstractButton*>()) words << b->text();
        for (auto* l : content->findChildren<QLabel*>()) words << l->text();
        for (auto* g : content->findChildren<QGroupBox*>()) words << g->title();

        const options_status::PageStatus* status = options_status::find(title.toStdString());
        const bool finished = status && status->status == options_status::Status::Implemented;
        if (!finished) {
            // Say so on the page itself, not only in the list.
            auto* banner = new QLabel(status ? QString::fromUtf8(status->note) : QStringLiteral("Pending: not designed yet."));
            banner->setWordWrap(true);
            banner->setStyleSheet(
                "QLabel { background: palette(alternate-base); border: 1px solid palette(mid);"
                " border-radius: 4px; padding: 6px 8px; }");
            if (auto* box = qobject_cast<QVBoxLayout*>(content->layout())) box->insertWidget(0, banner);
        }

        auto* scroll = new QScrollArea;
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        scroll->setWidget(content);
        const int stackIndex = stack_->addWidget(scroll);
        auto* item = new QListWidgetItem(options_icons::forPage(title), title);
        item->setData(Qt::UserRole, stackIndex);
        if (status) item->setToolTip(QString::fromUtf8(status->note));
        if (status && status->status == options_status::Status::Pending)
            item->setForeground(palette().color(QPalette::PlaceholderText));
        pages_->addItem(item);

        searchText_.push_back(words.join('\n').remove('&').toLower());  // indexed by stack index
    }

    void filterPages(const QString& needle) {
        const QString n = needle.trimmed().toLower();
        QListWidgetItem* firstVisible = nullptr;
        QListWidgetItem* header = nullptr;
        bool headerHasVisible = false;
        auto closeGroup = [&]() {
            if (header) header->setHidden(!headerHasVisible);  // an empty group hides its separator too
        };
        for (int i = 0; i < pages_->count(); ++i) {
            QListWidgetItem* item = pages_->item(i);
            const QVariant index = item->data(Qt::UserRole);
            if (!index.isValid()) {
                closeGroup();
                header = item;
                headerHasVisible = false;
                continue;
            }
            const bool match = n.isEmpty() || searchText_[static_cast<size_t>(index.toInt())].contains(n);
            item->setHidden(!match);
            if (match) {
                headerHasVisible = true;
                if (!firstVisible) firstVisible = item;
            }
        }
        closeGroup();
        if (firstVisible && pages_->currentItem() && pages_->currentItem()->isHidden())
            pages_->setCurrentItem(firstVisible);
    }

    static QLabel* heading(const QString& text) {
        auto* label = new QLabel(text);
        QFont f = label->font();
        f.setBold(true);
        if (f.pointSizeF() > 0) f.setPointSizeF(f.pointSizeF() * 1.4);
        label->setFont(f);
        return label;
    }

    static QHBoxLayout* indented(QWidget* widget) {
        auto* row = new QHBoxLayout;
        row->setContentsMargins(22, 0, 0, 0);
        row->addWidget(widget);
        row->addStretch(1);
        return row;
    }

    static QFrame* rule() {
        auto* line = new QFrame;
        line->setFrameShape(QFrame::HLine);
        line->setFrameShadow(QFrame::Plain);
        return line;
    }

    static QLabel* note(const QString& text) {
        auto* label = new QLabel(text);
        label->setWordWrap(true);
        return label;
    }

    static QWidget* pageShell(QVBoxLayout** layoutOut) {
        auto* page = new QWidget;
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(14, 8, 14, 8);
        layout->setSpacing(7);
        *layoutOut = layout;
        return page;
    }

    // ---- bind helpers: widget <-> a field inside draft_ -------------------
    QCheckBox* check(const QString& text, bool* field) {
        auto* box = new QCheckBox(text);
        box->setChecked(*field);
        connect(box, &QCheckBox::toggled, this, [this, field](bool on) {
            *field = on;
            markDirty();
        });
        return box;
    }

    template <typename Enum>
    QRadioButton* radio(const QString& text, Enum* field, Enum value, QButtonGroup* group) {
        auto* button = new QRadioButton(text);
        group->addButton(button);
        button->setChecked(*field == value);
        connect(button, &QRadioButton::toggled, this, [this, field, value](bool on) {
            if (!on) return;
            *field = value;
            markDirty();
        });
        return button;
    }

    QSpinBox* spin(int* field, int minimum, int maximum, const QString& suffix = QString()) {
        auto* box = new QSpinBox;
        box->setRange(minimum, maximum);
        box->setSuffix(suffix);
        box->setValue(*field);
        connect(box, qOverload<int>(&QSpinBox::valueChanged), this, [this, field](int v) {
            *field = v;
            markDirty();
        });
        return box;
    }

    QComboBox* stringCombo(QString* field, bool editable) {
        auto* combo = new QComboBox;
        combo->setEditable(editable);
        combo->setMinimumWidth(220);
        combo->setCurrentText(*field);
        connect(combo, &QComboBox::currentTextChanged, this, [this, field](const QString& t) {
            *field = t;
            markDirty();
        });
        return combo;
    }

    // "Font..." button that stores QFont::toString() in `field`.
    QPushButton* fontButton(QString* field) {
        auto* button = new QPushButton("Font...");
        connect(button, &QPushButton::clicked, this, [this, field]() {
            QFont current;
            if (!field->isEmpty()) current.fromString(*field);
            bool ok = false;
            const QFont chosen = QFontDialog::getFont(&ok, current, this);
            if (!ok) return;
            *field = chosen.toString();
            markDirty();
        });
        return button;
    }

    // ---- Startup ----------------------------------------------------------
    QWidget* buildStartup() {
        QVBoxLayout* v;
        auto* page = pageShell(&v);
        v->addWidget(heading("Startup"));

        auto* row = new QHBoxLayout;
        auto* loadCol = new QVBoxLayout;
        loadCol->addWidget(new QLabel("Load &workspace:"));
        loadCol->addWidget(stringCombo(&draft_.loadWorkspace, false));
        auto* saveCol = new QVBoxLayout;
        saveCol->addWidget(new QLabel("On exit, sa&ve workspace as:"));
        saveCol->addWidget(stringCombo(&draft_.saveWorkspaceOnExit, true));
        row->addLayout(loadCol, 1);
        row->addLayout(saveCol, 1);
        v->addLayout(row);

        auto* quick = check("When starting with file comparison, show quick compare dialog",
                            &draft_.showQuickCompareDialog);
        v->addWidget(quick);
        auto* group = new QButtonGroup(this);
        auto* binary = radio("&Binary quick compare", &draft_.quickCompareMode,
                             QuickCompareMode::Binary, group);
        auto* rules = radio("&Rules-based quick compare", &draft_.quickCompareMode,
                            QuickCompareMode::RulesBased, group);
        auto* openView = check("&Open view automatically if files are different",
                               &draft_.openViewIfFilesDiffer);
        for (QWidget* w : {static_cast<QWidget*>(binary), static_cast<QWidget*>(rules),
                           static_cast<QWidget*>(openView)}) {
            v->addLayout(indented(w));
            w->setEnabled(quick->isChecked());
            connect(quick, &QCheckBox::toggled, w, &QWidget::setEnabled);
        }

        v->addWidget(rule());
        v->addWidget(heading("Clipboard Compare"));
        v->addWidget(check("Run &Clipboard Compare on Windows startup",
                           &draft_.runClipboardCompareOnStartup));

        v->addWidget(rule());
        v->addWidget(heading("Explorer Integration"));
        auto* integrate = check("Include OpenBC in &Explorer context menu", &draft_.explorerIntegration);
        v->addWidget(integrate);

        auto* table = new QTableWidget(draft_.explorerCommands.size(), 2);
        table->setHorizontalHeaderLabels({"Command", "Location"});
        table->verticalHeader()->hide();
        table->horizontalHeader()->setStretchLastSection(true);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setFixedHeight(150);
        for (int i = 0; i < draft_.explorerCommands.size(); ++i) {
            auto* name = new QTableWidgetItem(draft_.explorerCommands[i].command);
            name->setFlags(name->flags() & ~Qt::ItemIsEditable);
            table->setItem(i, 0, name);
            auto* where = new QComboBox;
            where->addItems({"Main menu", "Submenu", "None"});
            where->setCurrentText(draft_.explorerCommands[i].location);
            table->setCellWidget(i, 1, where);
            connect(where, &QComboBox::currentTextChanged, this, [this, i](const QString& t) {
                draft_.explorerCommands[i].location = t;
                markDirty();
            });
        }
        table->setColumnWidth(0, 220);
        v->addWidget(table);
        auto* hideLeft = check("Hide \"Select Left\" if there is already a saved selection",
                               &draft_.hideSelectLeftIfSaved);
        v->addWidget(hideLeft);
        for (QWidget* w : {static_cast<QWidget*>(table), static_cast<QWidget*>(hideLeft)}) {
            w->setEnabled(integrate->isChecked());
            connect(integrate, &QCheckBox::toggled, w, &QWidget::setEnabled);
        }
        v->addStretch(1);
        return page;
    }

    // ---- Tabs -------------------------------------------------------------
    QWidget* buildTabs() {
        QVBoxLayout* v;
        auto* page = pageShell(&v);
        v->addWidget(heading("Tabs"));

        auto openIn = [&](const QString& title, OpenSessionsIn* field) {
            v->addWidget(new QLabel(title));
            auto* group = new QButtonGroup(this);
            v->addWidget(radio("A new &window", field, OpenSessionsIn::NewWindow, group));
            v->addWidget(radio("A new &tab", field, OpenSessionsIn::NewTab, group));
        };
        openIn("Open sessions in:", &draft_.openSessionsIn);
        openIn("Open child sessions in:", &draft_.openChildSessionsIn);

        v->addWidget(check("Open &new tab next to active tab", &draft_.openNewTabNextToActive));
        v->addWidget(check("Closing &last remaining tab closes the window",
                           &draft_.closingLastTabClosesWindow));
        v->addWidget(check("&Warn when closing multiple tabs", &draft_.warnClosingMultipleTabs));
        v->addWidget(check("Warn when closing multiple windo&ws", &draft_.warnClosingMultipleWindows));
        v->addWidget(check("&Hide tab bar if single tab", &draft_.hideTabBarIfSingleTab));
        v->addWidget(check("Show pre&views for individual tabs in the taskbar",
                           &draft_.showTabPreviewsInTaskbar));
        v->addSpacing(8);
        v->addWidget(note("For settings that control tab stops in text files, see "
                          "<b>Tools &gt; File Formats</b>"));
        v->addStretch(1);
        return page;
    }

    // ---- Appearance -------------------------------------------------------
    using ColorTree = QList<std::pair<QString, QStringList>>;   // item -> children

    QWidget* buildAppearance() {
        QVBoxLayout* v;
        auto* page = pageShell(&v);
        v->addWidget(heading("Theme, Colors, Fonts"));

        auto* theme = new QComboBox;
        theme->addItems({"Follow operating system", "Light", "Dark"});
        theme->setCurrentIndex(static_cast<int>(draft_.theme));
        connect(theme, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int i) {
            draft_.theme = static_cast<ThemeChoice>(i);
            markDirty();
        });
        v->addWidget(theme, 0, Qt::AlignLeft);
        v->addWidget(check("&Preview", &draft_.previewTheme));

        auto* tabs = new QTabWidget;
        tabs->addTab(buildColorTab("Folder Views",
                                   {{"Default text", {}}, {"Log panel", {}}, {"Selection", {}},
                                    {"Filtered out", {}},
                                    {"Compare colors", {"Unknown", "Same", "Orphan", "Older", "Newer", "Different"}},
                                    {"Merge colors", {"Left only", "Right only", "Conflict"}}},
                                   true),
                     "Folder Views");
        tabs->addTab(buildColorTab("File Views",
                                   {{"Default text", {}}, {"Line numbers", {}}, {"Selection", {}},
                                    {"Compare colors", {"Same", "Different", "Left only", "Right only", "Unimportant"}}},
                                   false),
                     "File Views");
        tabs->addTab(buildColorTab("Picture Compare",
                                   {{"Background", {}}, {"Compare colors", {"Same pixels", "Different pixels", "Left only", "Right only"}}},
                                   false),
                     "Picture Compare");
        v->addWidget(tabs, 1);

        auto* systemFont = check("Use s&ystem font", &draft_.useSystemFont);
        v->addWidget(systemFont);
        auto* font = fontButton(&draft_.customFont);
        font->setEnabled(!draft_.useSystemFont);
        connect(systemFont, &QCheckBox::toggled, font, [font](bool on) { font->setEnabled(!on); });
        v->addWidget(font, 0, Qt::AlignLeft);
        v->addWidget(check("Color &folders to reflect content", &draft_.colorFoldersToReflectContent));
        v->addWidget(check("Use &gradient for center background", &draft_.useGradientForCenterBackground));
        v->addWidget(note("Additional color schemes will be available from the OpenBC website."));
        return page;
    }

    QWidget* buildColorTab(const QString& tab, const ColorTree& items, bool folderExtras) {
        auto* widget = new QWidget;
        auto* row = new QHBoxLayout(widget);

        auto* tree = new QTreeWidget;
        tree->setHeaderHidden(true);
        tree->setMinimumHeight(190);
        for (const auto& [name, children] : items) {
            auto* top = new QTreeWidgetItem(tree, QStringList{name});
            for (const QString& child : children) new QTreeWidgetItem(top, QStringList{child});
        }
        tree->expandAll();
        row->addWidget(tree, 1);

        auto* side = new QVBoxLayout;
        auto* textColor = new OptionsColorButton;
        auto* background = new OptionsColorButton;
        auto* folderInfo = new OptionsColorButton;
        side->addWidget(new QLabel("Text color:"));
        side->addWidget(textColor);
        side->addWidget(new QLabel("Background:"));
        side->addWidget(background);
        if (folderExtras) {
            side->addWidget(new QLabel("Folder info:"));
            side->addWidget(folderInfo);
            side->addWidget(check("Use stripes", &draft_.useStripes));
        } else {
            folderInfo->hide();
        }
        side->addStretch(1);
        row->addLayout(side);

        auto currentKey = [tree]() {
            auto* item = tree->currentItem();
            return item ? item->text(0) : QString();
        };
        auto editColors = [this, tab, currentKey](std::function<void(ItemColors&)> change) {
            const QString item = currentKey();
            if (item.isEmpty()) return;
            ItemColors colors = draft_.colorsFor(tab, item);
            change(colors);
            draft_.colors[tab + "/" + item] = colors;
            markDirty();
        };
        textColor->onChanged = [editColors](const QColor& c) { editColors([&](ItemColors& i) { i.text = c; }); };
        background->onChanged = [editColors](const QColor& c) { editColors([&](ItemColors& i) { i.background = c; }); };
        folderInfo->onChanged = [editColors](const QColor& c) { editColors([&](ItemColors& i) { i.folderInfo = c; }); };

        connect(tree, &QTreeWidget::currentItemChanged, this,
                [this, tab, textColor, background, folderInfo](QTreeWidgetItem* item, QTreeWidgetItem*) {
                    const bool leaf = item && item->childCount() == 0;
                    for (auto* b : {textColor, background, folderInfo}) b->setEnabled(leaf);
                    if (!item) return;
                    const ItemColors c = draft_.colorsFor(tab, item->text(0));
                    textColor->setColor(c.text);
                    background->setColor(c.background);
                    folderInfo->setColor(c.folderInfo);
                });
        tree->setCurrentItem(tree->topLevelItem(0));
        return widget;
    }

    // ---- Text Editing -----------------------------------------------------
    QWidget* buildTextEditing() {
        QVBoxLayout* v;
        auto* page = pageShell(&v);
        v->addWidget(heading("Text Editing"));
        v->addWidget(check("Auto in&dent", &draft_.autoIndent));
        v->addWidget(check("Bac&kspace unindents", &draft_.backspaceUnindents));
        v->addWidget(check("Initialize \"Text to find\" to be current &word", &draft_.initFindWithCurrentWord));
        v->addWidget(check("Show filtered line counts", &draft_.showFilteredLineCounts));
        v->addWidget(new QLabel("Number of conte&xt lines:"));
        v->addWidget(spin(&draft_.contextLines, 0, 99), 0, Qt::AlignLeft);
        v->addSpacing(8);
        v->addWidget(note("To set tab stops, use <b>Tools &gt; File Formats</b>"));
        v->addStretch(1);
        return page;
    }

    // ---- Syntax Highlighting ----------------------------------------------
    // One table with the effective rule per extension / file name (built-in or the user's own),
    // and an editor row to add or change a rule. Only draft_.syntaxMappings is edited; OK/Apply
    // saves it to the profile and open comparisons re-highlight (see syntax_mappings.h).
    QWidget* buildSyntaxHighlighting() {
        QVBoxLayout* v;
        auto* page = pageShell(&v);
        v->addWidget(heading("Syntax Highlighting"));
        const SyntaxCatalog& catalog = SyntaxCatalog::get();
        v->addWidget(note("Choose the highlighting language for a file extension (for example h or cpp) or an "
                          "exact file name (for example Makefile). Your mappings override the built-in ones, are "
                          "saved in your profile, and also apply to comparisons that are already open once you "
                          "press OK or Apply."));

        auto* filterRow = new QHBoxLayout;
        auto* filter = new QLineEdit;
        filter->setPlaceholderText("Filter by extension, file name or language");
        filter->setClearButtonEnabled(true);
        auto* mineOnly = new QCheckBox("Show only my mappings");
        filterRow->addWidget(filter, 1);
        filterRow->addWidget(mineOnly);
        v->addLayout(filterRow);

        auto* table = new QTableWidget(0, 4);
        table->setObjectName("syntaxMappingsTable");
        table->setHorizontalHeaderLabels({"Extension / file name", "Language", "Source", "Highlighting"});
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->verticalHeader()->hide();
        table->horizontalHeader()->setStretchLastSection(true);
        table->setColumnWidth(0, 180);
        table->setColumnWidth(1, 170);
        table->setColumnWidth(2, 190);
        table->setMinimumHeight(260);
        v->addWidget(table, 1);

        auto* summary = new QLabel(QString("%1 built-in languages, %2 built-in extensions and file names.")
                                       .arg(catalog.languages().size())
                                       .arg(catalog.entries().size()));
        v->addWidget(summary);

        v->addWidget(rule());
        auto* form = new QGridLayout;
        auto* key = new QLineEdit;
        key->setObjectName("syntaxMappingKey");
        key->setPlaceholderText("cpp, h, Makefile");
        auto* language = new QComboBox;
        language->setObjectName("syntaxMappingLanguage");
        language->setEditable(true);
        language->setInsertPolicy(QComboBox::NoInsert);
        language->setMinimumWidth(220);
        QStringList names;
        for (const SyntaxLanguage& entry : catalog.languages()) names << entry.name;
        names.sort(Qt::CaseInsensitive);
        language->addItems(names);
        if (auto* completer = language->completer()) {
            completer->setCompletionMode(QCompleter::PopupCompletion);
            completer->setFilterMode(Qt::MatchContains);
            completer->setCaseSensitivity(Qt::CaseInsensitive);
        }
        language->setCurrentIndex(-1);
        language->lineEdit()->setPlaceholderText("Language (type to search)");
        auto* addButton = new QPushButton("&Add / Update");
        addButton->setObjectName("syntaxMappingAdd");
        auto* removeButton = new QPushButton("&Remove");
        removeButton->setObjectName("syntaxMappingRemove");
        removeButton->setEnabled(false);
        removeButton->setToolTip("Removes your mapping for the selected extension; the built-in rule applies again.");
        form->addWidget(new QLabel("Extension or file name:"), 0, 0);
        form->addWidget(new QLabel("Language:"), 0, 1);
        form->addWidget(key, 1, 0);
        form->addWidget(language, 1, 1);
        form->addWidget(addButton, 1, 2);
        form->addWidget(removeButton, 1, 3);
        form->setColumnStretch(0, 1);
        form->setColumnStretch(1, 1);
        v->addLayout(form);
        auto* status = new QLabel;
        status->setObjectName("syntaxMappingStatus");
        status->setWordWrap(true);
        v->addWidget(status);

        // Rebuilds the table from the built-in catalog plus draft_.syntaxMappings.
        auto populate = [=]() {
            const QString needle = filter->text().trimmed().toLower();
            QHash<QString, QString> custom;
            for (const SyntaxMapping& m : draft_.syntaxMappings) custom.insert(m.key, m.language);
            struct Row { QString key, language, source; };
            QList<Row> rows;
            QSet<QString> seen;
            for (const SyntaxCatalogEntry& e : catalog.entries()) {
                if (seen.contains(e.key)) continue;
                seen.insert(e.key);
                const auto it = custom.constFind(e.key);
                if (it != custom.constEnd())
                    rows.append({e.key, it.value(), "Custom (overrides " + e.language + ")"});
                else
                    rows.append({e.key, e.language, e.fileName ? "Built-in (file name)" : "Built-in"});
            }
            for (const SyntaxMapping& m : draft_.syntaxMappings)
                if (!seen.contains(m.key)) rows.append({m.key, m.language, "Custom"});
            std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
                return a.key.compare(b.key, Qt::CaseInsensitive) < 0;
            });

            QSignalBlocker block(table);
            table->setRowCount(0);
            for (const Row& r : rows) {
                const bool isCustom = r.source.startsWith("Custom");
                if (mineOnly->isChecked() && !isCustom) continue;
                if (!needle.isEmpty() && !r.key.toLower().contains(needle) &&
                    !r.language.toLower().contains(needle) && !r.source.toLower().contains(needle))
                    continue;
                const SyntaxLanguage* info = catalog.findLanguage(r.language);
                QString quality = "Exact";
                if (info && info->engine == "plain") quality = "None (plain text)";
                else if (info && info->approximate) quality = "Approximate";
                const int row = table->rowCount();
                table->insertRow(row);
                const QStringList cells{r.key, r.language, r.source, quality};
                for (int c = 0; c < cells.size(); ++c) {
                    auto* item = new QTableWidgetItem(cells[c]);
                    if (isCustom) {
                        QFont f = item->font();
                        f.setBold(true);
                        item->setFont(f);
                    }
                    table->setItem(row, c, item);
                }
            }
            removeButton->setEnabled(false);
        };

        // Selecting a row loads it into the editor row.
        auto syncEditor = [=]() {
            if (table->selectedItems().isEmpty()) {
                removeButton->setEnabled(false);
                return;
            }
            const int row = table->currentRow();
            key->setText(table->item(row, 0)->text());
            language->setCurrentText(table->item(row, 1)->text());
            removeButton->setEnabled(table->item(row, 2)->text().startsWith("Custom"));
        };
        auto selectKey = [=](const QString& wanted) {
            for (int row = 0; row < table->rowCount(); ++row) {
                if (table->item(row, 0)->text() != wanted) continue;
                table->selectRow(row);
                table->scrollToItem(table->item(row, 0));
                syncEditor();
                return;
            }
        };

        connect(filter, &QLineEdit::textChanged, this, [populate](const QString&) { populate(); });
        connect(mineOnly, &QCheckBox::toggled, this, [populate](bool) { populate(); });
        connect(table, &QTableWidget::itemSelectionChanged, this, [=]() {
            syncEditor();
            status->clear();
        });

        connect(addButton, &QPushButton::clicked, this, [=]() {
            const QString k = SyntaxCatalog::normalizeKey(key->text());
            if (k.isEmpty()) {
                status->setText("Enter an extension such as cpp or a file name such as Makefile "
                                "(no spaces, slashes or wildcards).");
                return;
            }
            const SyntaxLanguage* chosen = catalog.findLanguage(language->currentText());
            if (!chosen) {
                status->setText(QString("\"%1\" is not a known language. Pick one from the list.")
                                    .arg(language->currentText().trimmed()));
                return;
            }
            auto& list = draft_.syntaxMappings;
            int existing = -1;
            for (int i = 0; i < list.size(); ++i)
                if (list[i].key == k) existing = i;
            QString message;
            if (catalog.builtInLanguageFor(k) == chosen->name) {
                // Same as the built-in rule: nothing to store, and a leftover override goes away.
                if (existing >= 0) {
                    list.removeAt(existing);
                    markDirty();
                }
                message = QString("%1 uses %2 by default.").arg(k, chosen->name);
            } else {
                if (existing >= 0) list[existing].language = chosen->name;
                else list.append({k, chosen->name});
                markDirty();
                message = QString("%1 will use %2.").arg(k, chosen->name);
            }
            populate();
            selectKey(k);
            status->setText(message);  // after selectKey: selecting a row clears the status
        });

        connect(removeButton, &QPushButton::clicked, this, [=]() {
            if (table->selectedItems().isEmpty()) return;
            const QString k = table->item(table->currentRow(), 0)->text();
            auto& list = draft_.syntaxMappings;
            for (int i = 0; i < list.size(); ++i) {
                if (list[i].key != k) continue;
                list.removeAt(i);
                markDirty();
                break;
            }
            const QString fallback = catalog.builtInLanguageFor(k);
            populate();
            selectKey(k);
            status->setText(fallback.isEmpty() ? QString("Removed the mapping for %1.").arg(k)
                                               : QString("Removed. %1 uses %2 again.").arg(k, fallback));
        });

        populate();
        return page;
    }

    // ---- Next Difference --------------------------------------------------
    QWidget* buildNextDifference() {
        QVBoxLayout* v;
        auto* page = pageShell(&v);
        v->addWidget(heading("Next Difference"));
        v->addWidget(check("When &loading new files, go to first difference", &draft_.goToFirstDifferenceOnLoad));
        v->addWidget(check("Go to next difference in files after &copying to other side",
                           &draft_.goToNextDifferenceAfterCopy));
        v->addWidget(check("Limit \"Next Difference Files\" to current &folder",
                           &draft_.limitNextDifferenceFilesToFolder));
        v->addWidget(new QLabel("When no more differences:"));
        v->addWidget(check("&Wrap around to first difference", &draft_.wrapAroundToFirstDifference));
        v->addWidget(check("Show &message panel", &draft_.showMessagePanel));
        v->addStretch(1);
        return page;
    }

    // ---- Backups ----------------------------------------------------------
    QWidget* buildBackups() {
        QVBoxLayout* v;
        auto* page = pageShell(&v);
        v->addWidget(heading("Backups"));
        auto* beforeCopy = check("Back up files before &copy", &draft_.backupBeforeCopy);
        auto* beforeSave = check("Back up files before &save", &draft_.backupBeforeSave);
        v->addWidget(beforeCopy);
        v->addWidget(beforeSave);

        auto* naming = new QWidget;
        auto* grid = new QGridLayout(naming);
        grid->setContentsMargins(0, 0, 0, 0);
        grid->addWidget(new QLabel("Back up \"abc.txt\" as:"), 0, 0, 1, 3);
        auto* group = new QButtonGroup(this);
        struct Scheme { const char* label; BackupNaming value; };
        const Scheme schemes[] = {
            {"abc.~txt", BackupNaming::TildeInExtension}, {"abc.txt~", BackupNaming::TildeAfterName},
            {"abc.txt.bak", BackupNaming::AppendBak},     {"~abc.txt", BackupNaming::TildePrefix},
            {"abc.bak", BackupNaming::ReplaceWithBak},    {"abc.bak.txt", BackupNaming::BakBeforeExt},
        };
        for (int i = 0; i < 6; ++i)
            grid->addWidget(radio(schemes[i].label, &draft_.backupNaming, schemes[i].value, group),
                            1 + i / 3, i % 3);
        auto* inFolder = radio("abc.txt in this &folder:", &draft_.backupNaming, BackupNaming::InFolder, group);
        grid->addWidget(inFolder, 3, 0, 1, 3);
        auto* folder = new QLineEdit(draft_.backupFolder);
        auto* browse = new QPushButton("...");
        browse->setFixedWidth(32);
        grid->addWidget(folder, 4, 0, 1, 2);
        grid->addWidget(browse, 4, 2, 1, 1, Qt::AlignLeft);
        connect(folder, &QLineEdit::textChanged, this, [this](const QString& t) {
            draft_.backupFolder = t;
            markDirty();
        });
        connect(browse, &QPushButton::clicked, this, [this, folder]() {
            const QString dir = QFileDialog::getExistingDirectory(this, "Backup folder", folder->text());
            if (!dir.isEmpty()) folder->setText(dir);
        });
        v->addWidget(naming);

        auto refresh = [=]() {
            const bool on = beforeCopy->isChecked() || beforeSave->isChecked();
            naming->setEnabled(on);
            folder->setEnabled(on && inFolder->isChecked());
            browse->setEnabled(on && inFolder->isChecked());
        };
        connect(beforeCopy, &QCheckBox::toggled, this, refresh);
        connect(beforeSave, &QCheckBox::toggled, this, refresh);
        connect(inFolder, &QRadioButton::toggled, this, refresh);
        refresh();
        v->addStretch(1);
        return page;
    }

    // ---- File Operations --------------------------------------------------
    QWidget* buildFileOperations() {
        QVBoxLayout* v;
        auto* page = pageShell(&v);
        v->addWidget(heading("Confirmations"));
        v->addWidget(check("Confirm file &copy", &draft_.confirmCopy));
        v->addWidget(check("Confirm file m&ove", &draft_.confirmMove));
        v->addWidget(check("Confirm replacing, deleting &read-only files", &draft_.confirmReplaceReadOnly));
        v->addWidget(check("Confirm replacing, deleting &system files", &draft_.confirmReplaceSystem));
        v->addWidget(check("Confirm &overwriting newer files", &draft_.confirmOverwriteNewer));
        v->addWidget(check("Confirm replacing files during mo&ve", &draft_.confirmReplaceDuringMove));
        v->addWidget(check("Confirm con&tent compare", &draft_.confirmContentCompare));
        auto* del = check("Confirm file &delete", &draft_.confirmDelete);
        v->addWidget(del);
        auto* explicitSide = check("Require e&xplicit side selection in Delete dialog",
                                   &draft_.requireExplicitSideInDelete);
        explicitSide->setEnabled(draft_.confirmDelete);
        connect(del, &QCheckBox::toggled, explicitSide, &QWidget::setEnabled);
        v->addLayout(indented(explicitSide));
        v->addWidget(check("Confirm file me&rge", &draft_.confirmMerge));

        v->addWidget(new QLabel("Synchronize confirmations:"));
        auto* group = new QButtonGroup(this);
        v->addWidget(radio("&Prompt", &draft_.syncConfirmation, SyncConfirmation::Prompt, group));
        v->addWidget(radio("&Yes to All", &draft_.syncConfirmation, SyncConfirmation::YesToAll, group));
        v->addWidget(radio("&No to All", &draft_.syncConfirmation, SyncConfirmation::NoToAll, group));

        v->addWidget(rule());
        v->addWidget(heading("Filtered Items"));
        v->addWidget(note("Actions performed on folders that contain items hidden by display or file "
                          "filters can optionally affect the hidden items."));
        v->addWidget(check("Include &hidden items by default", &draft_.includeHiddenItemsByDefault));
        v->addStretch(1);
        return page;
    }

    // ---- Commands ---------------------------------------------------------
    // MOCK: a fixed sample list per view. The real list will come from the
    // QActions each viewer already exposes (actionsMenuItems() & friends).
    static QList<CommandBinding> mockCommands(const QString& view) {
        QList<CommandBinding> list = {
            {"About OpenBC", "Help", true, false, "", "", "Displays program information, version number and copyright."},
            {"Check for Updates", "Help", true, false, "", "", "Checks whether a newer version of OpenBC is available."},
            {"Close Tab", "Session", true, false, "Ctrl+W", "Ctrl+W", "Closes the current tab."},
            {"Contents", "Help", true, false, "", "", "Opens the help contents."},
            {"Context Sensitive Help", "Help", true, false, "F1", "F1", "Opens help for the current view."},
            {"Edit Text File", "Tools", true, false, "", "", "Opens a text file for editing."},
            {"Exit", "Session", true, false, "", "", "Closes OpenBC."},
            {"New Folder Compare", "Session", true, true, "", "", "Starts a new folder comparison."},
            {"Options", "Tools", true, false, "", "", "Opens the Options dialog."},
        };
        if (view == "Folder Compare")
            list.append({"Expand All", "View", true, true, "", "", "Expands every folder in the comparison."});
        else if (view == "Text Compare")
            list.append({"Next Difference", "Actions", true, true, "F8", "F8", "Jumps to the next difference."});
        else if (view == "Picture Compare")
            list.append({"Zoom to Fit", "View", true, true, "", "", "Fits both pictures in the window."});
        return list;
    }

    QWidget* buildCommands() {
        QVBoxLayout* v;
        auto* page = pageShell(&v);
        v->addWidget(heading("Toolbars, Shortcuts, Menus"));

        v->addWidget(new QLabel("Select &view:"));
        auto* viewCombo = new QComboBox;
        viewCombo->addItems({"Home", "Folder Compare", "Text Compare", "Picture Compare", "3D Model Compare",
                             "Table Compare", "Registry Compare", "Hex Compare", "Media Compare"});
        viewCombo->setMinimumWidth(180);
        v->addWidget(viewCombo, 0, Qt::AlignLeft);

        auto* filter = new QLineEdit;
        filter->setPlaceholderText("Search commands");
        filter->setClearButtonEnabled(true);
        auto* head = new QHBoxLayout;
        head->addWidget(new QLabel("Commands:"));
        head->addStretch(1);
        head->addWidget(filter);
        v->addLayout(head);

        auto* table = new QTableWidget(0, 4);
        table->setHorizontalHeaderLabels({"Name", "Menu", "Toolbar", "Shortcut"});
        table->verticalHeader()->hide();
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->horizontalHeader()->setStretchLastSection(true);
        table->setColumnWidth(0, 200);
        table->setColumnWidth(1, 120);
        table->setColumnWidth(2, 80);
        table->setMinimumHeight(220);
        v->addWidget(table, 1);

        auto* description = note(QString());
        v->addWidget(description);
        v->addWidget(new QLabel("&Shortcut:"));
        auto* shortcut = new QKeySequenceEdit;
        shortcut->setEnabled(false);
        auto* remove = new QPushButton("Remove");
        auto* restore = new QPushButton("Default");
        auto* shortcutRow = new QHBoxLayout;
        shortcutRow->addWidget(shortcut, 1);
        shortcutRow->addWidget(remove);
        shortcutRow->addWidget(restore);
        v->addLayout(shortcutRow);
        remove->setEnabled(false);
        restore->setEnabled(false);
        v->addWidget(rule());
        v->addWidget(check("Large buttons with text labels (all views)", &draft_.largeToolbarButtonsWithText));

        // Rows keep the binding's index in UserRole so sorting/filtering never
        // loses track of which CommandBinding a row edits.
        auto bindings = [this, viewCombo]() -> QList<CommandBinding>& {
            auto& list = draft_.commandsByView[viewCombo->currentText()];
            if (list.isEmpty()) list = mockCommands(viewCombo->currentText());
            return list;
        };
        auto setShortcutCell = [table](int row, const QString& text) {
            table->item(row, 3)->setText(text);
        };
        auto selectedIndex = [table]() {
            const auto rows = table->selectionModel()->selectedRows();
            return rows.isEmpty() ? -1 : table->item(rows.first().row(), 0)->data(Qt::UserRole).toInt();
        };
        auto selectedRow = [table]() {
            const auto rows = table->selectionModel()->selectedRows();
            return rows.isEmpty() ? -1 : rows.first().row();
        };

        auto populate = [=]() {
            QSignalBlocker block(table);
            table->setSortingEnabled(false);
            table->setRowCount(0);
            const QString needle = filter->text().trimmed();
            const auto& list = bindings();
            for (int i = 0; i < list.size(); ++i) {
                const auto& b = list[i];
                if (!needle.isEmpty() && !b.name.contains(needle, Qt::CaseInsensitive)) continue;
                const int row = table->rowCount();
                table->insertRow(row);
                auto* name = new QTableWidgetItem(b.name);
                name->setData(Qt::UserRole, i);
                auto* menu = new QTableWidgetItem(b.menu);
                menu->setCheckState(b.inMenu ? Qt::Checked : Qt::Unchecked);
                auto* toolbar = new QTableWidgetItem;
                toolbar->setCheckState(b.inToolbar ? Qt::Checked : Qt::Unchecked);
                auto* key = new QTableWidgetItem(b.shortcut);
                for (auto* item : {name, menu, toolbar, key}) item->setFlags(item->flags() & ~Qt::ItemIsEditable);
                name->setFlags(name->flags() | Qt::ItemIsSelectable);
                menu->setFlags(menu->flags() | Qt::ItemIsUserCheckable);
                toolbar->setFlags(toolbar->flags() | Qt::ItemIsUserCheckable);
                table->setItem(row, 0, name);
                table->setItem(row, 1, menu);
                table->setItem(row, 2, toolbar);
                table->setItem(row, 3, key);
            }
            table->setSortingEnabled(true);
            table->sortByColumn(0, Qt::AscendingOrder);
            description->clear();
            shortcut->clear();
            shortcut->setEnabled(false);
            remove->setEnabled(false);
            restore->setEnabled(false);
        };

        connect(viewCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [populate](int) { populate(); });
        connect(filter, &QLineEdit::textChanged, this, [populate](const QString&) { populate(); });

        connect(table, &QTableWidget::itemSelectionChanged, this,
                [=]() {
                    const int index = selectedIndex();
                    const bool has = index >= 0;
                    shortcut->setEnabled(has);
                    remove->setEnabled(has);
                    restore->setEnabled(has);
                    if (!has) return;
                    const auto& b = bindings()[index];
                    description->setText(b.description);
                    QSignalBlocker block(shortcut);
                    shortcut->setKeySequence(QKeySequence(b.shortcut));
                });

        connect(table, &QTableWidget::itemChanged, this, [=](QTableWidgetItem* item) {
            if (item->column() != 1 && item->column() != 2) return;
            const int index = table->item(item->row(), 0)->data(Qt::UserRole).toInt();
            auto& b = bindings()[index];
            (item->column() == 1 ? b.inMenu : b.inToolbar) = item->checkState() == Qt::Checked;
            markDirty();
        });

        auto assign = [=](const QString& text) {
            const int index = selectedIndex();
            const int row = selectedRow();
            if (index < 0 || row < 0) return;
            bindings()[index].shortcut = text;
            QSignalBlocker block(table);
            setShortcutCell(row, text);
            QSignalBlocker blockEdit(shortcut);
            shortcut->setKeySequence(QKeySequence(text));
            markDirty();
        };
        connect(shortcut, &QKeySequenceEdit::editingFinished, this, [=]() {
            assign(shortcut->keySequence().toString(QKeySequence::PortableText));
        });
        connect(remove, &QPushButton::clicked, this, [assign]() { assign(QString()); });
        connect(restore, &QPushButton::clicked, this, [=]() {
            const int index = selectedIndex();
            if (index >= 0) assign(bindings()[index].defaultShortcut);
        });

        populate();
        return page;
    }

    // ---- Tweaks -----------------------------------------------------------
    QWidget* buildTweaks() {
        QVBoxLayout* v;
        auto* page = pageShell(&v);

        v->addWidget(heading("Check for Updates"));
        auto* updates = check("Check e&very:", &draft_.checkForUpdates);
        v->addWidget(updates);
        auto* row = new QHBoxLayout;
        row->setContentsMargins(22, 0, 0, 0);
        auto* days = spin(&draft_.updateIntervalDays, 1, 365);
        row->addWidget(days);
        row->addWidget(new QLabel("days"));
        row->addSpacing(24);
        row->addWidget(new QLabel("Last checked: " + draft_.lastUpdateCheck));
        row->addStretch(1);
        v->addLayout(row);
        days->setEnabled(draft_.checkForUpdates);
        connect(updates, &QCheckBox::toggled, days, &QWidget::setEnabled);

        v->addWidget(rule());
        v->addWidget(heading("Editor Display"));
        v->addWidget(check("Show syntax highlighting on difference lines", &draft_.syntaxHighlightOnDiffLines));
        v->addWidget(check("Use crosshatching for lines beyond the end of file", &draft_.crosshatchBeyondEndOfFile));
        v->addWidget(check("Right side gutter for left editor (compare only)", &draft_.rightGutterForLeftEditor));
        v->addWidget(check("Use orphan color", &draft_.useOrphanColor));
        v->addWidget(new QLabel("Extra line spacing:"));
        v->addLayout(withSuffix(spin(&draft_.extraLineSpacingPx, 0, 50), "pixels"));
        v->addWidget(new QLabel("Show column line at:"));
        v->addWidget(spin(&draft_.columnLineAt, 0, 999), 0, Qt::AlignLeft);
        v->addWidget(new QLabel("Dim inactive pane by:"));
        v->addLayout(withSuffix(spin(&draft_.dimInactivePanePercent, 0, 100), "%"));
        auto* mergeFont = check("Use different font for merge input panes:", &draft_.useDifferentMergeInputFont);
        v->addWidget(mergeFont);
        auto* font = fontButton(&draft_.mergeInputFont);
        font->setEnabled(draft_.useDifferentMergeInputFont);
        connect(mergeFont, &QCheckBox::toggled, font, &QWidget::setEnabled);
        v->addWidget(font, 0, Qt::AlignLeft);

        v->addWidget(rule());
        v->addWidget(heading("File Operations"));
        v->addWidget(check("Beep after long file operations", &draft_.beepAfterLongFileOperations));
        v->addStretch(1);
        return page;
    }

    static QHBoxLayout* withSuffix(QWidget* widget, const QString& text) {
        auto* row = new QHBoxLayout;
        row->addWidget(widget);
        row->addWidget(new QLabel(text));
        row->addStretch(1);
        return row;
    }

    // ---- Placeholder pages (no design yet) --------------------------------
    QWidget* buildPlaceholder(const QString& title) {
        QVBoxLayout* v;
        auto* page = pageShell(&v);
        v->addWidget(heading(title));
        v->addWidget(note("These settings are not available yet."));
        v->addStretch(1);
        return page;
    }

    OptionsStore& store_;
    AppOptions draft_;
    QLineEdit* search_ = nullptr;
    QListWidget* pages_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    QDialogButtonBox* buttons_ = nullptr;
    QPushButton* apply_ = nullptr;
    std::vector<QString> searchText_;
};

}  // namespace openbc::app