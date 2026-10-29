//! Built-in language table: which file extensions and file names belong to which language, and
//! which grammar renders that language.
//!
//! This is the single source of truth. The router in `language` indexes it, the Options page in
//! the Qt UI lists it (through the catalog export), and `docs/SYNTAX_LANGUAGES.md` is a rendering
//! of it. Never hard-code extension lists anywhere else.
//!
//! # Grammars
//!
//! Only two engines exist and each ships a fixed set of grammars:
//!
//! * `syntect` renders the Sublime default pack (about 60 syntaxes, referenced here by their exact
//!   syntax *name*).
//! * `tree-painter` renders Rust, Python and JavaScript through tree-sitter (referenced by the file
//!   extension it understands). Its CUDA and Lisp grammars ship an empty highlight query, so they
//!   would render as plain text and are deliberately not used.
//!
//! A language the engines have no grammar for is either mapped to the closest available grammar
//! and flagged `approximate` (TypeScript renders with the JavaScript grammar, Kotlin with Java,
//! and so on) or given [`PLAIN`] (assembly, Fortran, ...). Users can remap any extension in
//! Tools > Options > Syntax Highlighting.
//!
//! # Conventions
//!
//! * Keys are lower case, without a leading dot. Keys may contain dots (`html.erb`).
//! * `filenames` are whole file names, lower case, with a leading dot removed (`.bashrc` is
//!   `bashrc`). They only match the complete name, never a suffix.
//! * A key belongs to exactly one language (enforced by a test). Ambiguous extensions are decided
//!   once, here: `.h` is C++ (a superset of C for highlighting), `.m` is Objective-C, `.pl` is
//!   Perl, `.l` is Lisp, `.ts` is TypeScript, `.v` is Verilog, `.s` is assembly.

/// The engine that renders a grammar.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum Backend {
    Syntect,
    TreePainter,
    Plain,
}

/// A concrete grammar.
#[derive(Clone, Copy, Debug)]
pub(super) struct Grammar {
    pub backend: Backend,
    /// syntect: exact syntax name. tree-painter: the extension it recognises. Plain: empty.
    pub key: &'static str,
    /// The grammar is a stand-in for a language it was not written for.
    pub approximate: bool,
}

/// One language and everything that selects it.
#[derive(Debug)]
pub(super) struct Language {
    /// Display name, unique (case-insensitively).
    pub name: &'static str,
    /// Extra names accepted when a language is requested by name (`cpp`, `golang`, ...).
    pub aliases: &'static [&'static str],
    pub extensions: &'static [&'static str],
    pub filenames: &'static [&'static str],
    pub grammar: Grammar,
}

const fn st(name: &'static str) -> Grammar {
    Grammar { backend: Backend::Syntect, key: name, approximate: false }
}
const fn st_approx(name: &'static str) -> Grammar {
    Grammar { backend: Backend::Syntect, key: name, approximate: true }
}
const fn tp(extension: &'static str) -> Grammar {
    Grammar { backend: Backend::TreePainter, key: extension, approximate: false }
}
pub(super) const PLAIN: Grammar = Grammar { backend: Backend::Plain, key: "", approximate: false };

const fn lang(
    name: &'static str,
    aliases: &'static [&'static str],
    extensions: &'static [&'static str],
    filenames: &'static [&'static str],
    grammar: Grammar,
) -> Language {
    Language { name, aliases, extensions, filenames, grammar }
}

/// Name of the language that disables highlighting.
pub(super) const PLAIN_TEXT: &str = "Plain Text";

