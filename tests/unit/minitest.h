#pragma once
// Minimal unit test harness.
//
// The project has no C++ test framework and Catch2/GoogleTest could not be fetched when
// these tests were written. The macros below are a source-compatible subset of Catch2
// (TEST_CASE, CHECK, REQUIRE), so switching later only means replacing this header by
// <catch2/catch_test_macros.hpp> and removing main.cpp.

#include <exception>
#include <iostream>
#include <vector>

namespace minitest {

struct TestCase {
    const char* name;
    void (*run)();
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(const char* name, void (*run)()) { registry().push_back({name, run}); }
};

struct Abort : std::exception {};   // thrown by REQUIRE to stop the current test case

inline int& checks() { static int n = 0; return n; }
inline int& failures() { static int n = 0; return n; }

inline bool report(bool ok, const char* expression, const char* file, int line) {
    ++checks();
    if (!ok) {
        ++failures();
        std::cout << "    FAILED " << file << ":" << line << ": " << expression << "\n";
    }
    return ok;
}

} // namespace minitest

#define MINITEST_CAT2(a, b) a##b
#define MINITEST_CAT(a, b) MINITEST_CAT2(a, b)

#define TEST_CASE(name)                                                                  \
    static void MINITEST_CAT(minitest_case_, __LINE__)();                                \
    static minitest::Registrar MINITEST_CAT(minitest_registrar_, __LINE__)(              \
        name, &MINITEST_CAT(minitest_case_, __LINE__));                                  \
    static void MINITEST_CAT(minitest_case_, __LINE__)()

#define CHECK(expression) minitest::report(static_cast<bool>(expression), #expression, __FILE__, __LINE__)

#define REQUIRE(expression)                                                              \
    do {                                                                                 \
        if (!minitest::report(static_cast<bool>(expression), #expression, __FILE__, __LINE__)) \
            throw minitest::Abort();                                                     \
    } while (false)
