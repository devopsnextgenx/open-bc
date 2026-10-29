#[cfg(feature = "qt")]
fn main() {
    // The views are mostly header-only and pulled in through cpp/app/qt_app.cpp, so track the
    // whole cpp/ tree (headers included) or editing a .h file would not trigger a rebuild.
    println!("cargo:rerun-if-changed=src");
    println!("cargo:rerun-if-changed=cpp");
    println!("cargo:rerun-if-changed=qml");
    println!("cargo:rerun-if-env-changed=PKG_CONFIG_PATH");

    // Fix for Windows MinGW/UCRT64: Ensure C++ standard library is linked
    if cfg!(target_os = "windows") && cfg!(target_env = "gnu") {
        println!("cargo:rustc-link-lib=stdc++");
    }

    let qt_widgets = pkg_config::Config::new()
        .probe("Qt6Widgets")
        .expect("Qt6Widgets development files are required to build the Qt UI");
    // 3D Compare viewport: QOpenGLWidget (OpenGLWidgets) + QOpenGLShaderProgram (OpenGL).
    let qt_opengl = pkg_config::Config::new()
        .probe("Qt6OpenGL")
        .expect("Qt6OpenGL development files are required for the 3D Compare tab");
    let qt_opengl_widgets = pkg_config::Config::new()
        .probe("Qt6OpenGLWidgets")
        .expect("Qt6OpenGLWidgets development files are required for the 3D Compare tab");
    // Release notices on the Home tab (home/update_notice.cpp) talk to the GitHub releases API.
    let qt_network = pkg_config::Config::new()
        .probe("Qt6Network")
        .expect("Qt6Network development files are required for the Home tab update notice");
    let qt_multimedia = pkg_config::Config::new()
        .cargo_metadata(false)
        .probe("Qt6Multimedia")
        .ok();
    let qt_multimedia_widgets = pkg_config::Config::new()
        .cargo_metadata(false)
        .probe("Qt6MultimediaWidgets")
        .ok();
    let multimedia_enabled = qt_multimedia.is_some() && qt_multimedia_widgets.is_some();

    let mut include_paths = Vec::new();
    for lib in [&qt_widgets, &qt_network, &qt_opengl, &qt_opengl_widgets]
        .into_iter()
        .chain(qt_multimedia.iter())
        .chain(qt_multimedia_widgets.iter())
    {
        for p in &lib.include_paths {
            if !include_paths.contains(p) {
                include_paths.push(p.clone());
            }
        }
    }

    let mut builder = cxx_qt_build::CxxQtBuilder::new()
        .qt_module("Widgets")
        .qt_module("Network")
        .qt_module("OpenGL")
        .qt_module("OpenGLWidgets")
        .qt_module("Qml");
    if multimedia_enabled {
        builder = builder
            .qt_module("Multimedia")
            .qt_module("MultimediaWidgets");
    }
    builder
        .qml_module(cxx_qt_build::QmlModule {
            uri: "OpenBC",
            version_major: 1,
            version_minor: 0,
            rust_files: &["src/highlight_bridge.rs"],
            qml_files: &["qml/HighlightedTextEdit.qml"],
            qrc_files: &[],
        })
        .cc_builder(move |cc| {
            // Every .cpp under cpp/ is part of the staticlib so the cargo `openbc-qt` binary links
            // the same C++ as the CMake `openbc` executable. Headers are found via `cpp/` as root.
            for source in cpp_sources(std::path::Path::new("cpp")) {
                cc.file(source);
            }
            cc.include("cpp");
            if multimedia_enabled {
                cc.define("OPENBC_HAVE_QT_MULTIMEDIA", None);
            }
            for include_path in &include_paths {
                cc.include(include_path);
            }
            cc.flag_if_supported("-std=c++17");
        })
        .build();
}

#[cfg(feature = "qt")]
fn cpp_sources(dir: &std::path::Path) -> Vec<std::path::PathBuf> {
    let mut found = Vec::new();
    if let Ok(entries) = std::fs::read_dir(dir) {
        for entry in entries.flatten() {
            let path = entry.path();
            if path.is_dir() {
                found.extend(cpp_sources(&path));
            } else if path.extension().is_some_and(|e| e == "cpp") && !is_standalone_tool(&path) {
                found.push(path);
            }
        }
    }
    found.sort();
    found
}

/// `cpp/tools/*` are separate executables (own `main()`), never part of the GUI library.
#[cfg(feature = "qt")]
fn is_standalone_tool(path: &std::path::Path) -> bool {
    path.components().any(|c| c.as_os_str() == "tools")
}

#[cfg(not(feature = "qt"))]
fn main() {}
