#!/usr/bin/env python3
"""
i18n_migrate.py - move hard-coded UI strings to openbc::i18n::t("key").

Run from the repository root:

    python3 tools/i18n_migrate.py --dry-run     # report only, touch nothing
    python3 tools/i18n_migrate.py               # rewrite sources + regenerate catalogs

What it does
  * finds string literals passed to UI calls (menus, labels, buttons, tooltips, message boxes, tr(), the
    options-dialog helpers check()/radio()/heading()/note(), ...) in crates/openbc-ui-qt/cpp/**
  * "text" + expr + "more"   ->  t("key", expr)   with text "text%1more"
    cond ? "A" : "B"         ->  cond ? t("a") : t("b")
    tr("x")                  ->  t("key")
  * assigns stable keys (<area>.<slug>, or common.<slug> when several areas use the same text); texts
    that already exist in resources/i18n/messages.json keep their key
  * adds  #include "i18n/i18n.h"  to every touched file
  * regenerates resources/i18n/messages.json and cpp/i18n/messages_defaults.h from the key registry
  * writes i18n_migration_report.txt (things that need a human) and i18n_leftovers.txt (literals that
    look like UI text but were not in a recognised call)

It is idempotent: a second run finds nothing left to migrate and keeps every existing key.

Deliberately NOT migrated (listed in the report): addItem()/addItems() literals, because the options
dialog persists combo-box *text* in preferences.ini - translating it would corrupt saved settings.
"""
import argparse
import json
import re
import sys
from collections import OrderedDict, defaultdict
from pathlib import Path

# ---- which calls carry user-visible text, and in which argument positions -------------------------------
FUNC_ARGS = {
    "tr": [0],
    "addMenu": [0], "addAction": [0], "setText": [0], "setWindowTitle": [0], "setToolTip": [0],
    "setPlaceholderText": [0], "setTitle": [0], "setStatusTip": [0], "setWhatsThis": [0],
    "setIconText": [0], "setBadge": [0], "setStatus": [0], "showMessage": [0], "log": [0],
    "setTabText": [1], "addTab": [1], "addRow": [0],
    # options dialog / viewer helper lambdas
    "heading": [0], "check": [0], "radio": [0], "note": [0], "openIn": [0],
    "make": [0], "mk": [0, 1, 3], "addCmd": [0, 2],
    # widget constructors
    "QLabel": [0], "QPushButton": [0], "QCheckBox": [0], "QRadioButton": [0], "QGroupBox": [0],
    "QAction": [0], "QMenu": [0], "QToolButton": [0],
}
QUALIFIED = {
    "QMessageBox": {n: [1, 2] for n in ("information", "warning", "critical", "question", "about")},
    "QFileDialog": {n: [1] for n in ("getOpenFileName", "getOpenFileNames", "getSaveFileName", "getExistingDirectory")},
    "QInputDialog": {n: [1, 2] for n in ("getText", "getInt", "getItem", "getDouble", "getMultiLineText")},
}
HEADER_LIST_FUNCS = {"setHeaderLabels"}            # setHeaderLabels({"A", "B"})
DATA_BEARING = {"addItem", "addItems"}             # reported, never rewritten
WRAPPERS = ("QStringLiteral", "QLatin1String", "QLatin1StringView", "QString::fromUtf8", "QString::fromLatin1", "QString")

HEAD_RE = re.compile(r"(?<![\w])(?:(?P<q>Q[A-Za-z]+)::)?(?P<name>[A-Za-z_]\w*)\s*\(")
STR_RE = re.compile(r'"(?:[^"\\\n]|\\.)*"')
INCLUDE_LINE = '#include "i18n/i18n.h"'
STOP_AT = re.compile(r"^\s*(namespace|class|struct|inline|template|enum|static|constexpr|using)\b")


