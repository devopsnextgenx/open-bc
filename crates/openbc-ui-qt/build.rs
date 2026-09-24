#[cfg(feature = "qt")]
fn main() {
    println!("cargo:rerun-if-changed=src/qt_app.cpp");
    println!("cargo:rerun-if-env-changed=PKG_CONFIG_PATH");

    // Fix for Windows MinGW/UCRT64: Ensure C++ standard library is linked
    if cfg!(target_os = "windows") && cfg!(target_env = "gnu") {
        println!("cargo:rustc-link-lib=stdc++");
    }

    let qt_widgets = pkg_config::Config::new()
        .probe("Qt6Widgets")
        .expect("Qt6Widgets development files are required to build the Qt UI");

    cxx_qt_build::CxxQtBuilder::new()
        .qt_module("Widgets")
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
            for include_path in &qt_widgets.include_paths {
                cc.include(include_path);
            }
            cc.flag_if_supported("-std=c++17");
        })
        .build();
}

#[cfg(not(feature = "qt"))]
fn main() {}
