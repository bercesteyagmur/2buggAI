#include "minitest.h"
#include "test_support.h"

#include "ChecklistReader.h"
#include "ErrorDetection.h"
#include "ProcessRunner.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <unistd.h>

using support::contains;
namespace fs = std::filesystem;

namespace {
// Against the real errorchecklist.txt of the repository (cwd = repo root).
ErrorDetection::Result detect(const std::string& language, const std::string& build, bool buildFailed,
                              const std::string& run, bool runFailed) {
    ChecklistReader reader;
    return ErrorDetection::detect(reader.load(), language, build, buildFailed, run, runFailed);
}

const std::string kMavenOk =
    "[INFO] Tests run: 33, Failures: 0, Errors: 0, Skipped: 0\n[INFO] BUILD SUCCESS\n";

// Mediaplatfrom started without its database (real output, shortened)
const std::string kNoDatabase =
    "Exception in thread \"main\" database.DataAccessException: Datenbankverbindungsaufbau nicht erfolgreich\n"
    "\tat database.DatabaseManager.getConnection(DatabaseManager.java:18)\n"
    "\tat database.UnitOfWork.<init>(UnitOfWork.java:11)\n"
    "Caused by: org.postgresql.util.PSQLException: Connection to localhost:5432 refused. Check that the "
    "hostname and port are correct and that the postmaster is accepting TCP/IP connections.\n"
    "Caused by: java.net.ConnectException: Connection refused\n";
} // namespace

TEST_CASE("detection: ordinary output of a working program raises nothing") {
    const std::string out =
        "Server started on port 8080\n"
        "{\"id\": 1, \"deletedAt\": null, \"nullable\": true}\n"
        "connection timeout=30s, retry permissions checked\n"
        "Ran 5 tests in 0.012s\n\nOK\n";
    for (const char* lang : {"java", "python", "cpp", "c"}) {
        const auto r = detect(lang, kMavenOk, false, out, false);
        CHECK(r.errors.empty());
        CHECK(r.environment.empty());
    }
}

TEST_CASE("detection: specific null messages are still recognised") {
    CHECK(contains(detect("javascript", "", false,
                          "TypeError: Cannot read properties of null (reading 'id')", true).errors, "null_reference"));
    CHECK(contains(detect("python", "", false,
                          "AttributeError: 'NoneType' object has no attribute 'name'", true).errors, "null_reference"));
}

TEST_CASE("detection: unknown crash and unknown build failure are not 'no errors'") {
    const auto crash = detect("cpp", "", false, "boom: the widget exploded\n", true);
    CHECK(crash.errors == std::vector<std::string>{ErrorDetection::kUnrecognizedRuntime});

    const auto build = detect("cpp", "ld: something odd happened\ncollect2: ld returned 1 exit status\n", true, "", false);
    CHECK(build.errors == std::vector<std::string>{ErrorDetection::kUnrecognizedBuild});

    // a recognised error needs no fallback
    const auto known = detect("java", kMavenOk, false,
        "Exception in thread \"main\" java.lang.NullPointerException\n\tat Main.main(Main.java:3)\n", true);
    CHECK(contains(known.errors, "null_pointer_exception"));
    CHECK(!contains(known.errors, ErrorDetection::kUnrecognizedRuntime));

    // a server stopped by the time limit did not fail
    CHECK(detect("java", kMavenOk, false, "Server started\n", false).errors.empty());
}

TEST_CASE("detection: database down is an environment problem, not a code bug") {
    const auto r = detect("java", kMavenOk, false, kNoDatabase, true);
    CHECK(contains(r.environment, "env_service_unreachable"));
    CHECK(contains(r.errors, "uncaught_exception_main"));
    CHECK(r.environmentCaused.count("uncaught_exception_main") == 1);
    CHECK(r.environmentCaused.size() == r.errors.size());
    CHECK(!ErrorDetection::environmentHint("env_service_unreachable").empty());
}

TEST_CASE("detection: code errors next to an environment problem are still fixed") {
    // compile error in the build, database error in the (older) run output
    const auto r = detect("java", "Main.java:3: error: ';' expected\n", true, kNoDatabase, true);
    CHECK(contains(r.errors, "missing_semicolon_java"));
    CHECK(r.environmentCaused.count("missing_semicolon_java") == 0);
    CHECK(r.environmentCaused.count("uncaught_exception_main") == 1);

    // a network failure while Maven downloads dependencies is an environment problem as well
    const auto offline = detect("java",
        "[ERROR] Failed to execute goal: Could not transfer artifact org.postgresql:postgresql:pom:42.7.7: "
        "java.net.UnknownHostException: repo.maven.apache.org\n", true, "", false);
    CHECK(contains(offline.environment, "env_unknown_host"));
    CHECK(!offline.errors.empty());
    CHECK(offline.environmentCaused.size() == offline.errors.size());
}

TEST_CASE("detection: a successful unittest run is no test failure") {
    CHECK(!containsTestFailure("....\n----------------------------------------------------------------------\nRan 4 tests in 0.001s\n\nOK\n"));
    CHECK(!containsTestFailure("Ran into a problem? No."));
    CHECK(containsTestFailure("Ran 4 tests in 0.001s\n\nFAILED (failures=1)\n"));
    CHECK(containsTestFailure("FAILED (errors=2)"));
}

TEST_CASE("checklist: categories learned from the analysis only get safe keywords") {
    const fs::path dir = fs::temp_directory_path() / ("buggy_checklist_" + std::to_string(getpid()));
    fs::create_directories(dir);
    const fs::path before = fs::current_path();
    fs::current_path(dir);
    { std::ofstream("errorchecklist.txt") << "# test\n"; }

    ChecklistReader reader;
    CHECK(reader.appendIfNew("sql_syntax_error", "java", {}));
    CHECK(reader.appendIfNew("timeout", "general", {}));
    CHECK(reader.appendIfNew("race condition", "general", {}));
    std::stringstream text;
    text << std::ifstream("errorchecklist.txt").rdbuf();
    const auto loaded = reader.load();

    fs::current_path(before);
    fs::remove_all(dir);

    CHECK(text.str().find("java | sql_syntax_error | detect_from_output | sql_syntax_error") != std::string::npos);
    CHECK(text.str().find("general | timeout | ai_presume | -") != std::string::npos);
    CHECK(text.str().find("general | race condition | ai_presume | -") != std::string::npos);
    CHECK(loaded.size() == 3);
}