# ---- lexer: which characters are real code (not comment / string / char literal) -------------------------
def code_mask(t):
    n = len(t)
    m = bytearray(b"\x01") * n
    i = 0
    while i < n:
        c = t[i]
        two = t[i:i + 2]
        if two == "//":
            j = t.find("\n", i)
            j = n if j < 0 else j
            m[i:j] = b"\x00" * (j - i)
            i = j
        elif two == "/*":
            j = t.find("*/", i + 2)
            j = n if j < 0 else j + 2
            m[i:j] = b"\x00" * (j - i)
            i = j
        elif c == '"':
            # raw string?  R"delim( ... )delim"
            if i > 0 and t[i - 1] == "R" and (i < 2 or not (t[i - 2].isalnum() or t[i - 2] == "_") or t[i - 2:i] in ("u8", "uR")):
                k = t.find("(", i)
                delim = t[i + 1:k]
                end = t.find(")" + delim + '"', k)
                j = n if end < 0 else end + len(delim) + 2
            else:
                j = i + 1
                while j < n and t[j] != '"':
                    j += 2 if t[j] == "\\" else 1
                    if j < n and t[j] == "\n":
                        break
                j = min(j + 1, n)
            m[i:j] = b"\x00" * (j - i)
            i = j
        elif c == "'" and not (i > 0 and (t[i - 1].isalnum() or t[i - 1] == "_")):
            j = i + 1
            while j < n and t[j] != "'":
                j += 2 if t[j] == "\\" else 1
                if j < n and t[j] == "\n":
                    break
            j = min(j + 1, n)
            m[i:j] = b"\x00" * (j - i)
            i = j
        else:
            i += 1
    return m


def match_paren(t, m, open_idx):
    """Index of the ')' closing the '(' at open_idx, plus argument spans (start, end) at depth 1."""
    depth = 0
    spans = []
    start = open_idx + 1
    i = open_idx
    n = len(t)
    while i < n:
        if m[i]:
            c = t[i]
            if c in "([{":
                depth += 1
            elif c in ")]}":
                depth -= 1
                if depth == 0:
                    spans.append((start, i))
                    return i, spans
            elif c == "," and depth == 1:
                spans.append((start, i))
                start = i + 1
        i += 1
    return -1, spans


def strip_span(t, s, e):
    while s < e and t[s].isspace():
        s += 1
    while e > s and t[e - 1].isspace():
        e -= 1
    return s, e


def top_level_positions(t, m, s, e, chars):
    """Positions of single chars from `chars` at bracket depth 0 inside [s, e)."""
    depth = 0
    out = []
    for i in range(s, e):
        if not m[i]:
            continue
        c = t[i]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif depth == 0 and c in chars:
            out.append(i)
    return out


# ---- literal decoding ------------------------------------------------------------------------------------
SIMPLE_ESC = {"n": "\n", "t": "\t", "r": "\r", "\\": "\\", '"': '"', "'": "'", "0": "\0", "?": "?"}


def decode_literal(tok):
    body = tok[1:-1]
    out = []
    i = 0
    while i < len(body):
        c = body[i]
        if c != "\\":
            out.append(c)
            i += 1
            continue
        nxt = body[i + 1] if i + 1 < len(body) else ""
        if nxt == "u" and re.fullmatch(r"[0-9a-fA-F]{4}", body[i + 2:i + 6]):
            out.append(chr(int(body[i + 2:i + 6], 16)))
            i += 6
        elif nxt in SIMPLE_ESC and not (nxt == "0" and body[i + 2:i + 3].isdigit()):
            out.append(SIMPLE_ESC[nxt])
            i += 2
        else:
            return None   # \x.., octal, \U...: leave for a human
    return "".join(out)


def parse_literal_expr(t, s, e):
    """Whole span is "lit" / "a" "b" / QStringLiteral("lit") ...  ->  decoded text, else None."""
    s, e = strip_span(t, s, e)
    seg = t[s:e]
    for w in WRAPPERS:
        if seg.startswith(w) and seg[len(w):].lstrip().startswith("(") and seg.endswith(")"):
            seg = seg[len(w):].lstrip()[1:-1]
            break
    seg = seg.strip()
    if not seg or not re.fullmatch(r'(?:\s*"(?:[^"\\\n]|\\.)*"\s*)+', seg):
        return None
    parts = []
    for tok in STR_RE.findall(seg):
        d = decode_literal(tok)
        if d is None:
            return None
        parts.append(d)
    return "".join(parts)


