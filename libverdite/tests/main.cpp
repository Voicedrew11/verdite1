#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <verdite/crash.h>

int main(int argc, char **argv) {
    // The crash tests start this binary again as the crash helper.
    if (const auto code = verdite::crash_helper_run(argc, argv))
        return *code;
    return doctest::Context(argc, argv).run();
}
