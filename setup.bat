@REM  # 1. Install Rust MSVC Target
rustup target add x86_64-pc-windows-msvc

@REM  # 2. Install Ninja & CMake via Winget
winget install Kitware.CMake Ninja-build.Ninja

@REM  # 3. Build using pure CMake and Ninja
cmake -B build -G "Ninja" -DCMAKE_BUILD_TYPE=Release
cmake --build build