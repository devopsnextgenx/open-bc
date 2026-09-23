cmake_minimum_required(VERSION 3.24)
project(OpenBC LANGUAGES C CXX)

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
find_package(Qt6 COMPONENTS Core Gui Widgets REQUIRED)

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
    crates/openbc-ui-qt/src/compare_session.h
    crates/openbc-ui-qt/src/main_window.h
)


# 3. Create C++ Qt Application Entrypoint
add_executable(OpenBC
    crates/openbc-ui-qt/src/qt_app.cpp ${OPENBC_HEADERS}
)

target_include_directories(OpenBC PRIVATE
    crates/openbc-ui-qt/src
)

# 4. Link Rust static library & Qt6 modules
target_link_libraries(OpenBC PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    openbc_ui_qt-static
)

target_compile_definitions(OpenBC PRIVATE OPENBC_BUILD_EXECUTABLE)

# 5. Windows-Specific Linker Flags
if(WIN32)
    set_target_properties(OpenBC PROPERTIES WIN32_EXECUTABLE TRUE)
    target_link_libraries(OpenBC PRIVATE ws2_32 userenv advapi32 ntdll)
endif()