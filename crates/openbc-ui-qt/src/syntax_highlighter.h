// syntax_highlighter.h
// ---------------------------------------------------------------------------
// A compact, dependency-free QSyntaxHighlighter that lights up keywords,
// strings, numbers and comments for a handful of common languages, chosen by
// the file's extension (LanguageRules::forExtension). It is intentionally
// simple (regex-rule based, like Qt's own "syntaxhighlighter" example) rather
// than a full tokenizer/AST highlighter - that keeps it cheap to run on
// every keystroke of a big diff file, and easy to extend with another
// language by adding one more LanguageRules entry.
//
// TextCompareView attaches one highlighter per editor and re-selects the
// language whenever the file path changes (open, Swap, Reload).
// ---------------------------------------------------------------------------
#pragma once

#include <QColor>
#include <QFont>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QTextDocument>
#include <QVector>
#include <functional>
#include <vector>

#include "diff_algorithms.h"

namespace openbc::app {

// One highlighting rule: text matching `pattern` gets `format` applied.
struct HighlightRule {
    QRegularExpression pattern;
    QTextCharFormat format;
};

// A named bundle of rules plus the single- and multi-line comment forms
// (multi-line comments need special handling since they can span blocks,
// which a single per-line regex can't express).
struct LanguageRules {
    QString name;
    QStringList extensions;
    std::vector<HighlightRule> rules;
    QRegularExpression commentStart;   // e.g. "/\\*" - empty pattern = none
    QRegularExpression commentEnd;     // e.g. "\\*/"
    QTextCharFormat commentFormat;
};

namespace syntax {

inline QTextCharFormat makeFormat(const QColor& color, bool bold = false, bool italic = false) {
    QTextCharFormat format;
    format.setForeground(color);
    if (bold) format.setFontWeight(QFont::Bold);
    if (italic) format.setFontItalic(true);
    return format;
}

// Colours picked to read well against the app's dark (#1e1e1e-ish) editor
// background; they intentionally echo common dark-theme conventions
// (keywords orchid/blue, strings amber, comments muted green, numbers teal)
// so highlighted diffs still look like "a real code editor".
inline QTextCharFormat keywordFormat() { return makeFormat(QColor(0xc7, 0x92, 0xea), true); }
inline QTextCharFormat typeFormat() { return makeFormat(QColor(0x8b, 0xe9, 0xfd)); }
inline QTextCharFormat stringFormat() { return makeFormat(QColor(0xf1, 0xc4, 0x0f)); }
inline QTextCharFormat numberFormat() { return makeFormat(QColor(0x6f, 0xd9, 0xb5)); }
inline QTextCharFormat commentFormat() { return makeFormat(QColor(0x6a, 0x73, 0x8f), false, true); }
inline QTextCharFormat preprocessorFormat() { return makeFormat(QColor(0xff, 0xa6, 0x5c)); }

inline std::vector<HighlightRule> keywordRules(const QStringList& keywords,
                                               const QTextCharFormat& format) {
    std::vector<HighlightRule> rules;
    for (const QString& word : keywords) {
        rules.push_back({QRegularExpression("\\b" + word + "\\b"), format});
    }
    return rules;
}

inline void addCommonLiterals(std::vector<HighlightRule>& rules) {
    // Numbers (int/float/hex), double- and single-quoted strings.
    rules.push_back({QRegularExpression("\\b[0-9]+\\.?[0-9]*([eE][+-]?[0-9]+)?[fFlLuU]?\\b"),
                     numberFormat()});
    rules.push_back({QRegularExpression("\\b0[xX][0-9a-fA-F]+\\b"), numberFormat()});
    rules.push_back({QRegularExpression("\"(?:\\\\.|[^\"\\\\])*\""), stringFormat()});
    rules.push_back({QRegularExpression("'(?:\\\\.|[^'\\\\])*'"), stringFormat()});
}

inline LanguageRules cLikeRules(const QString& name, const QStringList& extensions,
                                const QStringList& keywords, const QStringList& types) {
    LanguageRules lang;
    lang.name = name;
    lang.extensions = extensions;
    lang.rules = keywordRules(keywords, keywordFormat());
    for (auto& rule : keywordRules(types, typeFormat())) lang.rules.push_back(rule);
    addCommonLiterals(lang.rules);
    lang.rules.push_back({QRegularExpression("^\\s*#\\s*\\w+"), preprocessorFormat()});
    lang.rules.push_back({QRegularExpression("//[^\n]*"), commentFormat()});
    lang.commentStart = QRegularExpression("/\\*");
    lang.commentEnd = QRegularExpression("\\*/");
    lang.commentFormat = commentFormat();
    return lang;
}

inline const std::vector<LanguageRules>& allLanguages() {
    static const std::vector<LanguageRules> languages = [] {
        std::vector<LanguageRules> list;

        list.push_back(cLikeRules(
            "C++", {"cpp", "cc", "cxx", "h", "hpp", "hxx", "c"},
            {"alignas", "alignof", "and", "asm", "auto", "bool", "break", "case", "catch",
             "class", "const", "constexpr", "continue", "decltype", "default", "delete", "do",
             "else", "enum", "explicit", "export", "extern", "false", "final", "for", "friend",
             "goto", "if", "inline", "mutable", "namespace", "new", "noexcept", "nullptr",
             "operator", "override", "private", "protected", "public", "return", "sizeof",
             "static", "struct", "switch", "template", "this", "throw", "true", "try",
             "typedef", "typename", "union", "using", "virtual", "void", "volatile", "while"},
            {"bool", "char", "double", "float", "int", "long", "short", "signed", "unsigned",
             "wchar_t", "size_t", "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t",
             "uint16_t", "uint32_t", "uint64_t", "QString", "QVector", "QList", "QMap"}));

        list.push_back(cLikeRules(
            "Java/C#", {"java", "cs"},
            {"abstract", "assert", "base", "break", "case", "catch", "checked", "class",
             "const", "continue", "default", "delegate", "do", "else", "enum", "event",
             "explicit", "extends", "extern", "final", "finally", "for", "foreach", "goto",
             "if", "implements", "implicit", "import", "in", "interface", "internal", "is",
             "lock", "namespace", "new", "null", "operator", "out", "override", "package",
             "params", "private", "protected", "public", "readonly", "ref", "return", "sealed",
             "static", "struct", "switch", "this", "throw", "throws", "true", "false", "try",
             "typeof", "unsafe", "using", "virtual", "void", "volatile", "while"},
            {"bool", "byte", "char", "decimal", "double", "float", "int", "long", "object",
             "sbyte", "short", "string", "uint", "ulong", "ushort", "var", "String", "Integer"}));

        list.push_back(cLikeRules(
            "JavaScript/TypeScript", {"js", "jsx", "ts", "tsx", "mjs", "cjs"},
            {"async", "await", "break", "case", "catch", "class", "const", "continue",
             "debugger", "default", "delete", "do", "else", "export", "extends", "false",
             "finally", "for", "from", "function", "if", "import", "in", "instanceof",
             "interface", "let", "new", "null", "of", "return", "static", "super", "switch",
             "this", "throw", "true", "try", "type", "typeof", "undefined", "var", "void",
             "while", "yield"},
            {"any", "boolean", "never", "number", "object", "string", "symbol", "unknown"}));

        LanguageRules python;
        python.name = "Python";
        python.extensions = {"py", "pyw", "pyi"};
        python.rules = keywordRules(
            {"and", "as", "assert", "async", "await", "break", "class", "continue", "def",
             "del", "elif", "else", "except", "False", "finally", "for", "from", "global",
             "if", "import", "in", "is", "lambda", "None", "nonlocal", "not", "or", "pass",
             "raise", "return", "True", "try", "while", "with", "yield", "self"},
            keywordFormat());
        addCommonLiterals(python.rules);
        python.rules.push_back({QRegularExpression("#[^\n]*"), commentFormat()});
        // Python's triple-quoted strings are the "multi-line comment"
        // analogue here - good enough for a diff viewer, not a full lexer.
        python.commentStart = QRegularExpression("\"\"\"");
        python.commentEnd = QRegularExpression("\"\"\"");
        python.commentFormat = stringFormat();
        list.push_back(python);

        LanguageRules xmlLike;
        xmlLike.name = "XML/HTML";
        xmlLike.extensions = {"xml", "html", "htm", "xhtml", "opml", "svg", "xsl", "xsd"};
        xmlLike.rules.push_back({QRegularExpression("</?[A-Za-z][-\\w:.]*"), keywordFormat()});
        xmlLike.rules.push_back({QRegularExpression("[-\\w:.]+(?==)"), typeFormat()});
        xmlLike.rules.push_back({QRegularExpression("\"[^\"]*\""), stringFormat()});
        xmlLike.rules.push_back({QRegularExpression("'[^']*'"), stringFormat()});
        xmlLike.commentStart = QRegularExpression("<!--");
        xmlLike.commentEnd = QRegularExpression("-->");
        xmlLike.commentFormat = commentFormat();
        list.push_back(xmlLike);

        LanguageRules json;
        json.name = "JSON";
        json.extensions = {"json"};
        json.rules.push_back({QRegularExpression("\"(?:\\\\.|[^\"\\\\])*\"\\s*(?=:)"),
                              typeFormat()});
        json.rules.push_back({QRegularExpression("\"(?:\\\\.|[^\"\\\\])*\""), stringFormat()});
        addCommonLiterals(json.rules);
        json.rules.push_back({QRegularExpression("\\b(true|false|null)\\b"), keywordFormat()});
        list.push_back(json);

        LanguageRules shell;
        shell.name = "Shell";
        shell.extensions = {"sh", "bash", "zsh"};
        shell.rules = keywordRules(
            {"case", "do", "done", "elif", "else", "esac", "fi", "for", "function", "if", "in",
             "return", "then", "until", "while"},
            keywordFormat());
        addCommonLiterals(shell.rules);
        shell.rules.push_back({QRegularExpression("\\$\\{?\\w+\\}?"), typeFormat()});
        shell.rules.push_back({QRegularExpression("#[^\n]*"), commentFormat()});
        list.push_back(shell);

        return list;
    }();
    return languages;
}

// Picks the rule set for a file's extension (case-insensitive), or nullptr
// for an unrecognised/no extension - the highlighter is then simply
// disabled, leaving the plain diff colouring untouched.
inline const LanguageRules* forExtension(const QString& extension) {
    const QString ext = extension.toLower();
    for (const auto& lang : allLanguages()) {
        if (lang.extensions.contains(ext)) return &lang;
    }
    return nullptr;
}

}  // namespace syntax

// ---------------------------------------------------------------------------
// The highlighter itself. Block-state is used to track "currently inside a
// multi-line comment" across blocks, the standard QSyntaxHighlighter idiom.
// ---------------------------------------------------------------------------
class SyntaxHighlighter : public QSyntaxHighlighter {
public:
    explicit SyntaxHighlighter(QTextDocument* document) : QSyntaxHighlighter(document) {}

