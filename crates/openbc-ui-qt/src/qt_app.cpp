// main.cpp
// ---------------------------------------------------------------------------
// Executable entry point. openbc_run_gui() (main_window.h) does the actual
// work; this file exists so the GUI can also be linked as a library and
// started by other means (OPENBC_BUILD_EXECUTABLE off) without pulling in a
// main() that would collide with a host application's own.
// ---------------------------------------------------------------------------
#include "main_window.h"

#ifdef OPENBC_BUILD_EXECUTABLE
int main(int /*argc*/, char* /*argv*/[]) {
    return openbc::app::openbc_run_gui();
}
#endif
