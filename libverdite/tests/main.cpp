#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <verdite/crash.h>

#include <fstream>

int main(int argc, char **argv) {
    // The crash tests start this binary again as the crash helper.
    if (const auto code = verdite::crash_helper_run(argc, argv))
        return *code;
    // And as the crash window: leaves a mark for the test to find.
    if (const auto window = verdite::crash_window_request(argc, argv)) {
        std::ofstream(window->folder + "/window-started") << "yes";
        return window->exit_code;
    }
    return doctest::Context(argc, argv).run();
}
