cmake_minimum_required(VERSION 3.24)
project(openbc LANGUAGES C CXX)

if(WIN32)
    enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# Corrosion cannot use rustup's proxy when it probes the compiler version.
find_program(RUSTUP_EXECUTABLE rustup)
set(RUSTUP_RESULT 1)
if(RUSTUP_EXECUTABLE)
    execute_process(
        COMMAND "${RUSTUP_EXECUTABLE}" which rustc
        OUTPUT_VARIABLE RUSTC_EXECUTABLE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE RUSTUP_RESULT
    )
endif()
if(NOT RUSTUP_RESULT EQUAL 0 OR NOT EXISTS "${RUSTC_EXECUTABLE}")
    find_program(RUSTC_EXECUTABLE rustc REQUIRED)
endif()
set(Rust_COMPILER "${RUSTC_EXECUTABLE}" CACHE FILEPATH "Rust compiler used by Corrosion")

# 1. Find Qt6 Installation
# Multimedia / MultimediaWidgets are OPTIONAL: they power the Play buttons in Media Compare.
# Without them the app still builds and the Play buttons stay disabled.
# MSYS2: pacman -S mingw-w64-ucrt-x86_64-qt6-multimedia
find_package(Qt6 REQUIRED
    COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets
    OPTIONAL_COMPONENTS Multimedia MultimediaWidgets
)

# The Rust static lib (via Corrosion) pulls in a dependency that links
# zlib (e.g. suppaftp's TLS backend, or another crate using libz-sys).
# Static archives don't carry transitive link deps forward, so the final
# C++ link needs zlib named explicitly - otherwise `ld` reports undefined
# references to symbols like `inflateEnd` even though libz.so is present.
find_package(ZLIB REQUIRED)

find_package(LibLZMA REQUIRED)

# suppaftp's `tokio-async-native-tls` feature pulls in `native-tls`, which
# on Linux is backed by system OpenSSL via `openssl-sys`. Same "DSO missing
# from command line" issue as zlib above, this time for libssl/libcrypto -
# link them explicitly too.
find_package(OpenSSL REQUIRED)

# 2. Integrate Rust Cargo via Corrosion
include(FetchContent)
FetchContent_Declare(
    Corrosion
    GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
    GIT_TAG v0.5.0
)
FetchContent_MakeAvailable(Corrosion)

# Import ONLY the static library target, ignoring [[bin]] targets
corrosion_import_crate(
    MANIFEST_PATH "${CMAKE_CURRENT_SOURCE_DIR}/crates/openbc-ui-qt/Cargo.toml"
    CRATE_TYPES staticlib
)


set(OPENBC_HEADERS
    crates/openbc-ui-qt/src/qt_style.h
    crates/openbc-ui-qt/src/folder_model.h
    crates/openbc-ui-qt/src/diff_algorithms.h
    crates/openbc-ui-qt/src/syntax_highlighter.h
    crates/openbc-ui-qt/src/find_replace_bar.h
    crates/openbc-ui-qt/src/line_number_editor.h
    crates/openbc-ui-qt/src/minimap.h
    crates/openbc-ui-qt/src/diff_line_preview.h
    crates/openbc-ui-qt/src/text_compare_view.h
    crates/openbc-ui-qt/src/folder_compare_view.h
    crates/openbc-ui-qt/src/file_kinds.h
    crates/openbc-ui-qt/src/picture_compare_view.h
    crates/openbc-ui-qt/src/3d_compare_view.h
    crates/openbc-ui-qt/src/model3d_viewport.h
    crates/openbc-ui-qt/src/model3d_compare_api.h
    crates/openbc-ui-qt/src/model3d_extensions.h
    crates/openbc-ui-qt/src/model3d_icons.h
    crates/openbc-ui-qt/src/compare_backend_api.h
    crates/openbc-ui-qt/src/backend_channel.h
    crates/openbc-ui-qt/src/diff_grid_common.h
    crates/openbc-ui-qt/src/table_compare_viewer.h
    crates/openbc-ui-qt/src/registry_compare_viewer.h
    crates/openbc-ui-qt/src/hex_compare_viewer.h
    crates/openbc-ui-qt/src/media_player_pane.h
    crates/openbc-ui-qt/src/media_compare_viewer.h
    crates/openbc-ui-qt/src/main_window.h
)


# 3. Create C++ Qt Application Entrypoint
add_executable(openbc
    crates/openbc-ui-qt/src/qt_app.cpp
    ${OPENBC_HEADERS}
    resources/openbc.qrc
)

target_include_directories(openbc PRIVATE
    crates/openbc-ui-qt/src
)

# 4. Link Rust static library & Qt6 modules
target_link_libraries(openbc PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    openbc_ui_qt-static
    ZLIB::ZLIB
    OpenSSL::SSL
    OpenSSL::Crypto
    LibLZMA::LibLZMA
)

target_compile_definitions(openbc PRIVATE OPENBC_BUILD_EXECUTABLE)

# Media playback (optional). OPENBC_HAVE_QT_MULTIMEDIA is defined only when both modules were found.
if(TARGET Qt6::Multimedia AND TARGET Qt6::MultimediaWidgets)
    message(STATUS "Qt Multimedia found: media playback enabled")
    target_link_libraries(openbc PRIVATE Qt6::Multimedia Qt6::MultimediaWidgets)
    target_compile_definitions(openbc PRIVATE OPENBC_HAVE_QT_MULTIMEDIA)
else()
    message(STATUS "Qt Multimedia not found: media playback disabled (metadata compare still works)")
endif()

# 5. Windows-Specific Linker Flags
if(WIN32)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/resources/openbc.rc.in" "${CMAKE_CURRENT_BINARY_DIR}/openbc.rc" @ONLY)
    target_sources(openbc PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/openbc.rc")
    set_target_properties(openbc PROPERTIES WIN32_EXECUTABLE TRUE)
    target_link_libraries(openbc PRIVATE ws2_32 userenv advapi32 ntdll)
endif()


# --- Packaging Configuration ---
set(CPACK_PACKAGE_NAME "OpenBC")
set(CPACK_PACKAGE_VERSION "1.0.0")
set(CPACK_PACKAGE_VENDOR "OpenBC")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "OpenBC File & Directory Comparison Tool")

if(WIN32)
    set(CPACK_GENERATOR "WIX")
    set(CPACK_WIX_UPGRADE_GUID "a1b2c3d4-e5f6-7890-abcd-ef1234567890")
elseif(UNIX AND NOT APPLE)
    set(CPACK_GENERATOR "DEB")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "OpenBC Team")
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libqt6widgets6, zlib1g, libssl3")
endif()

include(CPack)