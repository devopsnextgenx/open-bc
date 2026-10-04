#[cfg(feature = "qt")]
fn main() {
    // The views are header-only and included from qt_app.cpp, so track the whole src dir,
    // otherwise editing a .h file does not trigger a rebuild.
    println!("cargo:rerun-if-changed=src");
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

    let mut include_paths = Vec::new();
    for lib in [&qt_widgets, &qt_opengl, &qt_opengl_widgets] {
        for p in &lib.include_paths {
            if !include_paths.contains(p) {
                include_paths.push(p.clone());
            }
        }
    }

    cxx_qt_build::CxxQtBuilder::new()
        .qt_module("Widgets")
        .qt_module("OpenGL")
        .qt_module("OpenGLWidgets")
        .qt_module("Qml")
        .qml_module(cxx_qt_build::QmlModule {
            uri: "OpenBC",
            version_major: 1,
            version_minor: 0,
            rust_files: &["src/highlight_bridge.rs"],
            qml_files: &["qml/HighlightedTextEdit.qml"],
            qrc_files: &[],
        })
        .cc_builder(move |cc| {
            cc.file("src/qt_app.cpp");
            cc.include("src");
            for include_path in &include_paths {
                cc.include(include_path);
            }
            cc.flag_if_supported("-std=c++17");
        })
        .build();
}

#[cfg(not(feature = "qt"))]
fn main() {}