# ---- policy ------------------------------------------------------------------------------------------------
def is_ui_text(s, explicit):
    if not any(ch.isalpha() for ch in s):
        return False
    if s.startswith(":/") or ("{" in s and ";" in s) or s.lstrip().startswith("<"):
        return False
    if not explicit and re.fullmatch(r"[a-z0-9_./\-:*]+", s):
        return False   # looks like an identifier / path / glob, not a sentence
    return True


def slug(text):
    words = re.findall(r"[a-z0-9]+", re.sub(r"%\d+", " ", text.replace("&", "")).lower())[:6]
    s = "_".join(words)[:48].strip("_")
    return s or "text"


def area_of(rel):
    p = rel.parts
    if p[0] == "viewers":
        return p[1]
    return p[0] if len(p) > 1 else "app"


# ---- scanning ----------------------------------------------------------------------------------------------
class Edit:
    __slots__ = ("start", "end", "kind", "text", "args", "line")

    def __init__(self, start, end, kind, text, args, line):
        self.start, self.end, self.kind, self.text, self.args, self.line = start, end, kind, text, args, line


def scan_file(t, report, rel):
    m = code_mask(t)
    edits, seen = [], set()

    def line_of(pos):
        return t.count("\n", 0, pos) + 1

    def add(start, end, text, args, explicit, func):
        if (start, end) in seen:
            return
        if not is_ui_text(text, explicit):
            report["skipped"].append((rel, line_of(start), func, text))
            return
        seen.add((start, end))
        edits.append(Edit(start, end, "t", text, args, line_of(start)))

    def unhandled(s, seg, func):
        if re.fullmatch(r'QString::fromUtf8\("(\\x[0-9A-Fa-f]{2})+"\)', seg):
            return   # a glyph such as a multiplication sign, not text
        report["unhandled"].append((rel, line_of(s), func, seg))

    def arg_chain(s, e):
        """QString("lit %1 %2").arg(a).arg(b, c)  ->  (text, [a, b, c]) or None."""
        seg = t[s:e]
        mm = re.match(r"QString\s*\(", seg)
        if not mm:
            return None
        close, spans = match_paren(t, m, s + mm.end() - 1)
        if close < 0 or len(spans) != 1:
            return None
        lit = parse_literal_expr(t, s, close + 1)
        if lit is None:
            return None
        args, pos = [], close + 1
        while True:
            am = re.compile(r"\s*\.\s*arg\s*\(").match(t, pos, e)
            if not am:
                break
            c2, sp = match_paren(t, m, am.end() - 1)
            if c2 < 0:
                return None
            parts = [" ".join(t[x:y].split()) for x, y in sp]
            if len(parts) >= 3 and parts[1] == "0" and re.fullmatch(r"'[fgeE]'", parts[2]) and len(parts) <= 4:
                prec = parts[3] if len(parts) == 4 else "6"   # arg(v, 0, 'f', p) == QString::number(v, 'f', p)
                parts = ["QString::number(%s, %s, %s)" % (parts[0], parts[2], prec)]
            if any(re.fullmatch(r"-?\d+(\.\d+)?|'.'|'\\\\?.'", x) for x in parts[1:]):
                return None   # width / fill / base arguments: keep QString::arg
            args += parts
            pos = c2 + 1
        if pos != e or not args:
            return None
        return lit, args

    def process(s, e, func, explicit):
        """Handle one argument. True = migrated or intentionally nothing to do; False = needs a human."""
        s, e = strip_span(t, s, e)
        if s >= e:
            return True
        seg = t[s:e]
        if seg.startswith("(") and match_paren(t, m, s)[0] == e - 1:
            return process(s + 1, e - 1, func, explicit)          # ( ... ) around the whole argument
        if seg.startswith("[") or re.match(r"(?:\w+::)?tr\s*\(", seg) or seg.startswith("i18n::") \
                or seg.startswith("::openbc::i18n"):
            return True   # lambda, or tr() which has its own head / already migrated
        lit = parse_literal_expr(t, s, e)
        if lit is not None:
            add(s, e, lit, [], explicit, func)
            return True
        chain = arg_chain(s, e)
        if chain is not None:
            add(s, e, chain[0], chain[1], explicit, func)
            report["concat"].append((rel, line_of(s), func, seg))
            return True
        if not any(m[i] == 0 and t[i] == '"' for i in range(s, e)):
            return True   # no literal in this argument at all
        q = top_level_positions(t, m, s, e, "?")
        if q:   # cond ? A : B   (each branch handled on its own)
            colon = [p for p in top_level_positions(t, m, q[0] + 1, e, ":")
                     if t[p - 1] != ":" and t[p + 1:p + 2] != ":"]
            if colon:
                ok1 = process(q[0] + 1, colon[0], func, explicit)
                ok2 = process(colon[0] + 1, e, func, explicit)
                if ok1 and ok2:
                    return True
                return False
            unhandled(s, seg, func)
            return False
        plus = [p for p in top_level_positions(t, m, s, e, "+")
                if t[p - 1:p] != "+" and t[p + 1:p + 2] not in ("+", "=")]
        if plus:
            bounds, prev = [], s
            for p in plus:
                bounds.append((prev, p))
                prev = p + 1
            bounds.append((prev, e))
            template, args, nlit = "", [], 0
            for (x, y) in bounds:
                x, y = strip_span(t, x, y)
                lit = parse_literal_expr(t, x, y)
                if lit is not None:
                    if re.search(r"%\d", lit) or (args and template.endswith("%" + str(len(args))) and lit[:1].isdigit()):
                        unhandled(s, seg, func)   # would clash with / glue onto a placeholder
                        return False
                    template += lit
                    nlit += 1
                else:
                    if re.search(r"(<<|&&|\|\||==|!=)", t[x:y]) and not re.match(r"[\w:]+\(", t[x:y]):
                        unhandled(s, seg, func)
                        return False
                    args.append(" ".join(t[x:y].split()))
                    template += "%%%d" % len(args)
            if nlit and args and len(args) <= 9 and is_ui_text(template.replace("%", ""), explicit):
                add(s, e, template, args, explicit, func)
                report["concat"].append((rel, line_of(s), func, seg))
                return True
            if nlit and any(ch.isalpha() for ch in template):
                unhandled(s, seg, func)
                return False
            return True
        unhandled(s, seg, func)
        return False

    for hm in HEAD_RE.finditer(t):
        if not m[hm.start("name")]:
            continue
        name, q = hm.group("name"), hm.group("q")
        open_idx = hm.end() - 1
        indexes, explicit = None, False
        if q in QUALIFIED and name in QUALIFIED[q]:
            indexes = QUALIFIED[q][name]
        elif name in FUNC_ARGS:
            indexes = FUNC_ARGS[name]
            explicit = name == "tr"
        elif name in HEADER_LIST_FUNCS or name in DATA_BEARING:
            pass
        else:
            continue
        close, spans = match_paren(t, m, open_idx)
        if close < 0:
            continue

        if name == "tr" and indexes:
            if len(spans) != 1:
                report["unhandled"].append((rel, line_of(open_idx), "tr", t[open_idx:close + 1]))
                continue
            s, e = strip_span(t, *spans[0])
            lit = parse_literal_expr(t, s, e)
            if lit is None:
                report["unhandled"].append((rel, line_of(s), "tr", t[s:e]))
                continue
            start = hm.start("q") if q else hm.start("name")
            pre = re.search(r"(\bthis->|\b\w+(?:->|\.))$", t[:start])
            if pre:
                start -= len(pre.group(1))
            add(start, close + 1, lit, [], True, "tr")
            continue

        if name in DATA_BEARING:
            for (s, e) in spans[:1]:
                if '"' in t[s:e]:
                    report["data_bearing"].append((rel, line_of(s), name, " ".join(t[s:e].split())[:110]))
            continue

        if name in HEADER_LIST_FUNCS:
            s, e = strip_span(t, *spans[0]) if spans else (0, 0)
            if t[s:s + 1] == "{":
                for lm in STR_RE.finditer(t, s, e):
                    if m[lm.start()] == 0:
                        d = decode_literal(lm.group(0))
                        if d is not None:
                            add(lm.start(), lm.end(), d, [], True, name)
            continue

        if name == "addTab" and len(spans) >= 2:
            indexes = [len(spans) - 1]            # addTab(w, text) / addTab(w, icon, text)
        elif name in ("addAction", "addMenu", "QAction", "QPushButton", "QToolButton") and spans \
                and re.search(r"[Ii]con", t[spans[0][0]:spans[0][1]]) and len(spans) > 1:
            indexes = [1]                         # (icon, text, ...) overloads
        for idx in indexes:
            if idx < len(spans):
                process(spans[idx][0], spans[idx][1], name, explicit)

    # drop overlaps (keep the earlier, outer-most)
    edits.sort(key=lambda x: (x.start, -x.end))
    out, last = [], -1
    for ed in edits:
        if ed.start >= last:
            out.append(ed)
            last = ed.end
    return out, m