pub(super) static LANGUAGES: &[Language] = &[
    // ---- Plain text and data without a grammar ------------------------------------------------
    lang(
        PLAIN_TEXT,
        &["text", "plain", "plaintext", "none"],
        &["txt", "text", "log", "nfo", "csv", "tsv", "psv", "srt", "lst"],
        &[
            "license", "licence", "copying", "readme", "authors", "changelog", "notice", "todo",
            "contributors", "codeowners", "version", "gitignore", "gitattributes", "dockerignore",
            "npmignore", "eslintignore", "prettierignore", "hgignore", "gitkeep",
        ],
        PLAIN,
    ),
    // ---- C family -----------------------------------------------------------------------------
    lang("C", &[], &["c"], &[], st("C")),
    lang(
        "C++",
        &["cpp", "cxx", "cc"],
        &[
            "cpp", "cc", "cp", "cxx", "c++", "h", "hh", "hpp", "hxx", "h++", "inl", "ipp", "tpp",
            "txx", "tcc", "ixx", "cppm", "ino",
        ],
        &[],
        st("C++"),
    ),
    lang("Objective-C", &["objc", "obj-c"], &["m"], &[], st("Objective-C")),
    lang("Objective-C++", &["objcpp", "obj-c++"], &["mm"], &[], st("Objective-C++")),
    lang("C#", &["csharp"], &["cs", "csx"], &[], st("C#")),
    lang("CUDA", &[], &["cu", "cuh"], &[], st_approx("C++")),
    lang(
        "GLSL",
        &["opengl shading language"],
        &[
            "glsl", "vert", "frag", "geom", "tesc", "tese", "comp", "rgen", "rint", "rahit",
            "rchit", "rmiss", "rcall", "vsh", "fsh",
        ],
        &[],
        st_approx("C++"),
    ),
    lang(
        "HLSL",
        &["shaderlab"],
        &["hlsl", "hlsli", "fx", "fxh", "cginc", "cg", "compute", "shader"],
        &[],
        st_approx("C++"),
    ),
    lang("Metal", &["metal shading language"], &["metal"], &[], st_approx("C++")),
    lang("WGSL", &["webgpu shading language"], &["wgsl"], &[], st_approx("Rust")),
    lang("Windows Resource", &["rc"], &["rc", "rc2", "idl"], &[], st_approx("C++")),
    lang("Verilog", &["systemverilog", "sv"], &["v", "vh", "sv", "svh", "svi"], &[], st_approx("C++")),
    // ---- JVM and other C-like languages -------------------------------------------------------
    lang("Java", &[], &["java", "bsh", "jsh"], &[], st("Java")),
    lang("Java Properties", &["properties"], &["properties"], &[], st("Java Properties")),
    lang("Kotlin", &["kt"], &["kt", "kts", "ktm"], &[], st_approx("Java")),
    lang("Scala", &[], &["scala", "sbt", "sc"], &[], st("Scala")),
    lang("Groovy", &["gradle"], &["groovy", "gvy", "gy", "gsh", "gradle"], &["jenkinsfile"], st("Groovy")),
    lang("Processing", &[], &["pde"], &[], st_approx("Java")),
    lang("Swift", &[], &["swift"], &[], st_approx("Java")),
    lang("Dart", &[], &["dart"], &[], st_approx("Java")),
    lang("Vala", &[], &["vala", "vapi"], &[], st_approx("C#")),
    lang("Zig", &[], &["zig", "zon"], &[], st_approx("Rust")),
    lang("Go", &["golang"], &["go"], &[], st("Go")),
    lang("D", &["dlang"], &["d", "di"], &[], st("D")),
    lang("ActionScript", &["as3"], &["as"], &[], st("ActionScript")),
    lang("Haxe", &[], &["hx", "hxsl"], &[], st_approx("ActionScript")),
    lang("Protocol Buffers", &["protobuf", "proto"], &["proto"], &[], st_approx("Java")),
    lang("Thrift", &[], &["thrift"], &[], st_approx("Java")),
    lang("Solidity", &[], &["sol"], &[], st_approx("JavaScript")),
    // ---- Rust, Python, JavaScript (tree-sitter) -----------------------------------------------
    lang("Rust", &["rs"], &["rs"], &[], tp("rs")),
    lang(
        "Python",
        &["py"],
        &["py", "pyw", "pyi", "py3", "rpy", "cpy", "gyp", "gypi", "wsgi", "tac"],
        &["sconstruct", "sconscript", "snakefile", "wscript"],
        tp("py"),
    ),
    lang("Cython", &["pyx"], &["pyx", "pxd", "pxi", "pyx.in", "pxd.in", "pxi.in"], &[], st_approx("Python")),
    lang("GDScript", &["gd"], &["gd"], &[], st_approx("Python")),
    lang("Nim", &[], &["nim", "nims", "nimble"], &[], st_approx("Python")),
    lang("Mojo", &[], &["mojo"], &[], st_approx("Python")),
    lang(
        "Starlark",
        &["bazel"],
        &["bzl", "bazel", "star", "sky", "bxl"],
        &["build", "workspace", "tiltfile", "meson.build", "meson_options.txt"],
        st_approx("Python"),
    ),
    lang(
        "JavaScript",
        &["js", "node", "nodejs", "ecmascript"],
        &["js", "mjs", "cjs", "jsx", "es", "es6", "jsm"],
        &["jakefile"],
        tp("js"),
    ),
    lang("TypeScript", &["ts"], &["ts", "tsx", "mts", "cts"], &[], st_approx("JavaScript")),
    lang("Jsonnet", &[], &["jsonnet", "libsonnet"], &[], st_approx("JavaScript")),
    // ---- Data and configuration ---------------------------------------------------------------
    lang(
        "JSON",
        &[],
        &[
            "json", "geojson", "topojson", "har", "webmanifest", "jsonld", "jsonl", "ndjson", "ipynb",
            "avsc", "babelrc", "eslintrc", "prettierrc", "stylelintrc", "jshintrc", "swcrc",
            "sublime-settings", "sublime-menu", "sublime-keymap", "sublime-mousemap",
            "sublime-theme", "sublime-build", "sublime-project", "sublime-completions",
            "sublime-commands", "sublime-macro", "sublime-color-scheme",
        ],
        &[],
        st("JSON"),
    ),
    lang("JSON with Comments", &["jsonc", "json5"], &["jsonc", "json5", "code-workspace"], &[], st_approx("JSON")),
    lang(
        "YAML",
        &["yml"],
        &["yaml", "yml", "sublime-syntax", "clang-format", "clang-tidy"],
        &[],
        st("YAML"),
    ),
    lang(
        "XML",
        &["xhtml-xml"],
        &[
            "xml", "xsd", "xslt", "xsl", "tld", "dtml", "rss", "opml", "svg", "atom", "plist",
            "csproj", "vbproj", "fsproj", "vcxproj", "vcproj", "props", "targets", "nuspec", "resx",
            "xaml", "axaml", "wxs", "wsdl", "kml", "gpx", "xliff", "xlf", "ui", "qrc", "rdf", "owl",
            "dtd", "ent", "config", "manifest", "storyboard", "xib", "mxml", "fxml", "jnlp", "dae",
            "x3d", "tmx",
        ],
        &[],
        st("XML"),
    ),
    lang(
        "INI",
        &["conf", "cfg", "config file"],
        &[
            "ini", "cfg", "conf", "inf", "prefs", "editorconfig", "gitconfig", "gitmodules",
            "desktop", "service", "socket", "timer", "mount", "tscn", "tres", "po", "pot",
        ],
        &[],
        st_approx("Java Properties"),
    ),
    lang(
        "TOML",
        &[],
        &["toml"],
        &["cargo.lock", "poetry.lock", "pipfile"],
        st_approx("Java Properties"),
    ),
    lang(
        "Terraform (HCL)",
        &["terraform", "hcl"],
        &["tf", "tfvars", "hcl", "nomad"],
        &[],
        st_approx("Bourne Again Shell (bash)"),
    ),
    // ---- Web ----------------------------------------------------------------------------------
    lang("HTML", &["htm", "xhtml"], &["html", "htm", "shtml", "xhtml", "tmpl", "tpl", "hta"], &[], st("HTML")),
    lang(
        "HTML Templates",
        &["handlebars", "mustache", "jinja", "jinja2", "twig", "liquid", "ejs", "nunjucks"],
        &[
            "hbs", "handlebars", "mustache", "j2", "jinja", "jinja2", "twig", "liquid", "njk", "ejs",
            "gohtml", "volt", "adp",
        ],
        &[],
        st_approx("HTML"),
    ),
    lang("Vue", &[], &["vue"], &[], st_approx("HTML")),
    lang("Svelte", &[], &["svelte"], &[], st_approx("HTML")),
    lang("Astro", &[], &["astro"], &[], st_approx("HTML")),
    lang("Razor", &["cshtml"], &["cshtml", "razor", "vbhtml"], &[], st_approx("HTML")),
    lang("ASP", &[], &["asa"], &[], st("ASP")),
    lang("HTML (ASP)", &[], &["asp"], &[], st("HTML (ASP)")),
    lang(
        "ASP.NET",
        &["aspx"],
        &["aspx", "ascx", "ashx", "asmx", "asax", "master"],
        &[],
        st_approx("HTML (ASP)"),
    ),
    lang("JSP", &["java server page"], &["jsp", "jspf", "jspx"], &[], st("Java Server Page (JSP)")),
    lang("HTML (Erlang)", &["yaws"], &["yaws"], &[], st("HTML (Erlang)")),
    lang("HTML (Rails)", &["erb", "rhtml"], &["erb", "rhtml", "rails", "html.erb"], &[], st("HTML (Rails)")),
    lang("JavaScript (Rails)", &[], &["js.erb"], &[], st("JavaScript (Rails)")),
    lang("SQL (Rails)", &[], &["erbsql", "sql.erb"], &[], st("SQL (Rails)")),
    lang("Ruby on Rails", &["rails-ruby"], &["rxml", "builder"], &[], st("Ruby on Rails")),
    lang("CSS", &[], &["css", "pcss", "postcss", "css.erb", "css.liquid"], &[], st("CSS")),
    lang("SCSS", &[], &["scss"], &[], st_approx("CSS")),
    lang("Sass", &[], &["sass"], &[], st_approx("CSS")),
    lang("Less", &[], &["less"], &[], st_approx("CSS")),
    lang("Stylus", &[], &["styl", "stylus"], &[], st_approx("CSS")),
    lang("Haml", &[], &["haml"], &[], st("Ruby Haml")),
    // ---- Documents and markup -----------------------------------------------------------------
    lang(
        "Markdown",
        &["md"],
        &["md", "markdown", "mdown", "mkd", "mkdn", "mdwn", "markdn", "livemd"],
        &[],
        st("Markdown"),
    ),
    lang("MDX", &[], &["mdx"], &[], st_approx("Markdown")),
    lang("R Markdown", &["rmd", "quarto"], &["rmd", "qmd"], &[], st_approx("Markdown")),
    lang("reStructuredText", &["rst"], &["rst", "rest"], &[], st("reStructuredText")),
    lang("Textile", &[], &["textile"], &[], st("Textile")),
    lang("LaTeX", &[], &["tex", "ltx", "latex"], &[], st("LaTeX")),
    lang("TeX", &[], &["sty", "cls"], &[], st("TeX")),
    lang("BibTeX", &[], &["bib"], &[], st("BibTeX")),
    lang("Diff", &["patch"], &["diff", "patch", "rej"], &[], st("Diff")),
    lang("Graphviz (DOT)", &["dot", "graphviz"], &["dot", "gv"], &[], st("Graphviz (DOT)")),
    // ---- Shell and build ----------------------------------------------------------------------
    lang(
        "Shell",
        &["sh", "bash", "zsh", "fish", "ksh"],
        &[
            "sh", "bash", "zsh", "fish", "ksh", "dash", "ash", "csh", "tcsh", "command", "bats",
            "envrc", "env", "ebuild", "eclass", "zsh-theme",
        ],
        &[
            "bashrc", "bash_profile", "bash_login", "bash_logout", "bash_aliases", "profile",
            "zshrc", "zshenv", "zprofile", "zlogin", "zlogout", "kshrc", "pkgbuild", "apkbuild",
            "bash_completions", "bash_functions", "bash_variables", "textmate_init",
        ],
        st("Bourne Again Shell (bash)"),
    ),
    lang("Batch File", &["batch", "bat", "cmd"], &["bat", "cmd", "btm"], &[], st("Batch File")),
    lang(
        "PowerShell",
        &["ps1", "pwsh"],
        &["ps1", "psm1", "psd1", "ps1xml", "pssc", "psrc", "cdxml"],
        &[],
        st_approx("Bourne Again Shell (bash)"),
    ),
    lang(
        "Dockerfile",
        &["docker", "containerfile"],
        &["dockerfile", "containerfile"],
        &["dockerfile", "containerfile"],
        st_approx("Bourne Again Shell (bash)"),
    ),
    lang(
        "CMake",
        &[],
        &["cmake"],
        &["cmakelists.txt"],
        st_approx("Bourne Again Shell (bash)"),
    ),
    lang(
        "Makefile",
        &["make"],
        &["make", "mk", "mak", "mkfile", "am"],
        &["makefile", "gnumakefile", "ocamlmakefile", "kbuild"],
        st("Makefile"),
    ),
    // ---- Scripting languages ------------------------------------------------------------------
    lang(
        "Ruby",
        &["rb"],
        &[
            "rb", "rbw", "rake", "gemspec", "ru", "podspec", "thor", "jbuilder", "rabl", "rbx",
            "irbrc", "prawn", "rjs", "simplecov", "ruby.rail",
        ],
        &[
            "rakefile", "gemfile", "guardfile", "capfile", "vagrantfile", "brewfile", "podfile",
            "fastfile", "appfile", "berksfile", "thorfile", "snapfile", "deliverfile", "scanfile",
            "cheffile", "appraisals", "dangerfile", "rantfile",
        ],
        st("Ruby"),
    ),
    lang("Crystal", &[], &["cr"], &[], st_approx("Ruby")),
    lang("Elixir", &[], &["ex", "exs", "eex", "heex", "leex"], &[], st_approx("Ruby")),
    lang("Julia", &["jl"], &["jl"], &[], st_approx("Ruby")),
    lang("Gleam", &[], &["gleam"], &[], st_approx("Rust")),
    lang(
        "PHP",
        &[],
        &["php", "php3", "php4", "php5", "php7", "php8", "phps", "phpt", "phtml", "pht"],
        &[],
        st("PHP"),
    ),
    lang("Perl", &[], &["pl", "pm", "pod", "t", "perl", "plx", "psgi"], &["cpanfile"], st("Perl")),
    lang("Raku", &["perl6"], &["raku", "rakumod", "rakudoc", "rakutest", "p6", "pm6", "pl6", "nqp"], &[], st_approx("Perl")),
    lang("AWK", &[], &["awk", "gawk"], &[], st_approx("Perl")),
    lang("Lua", &[], &["lua", "wlua", "rockspec"], &[], st("Lua")),
    lang("Luau", &[], &["luau"], &[], st_approx("Lua")),
    lang("R", &[], &["r", "rprofile"], &[], st("R")),
    lang("Rd (R Documentation)", &["rd"], &["rd"], &[], st("Rd (R Documentation)")),
    lang("MATLAB", &[], &["matlab"], &[], st("MATLAB")),
    lang("Tcl", &[], &["tcl", "tk", "itcl", "itk", "tm"], &[], st("Tcl")),
    lang("AppleScript", &[], &["applescript"], &[], st("AppleScript")),
    lang("Regular Expression", &["regex"], &["re"], &[], st("Regular Expression")),
    // ---- Functional languages -----------------------------------------------------------------
    lang("Haskell", &["hs"], &["hs", "hsc", "hs-boot"], &[], st("Haskell")),
    lang("Literate Haskell", &["lhs"], &["lhs"], &[], st("Literate Haskell")),
    lang("Elm", &[], &["elm"], &[], st_approx("Haskell")),
    lang("PureScript", &[], &["purs"], &[], st_approx("Haskell")),
    lang("Idris", &[], &["idr", "idris2"], &[], st_approx("Haskell")),
    lang("Agda", &[], &["agda"], &[], st_approx("Haskell")),
    lang("Lean", &[], &["lean"], &[], st_approx("Haskell")),
    lang("OCaml", &["ml"], &["ml", "mli", "eliom", "eliomi", "ml4"], &[], st("OCaml")),
    lang("OCamllex", &[], &["mll"], &[], st("OCamllex")),
    lang("OCamlyacc", &["menhir"], &["mly"], &[], st("OCamlyacc")),
    lang("F#", &["fsharp"], &["fs", "fsi", "fsx", "fsscript"], &[], st_approx("OCaml")),
    lang("Standard ML", &["sml"], &["sml", "sig", "fun"], &[], st_approx("OCaml")),
    lang("Erlang", &[], &["erl", "hrl", "escript", "xrl", "yrl"], &["emakefile"], st("Erlang")),
    lang("Clojure", &["clj"], &["clj", "cljs", "cljc", "cljx", "edn", "boot", "bb"], &[], st("Clojure")),
    lang("Common Lisp", &["lisp", "commonlisp"], &["lisp", "lsp", "cl", "clisp", "l", "mud", "asd"], &[], st("Lisp")),
    lang("Emacs Lisp", &["elisp"], &["el"], &[], st_approx("Lisp")),
    lang("Scheme", &[], &["scm", "ss", "sld", "sls", "sch"], &[], st_approx("Lisp")),
    lang("Racket", &[], &["rkt", "rktl", "rktd", "scrbl"], &[], st_approx("Lisp")),
    lang("Fennel", &[], &["fnl"], &[], st_approx("Lisp")),
    lang("Hy", &[], &["hy"], &[], st_approx("Lisp")),
    // ---- Databases ----------------------------------------------------------------------------
    lang("SQL", &[], &["sql", "ddl", "dml"], &[], st("SQL")),
    lang(
        "SQL Dialects",
        &["postgresql", "mysql", "tsql", "plsql", "t-sql", "pl/sql"],
        &[
            "pgsql", "psql", "mysql", "tsql", "plsql", "pls", "plb", "pck", "pks", "pkb", "hql",
            "cql",
        ],
        &[],
        st_approx("SQL"),
    ),
    // ---- Other languages ----------------------------------------------------------------------
    lang("Pascal", &["delphi", "object pascal"], &["pas", "pp", "p", "dpr", "dpk", "lpr"], &[], st("Pascal")),
    // ---- Known languages without a usable grammar (rendered as plain text) --------------------
    lang("Assembly", &["asm", "nasm"], &["asm", "s", "nasm", "masm", "a51"], &[], PLAIN),
    lang("Fortran", &[], &["f", "for", "f77", "f90", "f95", "f03", "f08", "fpp"], &[], PLAIN),
    lang("COBOL", &[], &["cob", "cbl", "cobol"], &[], PLAIN),
    lang("Ada", &[], &["adb", "ads", "ada", "gpr"], &[], PLAIN),
    lang("VHDL", &[], &["vhd", "vhdl", "vho", "vht"], &[], PLAIN),
    lang("Visual Basic", &["vb", "vbscript"], &["vb", "bas", "frm", "vba", "vbs", "vbe"], &[], PLAIN),
    lang("Smalltalk", &[], &["st"], &[], PLAIN),
    lang("Prolog", &[], &["pro", "prolog"], &[], PLAIN),
    lang("CoffeeScript", &["coffee"], &["coffee", "litcoffee", "cson"], &[], PLAIN),
    lang("Pug", &["jade"], &["pug", "jade"], &[], PLAIN),
    lang("GraphQL", &[], &["graphql", "gql", "graphqls"], &[], PLAIN),
    lang("Nix", &[], &["nix"], &[], PLAIN),
    lang("AsciiDoc", &[], &["adoc", "asciidoc"], &[], PLAIN),
    lang("Org Mode", &["org"], &["org"], &[], PLAIN),
    lang("Vim Script", &["vim"], &["vim", "vimrc"], &[], PLAIN),
    lang("Gherkin", &["cucumber"], &["feature"], &[], PLAIN),
    lang("Mermaid", &[], &["mmd", "mermaid"], &[], PLAIN),
    lang("PlantUML", &[], &["puml", "plantuml", "wsd"], &[], PLAIN),
    lang("Ninja", &[], &["ninja"], &[], PLAIN),
    lang("Prisma", &[], &["prisma"], &[], PLAIN),
];
