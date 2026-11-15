//! Display-only demangling of C++ symbol names, with no external dependency.
//!
//! Matching between versions always uses the raw linkage name (the identity of an element); the
//! demangled text is only what the viewer shows, so turning demangling on or off never changes
//! which elements are classified as added, removed or modified.
//!
//! Supported:
//! * Itanium ABI (`_Z...`, Mach-O `__Z...`): nested and `std::` names, constructors and
//!   destructors, operators, templates and their arguments (types, integer / bool literals),
//!   substitutions (`S_`, `S0_`, `Sa`, `Ss`, ...), builtin / pointer / reference / cv types,
//!   arrays, `T_` template parameters, vtable / typeinfo / guard-variable special names and GCC
//!   clone suffixes (`.part.0`). Function-pointer types, member pointers, local names, lambdas and
//!   expressions are not decoded: such a name is shown raw.
//! * Microsoft Visual C++ (`?name@scope@@...`): reduced to the qualified name; parameters are not
//!   decoded.
//!
//! Anything that cannot be parsed returns `None` and is displayed as is.

use std::panic::{catch_unwind, AssertUnwindSafe};

const MAX_DEPTH: usize = 64;
const MAX_LEN: usize = 8192;

/// Demangled form of `name`, or `None` when `name` is not a mangled C++ name or cannot be decoded.
///
/// An ELF symbol-version suffix (`@@GLIBCXX_3.4`) is kept after the demangled text.
#[must_use]
pub fn demangle(name: &str) -> Option<String> {
    // Runs on untrusted symbol tables: a bug here must never take the worker down.
    catch_unwind(AssertUnwindSafe(|| demangle_inner(name))).ok().flatten()
}

fn demangle_inner(name: &str) -> Option<String> {
    if name.starts_with('?') {
        return demangle_msvc(name);
    }
    let itanium = if name.starts_with("__Z") { &name[1..] } else { name };
    let rest = itanium.strip_prefix("_Z")?;
    let (base, version) = match rest.find('@') {
        Some(at) => (&rest[..at], &rest[at..]),
        None => (rest, ""),
    };
    let mut parser = Parser { s: base.as_bytes(), i: 0, subs: Vec::new(), tpl: Vec::new(), depth: 0 };
    let text = parser.encoding()?;
    if text.is_empty() || text.len() > MAX_LEN {
        return None;
    }
    Some(format!("{text}{version}"))
}

/// Label for `key` (a raw linkage name, possibly with a `#2` duplicate suffix added by the
/// aligner): the demangled text plus the suffix, or `key` itself when it cannot be demangled.
#[must_use]
pub fn display_name(key: &str, raw_name: &str, enabled: bool) -> String {
    if !enabled {
        return key.to_owned();
    }
    match demangle(raw_name) {
        Some(text) => format!("{text}{}", key.strip_prefix(raw_name).unwrap_or("")),
        None => key.to_owned(),
    }
}

// ----------------------------------------------------------------------------- MSVC

fn demangle_msvc(name: &str) -> Option<String> {
    let body = name.strip_prefix('?')?;
    // `??0Class@@...` constructor, `??1Class@@...` destructor; other `??` names are not decoded.
    let (special, body) = match body.strip_prefix('?') {
        Some(rest) => {
            let mut chars = rest.chars();
            let code = chars.next()?;
            (Some(code), chars.as_str())
        }
        None => (None, body),
    };
    let end = body.find("@@")?;
    let parts: Vec<&str> = body[..end].split('@').collect();
    if parts.iter().any(|p| p.is_empty() || p.starts_with('$') || p.starts_with('?')) {
        return None;
    }
    let (first, scopes) = parts.split_first()?;
    let mut path: Vec<String> = scopes.iter().rev().map(|s| (*s).to_owned()).collect();
    match special {
        None => path.push((*first).to_owned()),
        Some('0') => {
            path.push((*first).to_owned());
            path.push((*first).to_owned());
        }
        Some('1') => {
            path.push((*first).to_owned());
            path.push(format!("~{first}"));
        }
        Some(_) => return None,
    }
    Some(path.join("::"))
}