def leftovers(t, m, edits, rel):
    covered = [(e.start, e.end) for e in edits]
    found = []
    for lm in STR_RE.finditer(t):
        if m[lm.start()]:
            continue
        if any(a <= lm.start() < b for a, b in covered):
            continue
        d = decode_literal(lm.group(0))
        if d is None or len(d) < 6 or " " not in d or not d[0].isupper() or "{" in d or ";" in d or "<" in d:
            continue
        if len(re.findall(r"[A-Za-z]{2,}", d)) < 2:
            continue
        ls = t.rfind("\n", 0, lm.start()) + 1
        line = t[ls:t.find("\n", lm.start())]
        if re.search(r"setObjectName|setProperty|qWarning|qDebug|qCritical|openbc_log|beginGroup|Q_ASSERT|QStringLiteral\(\"[A-Z][a-z]+/", line):
            continue
        found.append((rel, t.count("\n", 0, lm.start()) + 1, d))
    return found


# ---- registry / output -------------------------------------------------------------------------------------
def cpp_escape(s):
    out = []
    for ch in s:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\t":
            out.append("\\t")
        elif ch == "\r":
            out.append("\\r")
        elif ch == "?":
            out.append("?")
        else:
            out.append(ch)
    return "".join(out).replace("??", "?\"\"?")