    // Pass nullptr to turn highlighting off (falls back to plain text).
    void setLanguage(const LanguageRules* language) {
        if (language_ == language) return;
        language_ = language;
        rehighlight();
    }

    const LanguageRules* language() const { return language_; }

    // Supplies, for a given block (row) number, the char-level diff segments
    // TextCompareView computed for that row (see computeInlineDiff in
    // diff_algorithms.h). Qt only allows one QSyntaxHighlighter per
    // document, so the red "text differs" colouring and the whitespace-
    // mismatch background tint are applied here, layered on top of whatever
    // language rule coloured that span - rather than via a second,
    // independent highlighter, which Qt does not support cleanly.
    void setInlineDiffProvider(std::function<QVector<CharSegment>(int)> provider) {
        inlineDiffProvider_ = std::move(provider);
    }

protected:
    void highlightBlock(const QString& text) override {
        if (!language_) {
            setCurrentBlockState(0);
        } else {
        for (const auto& rule : language_->rules) {
            auto it = rule.pattern.globalMatch(text);
            while (it.hasNext()) {
                const auto match = it.next();
                setFormat(match.capturedStart(), match.capturedLength(), rule.format);
            }
        }

        // Multi-line comment/string spans (block state 1 = "still inside").
        if (language_->commentStart.pattern().isEmpty()) {
            setCurrentBlockState(0);
        } else {
        int startIndex = previousBlockState() == 1
                             ? 0
                             : text.indexOf(language_->commentStart);
        while (startIndex >= 0) {
            const auto endMatch = language_->commentEnd.match(text, startIndex);
            int length;
            if (!endMatch.hasMatch()) {
                setCurrentBlockState(1);
                length = text.length() - startIndex;
            } else {
                length = endMatch.capturedEnd() - startIndex;
            }
            setFormat(startIndex, length, language_->commentFormat);
            if (!endMatch.hasMatch()) break;
            startIndex = text.indexOf(language_->commentStart, startIndex + length);
        }
        if (startIndex < 0 && previousBlockState() != 1) {
            setCurrentBlockState(0);
        }
        }
        }

        // Diff overlay: applied last so it wins over language colouring on
        // whichever span actually differs from the counterpart line.
        if (!inlineDiffProvider_) return;
        const QVector<CharSegment> segments = inlineDiffProvider_(currentBlock().blockNumber());
        for (const CharSegment& segment : segments) {
            if (segment.kind == CharSegmentKind::Equal) continue;
            QTextCharFormat format;
            if (segment.kind == CharSegmentKind::Mismatch) {
                format.setForeground(QColor(0xff, 0x6b, 0x6b));
                format.setFontWeight(QFont::Bold);
            } else {  // WhitespaceMismatch
                format.setBackground(QColor(0x3a, 0x4a, 0x63));
                format.setForeground(QColor(0x9d, 0xb4, 0xd1));
            }
            setFormat(segment.start, segment.length, format);
        }
    }

private:
    const LanguageRules* language_ = nullptr;
    std::function<QVector<CharSegment>(int)> inlineDiffProvider_;
};

}  // namespace openbc::app
