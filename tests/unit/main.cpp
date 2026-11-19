#include "minitest.h"

#include <cstring>
#include <filesystem>

// Usage: build/unit_tests [filter]   (run from the repository root: the tests read
// errorchecklist.txt and tests/fixtures/ relative to the working directory)
int main(int argc, char** argv) {
    if (!std::filesystem::exists("tests/fixtures/fullstack_sample") || !std::filesystem::exists("errorchecklist.txt")) {
        std::cout << "Run the unit tests from the repository root (make unit-test).\n";
        return 2;
    }

    int failedCases = 0, ran = 0;
    for (const auto& test : minitest::registry()) {
        if (argc > 1 && !std::strstr(test.name, argv[1])) continue;
        ++ran;
        const int before = minitest::failures();
        try {
            test.run();
        } catch (const minitest::Abort&) {
        } catch (const std::exception& e) {
            ++minitest::failures();
            std::cout << "    FAILED unexpected exception: " << e.what() << "\n";
        }
        const bool ok = minitest::failures() == before;
        if (!ok) ++failedCases;
        std::cout << (ok ? "  ok   " : "  FAIL ") << test.name << "\n";
    }

    std::cout << "\n" << ran << " test case(s), " << (ran - failedCases) << " passed, " << failedCases
              << " failed (" << minitest::checks() << " checks)\n";
    return failedCases == 0 ? 0 : 1;
}