def write_defaults_header(path, registry):
    groups = OrderedDict()
    for k in sorted(registry):
        groups.setdefault(k.split(".", 1)[0], []).append(k)
    lines = [
        "// ---------------------------------------------------------------------------",
        "// Built-in message table = the safety net of the i18n system (see i18n.h).",
        "//",
        "// GENERATED by tools/i18n_migrate.py from resources/i18n/messages.json - keep the two in sync by",
        "// editing messages.json (or adding keys through the tool) and re-running the tool, or by",
        "//     cpp/tools/i18n_test/build_and_run.sh --dump > resources/i18n/messages.json",
        "// after editing this table by hand.",
        "//",
        "// If a user deletes keys from (or breaks) messages*.json the app still shows these texts;",
        "// a key that is in neither is shown as the key itself.",
        "// ---------------------------------------------------------------------------",
        "#pragma once",
        "",
        "#include <cstddef>",
        "#include <iterator>",
        "",
        "namespace openbc::i18n {",
        "",
        "struct BuiltinMessage {",
        "    const char* key;",
        "    const char* text;   // UTF-8; %1 %2 ... are placeholders filled by t(key, args...)",
        "};",
        "",
        "inline constexpr BuiltinMessage kBuiltinMessages[] = {",
    ]
    for g, keys in groups.items():
        lines.append("    // ---- %s" % g)
        for k in keys:
            lines.append('    {"%s", "%s"},' % (k, cpp_escape(registry[k])))
    lines += ["};", "", "inline constexpr std::size_t kBuiltinMessageCount = std::size(kBuiltinMessages);", "",
              "}  // namespace openbc::i18n", ""]
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cpp-dir", default="crates/openbc-ui-qt/cpp")
    ap.add_argument("--res-dir", default="resources/i18n")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--report-dir", default=".")
    a = ap.parse_args()
    cpp, res = Path(a.cpp_dir), Path(a.res_dir)
    if not cpp.is_dir():
        sys.exit("cpp dir not found: %s (run from the repo root or pass --cpp-dir)" % cpp)

    registry = {}
    mj = res / "messages.json"
    if mj.exists():
        registry = json.loads(mj.read_text(encoding="utf-8").lstrip("\ufeff"))
    reverse = {v: k for k, v in registry.items()}

    report = defaultdict(list)
    per_file, areas_of_text, order = {}, defaultdict(set), []
    for path in sorted(list(cpp.rglob("*.h")) + list(cpp.rglob("*.cpp"))):
        rel = path.relative_to(cpp)
        if rel.parts[0] in ("tools", "i18n"):
            continue
        try:
            with open(path, encoding="utf-8", newline="") as fh:
                t = fh.read()
        except UnicodeDecodeError:
            report["unhandled"].append((rel, 0, "file", "not valid UTF-8, skipped"))
            continue
        edits, m = scan_file(t, report, rel)
        report["leftover"] += leftovers(t, m, edits, rel)
        if edits:
            per_file[path] = (t, edits)
            for e in edits:
                if e.text not in areas_of_text:
                    order.append(e.text)
                areas_of_text[e.text].add(area_of(rel))

    # global key assignment
    used = set(registry)
    key_of = dict(reverse)
    for text in order:
        if text in key_of:
            continue
        areas = areas_of_text[text]
        prefix = "common" if len(areas) > 1 else next(iter(areas))
        base = "%s.%s" % (prefix, slug(text))
        key, n = base, 2
        while key in used:
            key = "%s_%d" % (base, n)
            n += 1
        used.add(key)
        key_of[text] = key
        registry[key] = text

    changed = 0
    for path, (t, edits) in per_file.items():
        out, pos = [], 0
        nl = "\r\n" if "\r\n" in t else "\n"
        for e in edits:
            out.append(t[pos:e.start])
            call = '::openbc::i18n::t("%s"' % key_of[e.text]
            if e.args:
                call += ", " + ", ".join(" ".join(x.split()) for x in e.args)
            out.append(call + ")")
            pos = e.end
        out.append(t[pos:])
        new = "".join(out)
        if INCLUDE_LINE not in new:
            lines = new.split(nl)
            last = -1
            for i, ln in enumerate(lines[:400]):
                if ln.startswith("#include"):
                    last = i
                elif STOP_AT.match(ln) and last >= 0:
                    break
            lines.insert(last + 1 if last >= 0 else 0, INCLUDE_LINE)
            new = nl.join(lines)
        changed += 1
        if not a.dry_run:
            with open(path, "w", encoding="utf-8", newline="") as fh:
                fh.write(new)

    total = sum(len(e) for _, e in per_file.values())
    print("files changed: %d   literals migrated: %d   keys in catalog: %d" % (changed, total, len(registry)))
    if not a.dry_run:
        res.mkdir(parents=True, exist_ok=True)
        mj.write_text(json.dumps(dict(sorted(registry.items())), ensure_ascii=False, indent=4) + "\n", encoding="utf-8")
        write_defaults_header(cpp / "i18n" / "messages_defaults.h", registry)

    rd = Path(a.report_dir)
    with open(rd / "i18n_migration_report.txt", "w", encoding="utf-8") as f:
        f.write("# Migrated: %d literals in %d files, %d catalog keys\n\n" % (total, changed, len(registry)))
        for title, key, hint in (
            ("NEEDS A HUMAN: UI call whose text is not a plain literal (variables, mixed expressions)", "unhandled", ""),
            ("DATA-BEARING combo items (left untouched: text is persisted/compared; give items itemData ids, then migrate)", "data_bearing", ""),
            ("Converted from concatenation to placeholders (review that every operand is QString/number/char*/std::string)", "concat", ""),
            ("Skipped as not UI text (identifier/path/no letters) - check none are real labels", "skipped", ""),
        ):
            f.write("## %s  [%d]\n" % (title, len(report[key])))
            for rel, line, func, src in report[key]:
                f.write("%s:%d  %s(%s)\n" % (rel, line, func, " ".join(str(src).split())[:140]))
            f.write("\n")
    with open(rd / "i18n_leftovers.txt", "w", encoding="utf-8") as f:
        f.write("# String literals that look like UI text but were not in a recognised call [%d]\n" % len(report["leftover"]))
        for rel, line, d in report["leftover"]:
            f.write("%s:%d  %s\n" % (rel, line, d[:140].replace("\n", "\\n")))
    for key in ("unhandled", "data_bearing", "concat", "skipped", "leftover"):
        print("%-13s %d" % (key, len(report[key])))


if __name__ == "__main__":
    main()
