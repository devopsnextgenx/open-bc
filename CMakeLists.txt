cmake_minimum_required(VERSION 3.24)
project(OpenBC LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

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

# 3. Create C++ Qt Application Entrypoint
add_executable(OpenBC
    crates/openbc-ui-qt/src/qt_app.cpp
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

# 5. Windows-Specific Linker Flags
if(WIN32)
    target_compile_definitions(OpenBC PRIVATE OPENBC_BUILD_EXECUTABLE)
    set_target_properties(OpenBC PROPERTIES WIN32_EXECUTABLE TRUE)
    target_link_libraries(OpenBC PRIVATE ws2_32 userenv advapi32 ntdll)
endif()