// ----------------------------------------------------------------------------- Itanium

/// A parsed `<name>`.
struct Name {
    text: String,
    /// The last component carries template arguments (a template function has a return type).
    templated: bool,
    /// Constructor, destructor or conversion operator (no return type even when templated).
    special: bool,
    /// Member-function qualifiers (`const`, `&`).
    quals: String,
}

struct Parser<'a> {
    s: &'a [u8],
    i: usize,
    subs: Vec<String>,
    /// Template arguments of the function name, for `T_` in the parameter list.
    tpl: Vec<String>,
    depth: usize,
}

fn builtin(c: u8) -> Option<&'static str> {
    Some(match c {
        b'v' => "void",
        b'w' => "wchar_t",
        b'b' => "bool",
        b'c' => "char",
        b'a' => "signed char",
        b'h' => "unsigned char",
        b's' => "short",
        b't' => "unsigned short",
        b'i' => "int",
        b'j' => "unsigned int",
        b'l' => "long",
        b'm' => "unsigned long",
        b'x' => "long long",
        b'y' => "unsigned long long",
        b'n' => "__int128",
        b'o' => "unsigned __int128",
        b'f' => "float",
        b'd' => "double",
        b'e' => "long double",
        b'g' => "__float128",
        b'z' => "...",
        _ => return None,
    })
}

fn operator(code: &[u8]) -> Option<&'static str> {
    Some(match code {
        b"nw" => "new",
        b"na" => "new[]",
        b"dl" => "delete",
        b"da" => "delete[]",
        b"ps" | b"pl" => "+",
        b"ng" | b"mi" => "-",
        b"ad" | b"an" => "&",
        b"de" | b"ml" => "*",
        b"co" => "~",
        b"dv" => "/",
        b"rm" => "%",
        b"or" => "|",
        b"eo" => "^",
        b"aS" => "=",
        b"pL" => "+=",
        b"mI" => "-=",
        b"mL" => "*=",
        b"dV" => "/=",
        b"rM" => "%=",
        b"aN" => "&=",
        b"oR" => "|=",
        b"eO" => "^=",
        b"ls" => "<<",
        b"rs" => ">>",
        b"lS" => "<<=",
        b"rS" => ">>=",
        b"eq" => "==",
        b"ne" => "!=",
        b"lt" => "<",
        b"gt" => ">",
        b"le" => "<=",
        b"ge" => ">=",
        b"ss" => "<=>",
        b"nt" => "!",
        b"aa" => "&&",
        b"oo" => "||",
        b"pp" => "++",
        b"mm" => "--",
        b"cm" => ",",
        b"pm" => "->*",
        b"pt" => "->",
        b"cl" => "()",
        b"ix" => "[]",
        _ => return None,
    })
}

fn std_abbreviation(c: u8) -> Option<&'static str> {
    Some(match c {
        b't' => "std",
        b'a' => "std::allocator",
        b'b' => "std::basic_string",
        b's' => "std::string",
        b'i' => "std::istream",
        b'o' => "std::ostream",
        b'd' => "std::iostream",
        _ => return None,
    })
}

fn join_template(args: &[String]) -> String {
    let mut text = String::from("<");
    text.push_str(&args.join(", "));
    if text.ends_with('>') {
        text.push(' ');
    }
    text.push('>');
    text
}

