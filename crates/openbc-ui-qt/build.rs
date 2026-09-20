#[cfg(feature = "qt")]
fn main() {
    cxx_qt_build::CxxQtBuilder::new()
        .qt_module("Widgets")
        .cc_builder(|cc| {
            cc.file("src/qt_app.cpp");
            cc.include("src");
            cc.flag_if_supported("-std=c++17");
        })
        .build();
}

#[cfg(not(feature = "qt"))]
fn main() {}