impl Parser<'_> {
    fn peek(&self) -> Option<u8> {
        self.s.get(self.i).copied()
    }

    fn peek_at(&self, offset: usize) -> Option<u8> {
        self.s.get(self.i + offset).copied()
    }

    fn next(&mut self) -> Option<u8> {
        let c = self.peek()?;
        self.i += 1;
        Some(c)
    }

    fn eat(&mut self, c: u8) -> bool {
        if self.peek() == Some(c) {
            self.i += 1;
            true
        } else {
            false
        }
    }

    fn at_end(&self) -> bool {
        self.i >= self.s.len()
    }

    fn enter(&mut self) -> Option<()> {
        self.depth += 1;
        (self.depth <= MAX_DEPTH).then_some(())
    }

    fn leave(&mut self) {
        self.depth -= 1;
    }

    fn register(&mut self, text: &str) {
        if self.subs.len() < 4096 {
            self.subs.push(text.to_owned());
        }
    }

    fn number(&mut self) -> Option<usize> {
        let start = self.i;
        while self.peek().is_some_and(|c| c.is_ascii_digit()) {
            self.i += 1;
        }
        if self.i == start || self.i - start > 8 {
            return None;
        }
        std::str::from_utf8(&self.s[start..self.i]).ok()?.parse().ok()
    }

    fn source_name(&mut self) -> Option<String> {
        let len = self.number()?;
        let end = self.i.checked_add(len)?;
        let ident = std::str::from_utf8(self.s.get(self.i..end)?).ok()?;
        self.i = end;
        Some(if ident.starts_with("_GLOBAL__N") { "(anonymous namespace)".to_owned() } else { ident.to_owned() })
    }

    // <encoding>
    fn encoding(&mut self) -> Option<String> {
        if self.peek() == Some(b'T') {
            let prefix = match self.peek_at(1)? {
                b'V' => "vtable for ",
                b'T' => "VTT for ",
                b'I' => "typeinfo for ",
                b'S' => "typeinfo name for ",
                _ => return None,
            };
            self.i += 2;
            return Some(format!("{prefix}{}", self.ty()?));
        }
        if self.peek() == Some(b'G') && self.peek_at(1) == Some(b'V') {
            self.i += 2;
            return Some(format!("guard variable for {}", self.name()?.text));
        }
        let name = self.name()?;
        if self.at_end() || self.peek() == Some(b'.') {
            return Some(self.with_clone_suffix(name.text));
        }
        let mut ret = None;
        if name.templated && !name.special {
            ret = Some(self.ty()?);
        }
        let mut params: Vec<String> = Vec::new();
        while !self.at_end() && self.peek() != Some(b'.') {
            params.push(self.ty()?);
        }
        if params.len() == 1 && params[0] == "void" {
            params.clear();
        }
        let mut text = String::new();
        if let Some(r) = ret {
            text.push_str(&r);
            text.push(' ');
        }
        text.push_str(&name.text);
        text.push('(');
        text.push_str(&params.join(", "));
        text.push(')');
        if !name.quals.is_empty() {
            text.push(' ');
            text.push_str(&name.quals);
        }
        Some(self.with_clone_suffix(text))
    }

    fn with_clone_suffix(&self, text: String) -> String {
        match self.peek() {
            Some(b'.') => format!("{text} [clone {}]", String::from_utf8_lossy(&self.s[self.i..])),
            _ => text,
        }
    }

    // <name>
    fn name(&mut self) -> Option<Name> {
        self.enter()?;
        let result = match self.peek()? {
            b'N' => {
                self.i += 1;
                self.nested()
            }
            b'S' if self.peek_at(1) == Some(b't') => {
                self.i += 2;
                let unq = self.source_name()?;
                let mut text = format!("std::{unq}");
                self.register(&text);
                let mut templated = false;
                if self.peek() == Some(b'I') {
                    text.push_str(&self.template_args(true)?);
                    templated = true;
                }
                Some(Name { text, templated, special: false, quals: String::new() })
            }
            b'0'..=b'9' => {
                let mut text = self.source_name()?;
                let mut templated = false;
                if self.peek() == Some(b'I') {
                    self.register(&text);
                    text.push_str(&self.template_args(true)?);
                    templated = true;
                }
                Some(Name { text, templated, special: false, quals: String::new() })
            }
            b'L' => {
                self.i += 1;
                self.name()
            }
            b'S' => {
                let mut text = self.substitution()?;
                let mut templated = false;
                if self.peek() == Some(b'I') {
                    text.push_str(&self.template_args(true)?);
                    templated = true;
                }
                Some(Name { text, templated, special: false, quals: String::new() })
            }
            _ => {
                // Unscoped operator name: _Zpl..., _ZnwmE ...
                let (text, special) = self.operator_name()?;
                let mut text = format!("operator{text}");
                let mut templated = false;
                if self.peek() == Some(b'I') {
                    self.register(&text);
                    text.push_str(&self.template_args(true)?);
                    templated = true;
                }
                Some(Name { text, templated, special, quals: String::new() })
            }
        };
        self.leave();
        result
    }

    /// After `cv` / operator letters: returns (text after `operator`, is conversion).
    fn operator_name(&mut self) -> Option<(String, bool)> {
        let code = [self.peek()?, self.peek_at(1)?];
        if &code == b"cv" {
            self.i += 2;
            let target = self.ty()?;
            return Some((format!(" {target}"), true));
        }
        let op = operator(&code)?;
        self.i += 2;
        let spaced = op.chars().next().is_some_and(char::is_alphabetic);
        Some((if spaced { format!(" {op}") } else { op.to_owned() }, false))
    }

    // N [cv] [ref] <prefix> <unqualified-name> E   (the N is already consumed)
    fn nested(&mut self) -> Option<Name> {
        let mut quals = String::new();
        loop {
            match self.peek()? {
                b'K' => quals.push_str(if quals.is_empty() { "const" } else { " const" }),
                b'V' => quals.push_str(if quals.is_empty() { "volatile" } else { " volatile" }),
                b'r' => quals.push_str(if quals.is_empty() { "restrict" } else { " restrict" }),
                b'R' => quals.push_str(if quals.is_empty() { "&" } else { " &" }),
                b'O' => quals.push_str(if quals.is_empty() { "&&" } else { " &&" }),
                _ => break,
            }
            self.i += 1;
        }
        let mut prefix = String::new();
        let mut last = String::new();
        let mut templated = false;
        let mut special = false;
        let join = |prefix: &str, part: &str| if prefix.is_empty() { part.to_owned() } else { format!("{prefix}::{part}") };
        loop {
            let c = self.peek()?;
            if c == b'E' {
                self.i += 1;
                break;
            }
            templated = false;
            match c {
                b'S' => {
                    // An abbreviation / back-reference is already a candidate (or deliberately
                    // not one), so it is not registered again here.
                    let sub = self.substitution()?;
                    last = sub.rsplit("::").next().unwrap_or(&sub).to_owned();
                    prefix = sub;
                    continue;
                }
                b'I' => {
                    let args = self.template_args(true)?;
                    prefix.push_str(&args);
                    templated = true;
                    if self.peek() != Some(b'E') {
                        let p = prefix.clone();
                        self.register(&p);
                    }
                    continue;
                }
                b'L' => {
                    self.i += 1;
                    continue;
                }
                b'0'..=b'9' => {
                    let part = self.source_name()?;
                    last = part.clone();
                    prefix = join(&prefix, &part);
                }
                b'C' => {
                    self.i += 1;
                    if self.peek() == Some(b'I') {
                        return None; // inheriting constructor
                    }
                    self.number()?;
                    prefix = join(&prefix, &last);
                    special = true;
                }
                b'D' => {
                    self.i += 1;
                    self.number()?;
                    prefix = join(&prefix, &format!("~{last}"));
                    special = true;
                }
                _ => {
                    let (text, conversion) = self.operator_name()?;
                    prefix = join(&prefix, &format!("operator{text}"));
                    special |= conversion;
                }
            }
            if self.peek() != Some(b'E') {
                let p = prefix.clone();
                self.register(&p);
            }
        }
        if prefix.is_empty() {
            return None;
        }
        Some(Name { text: prefix, templated, special, quals })
    }

    // S_ / S<seq>_ / St / Sa ...; returns the expansion (does not register it).
    fn substitution(&mut self) -> Option<String> {
        if !self.eat(b'S') {
            return None;
        }
        let c = self.peek()?;
        if let Some(abbr) = std_abbreviation(c) {
            self.i += 1;
            if c == b't' {
                // `St` is only a prefix: the unqualified name follows.
                let unq = self.source_name()?;
                let text = format!("std::{unq}");
                self.register(&text);
                return Some(text);
            }
            return Some(abbr.to_owned());
        }
        let mut index = 0usize;
        if c != b'_' {
            let mut value = 0usize;
            while let Some(d) = self.peek() {
                if d == b'_' {
                    break;
                }
                let digit = match d {
                    b'0'..=b'9' => usize::from(d - b'0'),
                    b'A'..=b'Z' => usize::from(d - b'A') + 10,
                    _ => return None,
                };
                value = value.checked_mul(36)?.checked_add(digit)?;
                self.i += 1;
            }
            index = value + 1;
        }
        if !self.eat(b'_') {
            return None;
        }
        self.subs.get(index).cloned()
    }

    // I <arg>+ E ; `record` stores the arguments for later `T_` references.
    fn template_args(&mut self, record: bool) -> Option<String> {
        if !self.eat(b'I') {
            return None;
        }
        self.enter()?;
        let mut args = Vec::new();
        while self.peek()? != b'E' {
            self.template_arg(&mut args)?;
        }
        self.i += 1;
        self.leave();
        let text = join_template(&args);
        if record {
            self.tpl = args;
        }
        Some(text)
    }

    fn template_arg(&mut self, out: &mut Vec<String>) -> Option<()> {
        match self.peek()? {
            b'L' => {
                self.i += 1;
                let literal = self.literal()?;
                out.push(literal);
            }
            b'J' => {
                self.i += 1;
                while self.peek()? != b'E' {
                    self.template_arg(out)?;
                }
                self.i += 1;
            }
            b'X' => return None,
            _ => out.push(self.ty()?),
        }
        Some(())
    }

    // L <type> <value> E
    fn literal(&mut self) -> Option<String> {
        let ty = self.ty()?;
        let negative = self.eat(b'n');
        let start = self.i;
        while self.peek()? != b'E' {
            self.i += 1;
        }
        let digits = std::str::from_utf8(&self.s[start..self.i]).ok()?.to_owned();
        self.i += 1;
        if !digits.bytes().all(|b| b.is_ascii_digit()) || digits.is_empty() {
            return None;
        }
        let sign = if negative { "-" } else { "" };
        Some(match ty.as_str() {
            "bool" => (if digits == "0" { "false" } else { "true" }).to_owned(),
            "int" => format!("{sign}{digits}"),
            "unsigned int" => format!("{digits}u"),
            "long" => format!("{sign}{digits}l"),
            "unsigned long" => format!("{digits}ul"),
            "long long" => format!("{sign}{digits}ll"),
            "unsigned long long" => format!("{digits}ull"),
            _ => format!("({ty}){sign}{digits}"),
        })
    }

    // <type>
    fn ty(&mut self) -> Option<String> {
        self.enter()?;
        let result = self.ty_inner();
        self.leave();
        let text = result?;
        (text.len() <= MAX_LEN).then_some(text)
    }

    fn ty_inner(&mut self) -> Option<String> {
        let c = self.peek()?;
        if let Some(b) = builtin(c) {
            self.i += 1;
            return Some(b.to_owned());
        }
        match c {
            b'P' | b'R' | b'O' => {
                self.i += 1;
                let inner = self.ty()?;
                let text = format!("{inner}{}", match c {
                    b'P' => "*",
                    b'R' => "&",
                    _ => "&&",
                });
                self.register(&text);
                Some(text)
            }
            b'K' | b'V' | b'r' => {
                self.i += 1;
                let inner = self.ty()?;
                let word = match c {
                    b'K' => "const",
                    b'V' => "volatile",
                    _ => "restrict",
                };
                let text = format!("{inner} {word}");
                self.register(&text);
                Some(text)
            }
            b'S' => {
                let text = self.substitution()?;
                if self.peek() == Some(b'I') {
                    let full = format!("{text}{}", self.template_args(false)?);
                    self.register(&full);
                    return Some(full);
                }
                Some(text)
            }
            b'0'..=b'9' => {
                let mut text = self.source_name()?;
                self.register(&text);
                if self.peek() == Some(b'I') {
                    text.push_str(&self.template_args(false)?);
                    self.register(&text);
                }
                Some(text)
            }
            b'N' => {
                self.i += 1;
                let name = self.nested()?;
                let text = if name.quals.is_empty() { name.text } else { format!("{} {}", name.text, name.quals) };
                self.register(&text);
                Some(text)
            }
            b'T' => {
                self.i += 1;
                let index = if self.eat(b'_') {
                    0
                } else {
                    let n = self.number()?;
                    if !self.eat(b'_') {
                        return None;
                    }
                    n + 1
                };
                let text = self.tpl.get(index)?.clone();
                self.register(&text);
                Some(text)
            }
            b'A' => {
                self.i += 1;
                let len = self.number()?;
                if !self.eat(b'_') {
                    return None;
                }
                let element = self.ty()?;
                let text = format!("{element} [{len}]");
                self.register(&text);
                Some(text)
            }
            b'D' => {
                let text = match self.peek_at(1)? {
                    b'n' => "decltype(nullptr)",
                    b'i' => "char32_t",
                    b's' => "char16_t",
                    b'u' => "char8_t",
                    b'a' => "auto",
                    _ => return None,
                };
                self.i += 2;
                Some(text.to_owned())
            }
            _ => None,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn d(name: &str) -> Option<String> {
        demangle(name)
    }

    #[test]
    fn simple_and_nested_functions() {
        assert_eq!(d("_Z3addii").as_deref(), Some("add(int, int)"));
        assert_eq!(d("_ZN3foo3barEv").as_deref(), Some("foo::bar()"));
        assert_eq!(d("_Z4funcPKc").as_deref(), Some("func(char const*)"));
        assert_eq!(d("_ZNK3Foo3getEv").as_deref(), Some("Foo::get() const"));
    }

    #[test]
    fn constructors_destructors_and_operators() {
        assert_eq!(d("_ZN3FooC1Ev").as_deref(), Some("Foo::Foo()"));
        assert_eq!(d("_ZN3FooD0Ev").as_deref(), Some("Foo::~Foo()"));
        assert_eq!(d("_ZplRK3FooS1_").as_deref(), Some("operator+(Foo const&, Foo const&)"));
    }

    #[test]
    fn templates_and_std() {
        assert_eq!(
            d("_ZNSt6vectorIiSaIiEE9push_backERKi").as_deref(),
            Some("std::vector<int, std::allocator<int> >::push_back(int const&)")
        );
        assert_eq!(d("_ZSt4cout").as_deref(), Some("std::cout"));
    }

    #[test]
    fn special_names_versions_and_clones() {
        assert_eq!(d("_ZTV3Foo").as_deref(), Some("vtable for Foo"));
        assert_eq!(d("__Z3addii").as_deref(), Some("add(int, int)"));
        assert_eq!(d("_Z3addii@@VER_1").as_deref(), Some("add(int, int)@@VER_1"));
        assert_eq!(d("_Z3fooi.part.0").as_deref(), Some("foo(int) [clone .part.0]"));
    }

    #[test]
    fn msvc_names_reduce_to_the_qualified_name() {
        assert_eq!(d("?bar@Foo@@QEAAXXZ").as_deref(), Some("Foo::bar"));
        assert_eq!(d("?main@@YAHXZ").as_deref(), Some("main"));
    }

    #[test]
    fn plain_and_broken_names_are_left_alone() {
        for name in ["main", "_start", "_Z", "", "#ordinal 7", "_ZN3foo", "_ZNZ", "_Z3fooX", "_ZN3foo3barIiE"] {
            assert_eq!(d(name), None, "{name}");
        }
    }

    #[test]
    fn deep_nesting_cannot_overflow_the_stack() {
        let name = format!("_Z1f{}i", "P".repeat(10_000));
        let _ = d(&name); // must return (None), not crash
    }

    #[test]
    fn display_name_keeps_duplicate_suffix_and_respects_toggle() {
        assert_eq!(display_name("_Z3addii#2", "_Z3addii", true), "add(int, int)#2");
        assert_eq!(display_name("_Z3addii", "_Z3addii", false), "_Z3addii");
        assert_eq!(display_name("main", "main", true), "main");
    }
}
