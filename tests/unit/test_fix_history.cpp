#include "minitest.h"

#include "ErrorCollector.h"
#include "FixHistory.h"

#include <string>
#include <vector>

namespace {

const std::vector<std::string> kFiles = {
    "/p/src/main/java/Main.java",
    "/p/src/main/java/database/DatabaseManager.java",
    "/p/src/main/java/database/UnitOfWork.java",
    "/p/src/main/java/restserver/server/Server.java",
    "/p/pom.xml",
};

// Real outputs of a run on a Maven project (JAR without its dependencies).
const std::string kNoDriver =
    "\x1B[1;34mINFO\x1B[m] BUILD SUCCESS\n"
    "[INFO] Tests run: 33, Failures: 0, Errors: 0, Skipped: 0\n"
    "Exception in thread \"main\" database.DataAccessException: Datenbankverbindungsaufbau nicht erfolgreich\n"
    "\tat database.DatabaseManager.getConnection(DatabaseManager.java:18)\n"
    "\tat database.UnitOfWork.<init>(UnitOfWork.java:11)\n"
    "\tat restserver.server.Server.start(Server.java:24)\n"
    "\tat Main.main(Main.java:19)\n"
    "Caused by: java.sql.SQLException: No suitable driver found for jdbc:postgresql://localhost:5432/swendb\n"
    "\tat java.sql/java.sql.DriverManager.getConnection(DriverManager.java:708)\n"
    "\tat database.DatabaseManager.getConnection(DatabaseManager.java:13)\n"
    "\t... 3 more\n";

// After a Class.forName "fix" in DatabaseManager.java: other message, same crash.
const std::string kClassNotFound =
    "Exception in thread \"main\" java.lang.ExceptionInInitializerError\n"
    "\tat database.UnitOfWork.<init>(UnitOfWork.java:11)\n"
    "\tat restserver.server.Server.start(Server.java:24)\n"
    "\tat Main.main(Main.java:19)\n"
    "Caused by: java.lang.RuntimeException: Failed to load PostgreSQL JDBC driver\n"
    "\tat database.DatabaseManager.<clinit>(DatabaseManager.java:15)\n"
    "Caused by: java.lang.ClassNotFoundException: org.postgresql.Driver\n"
    "\tat java.base/jdk.internal.loader.BuiltinClassLoader.loadClass(BuiltinClassLoader.java:641)\n";

const std::string kNoManifest = "no main manifest attribute, in /p/target/app.jar\n";
const std::string kServerRuns = "Echo Server läuft auf http://localhost:8080\nServer started\n";

} // namespace

TEST_CASE("fix history: project frames of a Java stack trace, innermost first, JDK frames dropped") {
    const auto frames = FixHistory::projectFrames(kNoDriver, kFiles);
    CHECK(frames.size() == 5);
    CHECK(frames[0].file == "DatabaseManager.java" && frames[0].line == 18);
    CHECK(frames[1].file == "UnitOfWork.java" && frames[1].line == 11);
    CHECK(frames[4].file == "DatabaseManager.java" && frames[4].line == 13);
}

TEST_CASE("fix history: Python traceback frames are reversed to innermost first") {
    const std::string tb =
        "Traceback (most recent call last):\n"
        "  File \"/p/app.py\", line 10, in <module>\n"
        "    main()\n"
        "  File \"/p/service.py\", line 4, in main\n"
        "  File \"/usr/lib/python3.12/json/__init__.py\", line 346, in loads\n"
        "ValueError: boom\n";
    const auto frames = FixHistory::projectFrames(tb, {"/p/app.py", "/p/service.py"});
    CHECK(frames.size() == 2);
    CHECK(frames[0].file == "service.py" && frames[0].line == 4);
    CHECK(FixHistory::crashSite(tb, {"/p/app.py", "/p/service.py"}, "/p/service.py") == "app.py:10");
}

TEST_CASE("fix history: crash site skips the changed file") {
    CHECK(FixHistory::crashSite(kNoDriver, kFiles, "/p/src/main/java/database/DatabaseManager.java") == "UnitOfWork.java:11");
    CHECK(FixHistory::crashSite(kClassNotFound, kFiles, "/p/src/main/java/database/DatabaseManager.java") == "UnitOfWork.java:11");
    CHECK(FixHistory::crashSite(kNoDriver, kFiles, "/p/pom.xml") == "DatabaseManager.java:18");
    CHECK(FixHistory::crashSite(kNoManifest, kFiles, "/p/pom.xml").empty());
    CHECK(FixHistory::crashSite(kServerRuns, kFiles, "/p/pom.xml").empty());
}

TEST_CASE("fix history: a workaround that only renames the crash is detected") {
    // Class.forName in DatabaseManager: jdbc_driver_missing is gone, class_not_found is new,
    // the program still dies in UnitOfWork.java:11
    CHECK(FixHistory::onlyTransformed({"jdbc_driver_missing", "uncaught_exception_main"},
                                      {"class_not_found", "uncaught_exception_main"},
                                      kNoDriver, kClassNotFound, kFiles,
                                      "/p/src/main/java/database/DatabaseManager.java"));
}

TEST_CASE("fix history: real fixes are not taken for a transformation") {
    // pom.xml gets a Main-Class: the next error appears, but there was no crash site before
    CHECK(!FixHistory::onlyTransformed({"missing_main_manifest"}, {"jdbc_driver_missing", "uncaught_exception_main"},
                                       kNoManifest, kNoDriver, kFiles, "/p/pom.xml"));
    // dependencies packaged into the JAR: the server runs, nothing new appears
    CHECK(!FixHistory::onlyTransformed({"jdbc_driver_missing", "uncaught_exception_main"}, {},
                                       kNoDriver, kServerRuns, kFiles, "/p/pom.xml"));
    // the error is gone and the program fails later for another reason, but no new category
    CHECK(!FixHistory::onlyTransformed({"jdbc_driver_missing", "uncaught_exception_main"}, {"uncaught_exception_main"},
                                       kNoDriver, kClassNotFound, kFiles, "/p/pom.xml"));
    // a new category appears, but nothing vanished: not a renamed crash
    CHECK(!FixHistory::onlyTransformed({"uncaught_exception_main"}, {"uncaught_exception_main", "class_not_found"},
                                       kNoDriver, kClassNotFound, kFiles,
                                       "/p/src/main/java/database/DatabaseManager.java"));
}

TEST_CASE("fix history: failure fingerprint tells 'no effect' from 'progress'") {
    // the same failure after code moved by one line: same fingerprint (no effect)
    const std::string before =
        "Traceback (most recent call last):\n  File \"/p/main.py\", line 4, in <module>\n"
        "    assert double(2) == 4, f\"double: got {double(2)}\"\nAssertionError: double: got 5\n";
    const std::string moved =
        "Traceback (most recent call last):\n  File \"/p/main.py\", line 5, in <module>\n"
        "    assert double(2) == 4, f\"double: got {double(2)}\"\nAssertionError: double: got 5\n";
    CHECK(FixHistory::failureFingerprint(before) == FixHistory::failureFingerprint(moved));
    CHECK(FixHistory::failureFingerprint(kNoDriver) ==
          FixHistory::failureFingerprint(std::string(kNoDriver).replace(kNoDriver.find(":18)"), 4, ":19)")));
    // gdb: other addresses, same crash
    CHECK(FixHistory::failureFingerprint("Program received signal SIGSEGV, Segmentation fault.\n0x0000555555555161 in main () at crash.c:6\n")
       == FixHistory::failureFingerprint("Program received signal SIGSEGV, Segmentation fault.\n0x0000555555555171 in main () at crash.c:7\n"));

    // the next failing assert: different fingerprint (progress)
    const std::string next =
        "Traceback (most recent call last):\n  File \"/p/main.py\", line 5, in <module>\n"
        "    assert square(3) == 9, f\"square: got {square(3)}\"\nAssertionError: square: got 10\n";
    CHECK(FixHistory::failureFingerprint(before) != FixHistory::failureFingerprint(next));
    CHECK(FixHistory::failureFingerprint(kNoDriver) != FixHistory::failureFingerprint(kClassNotFound));
}

TEST_CASE("fix history: the catch-all crash category is worked on last") {
    ErrorCollector collector;
    const auto sorted = collector.sortedErrors(
        {"uncaught_exception_main", "out_of_bounds_access", "array_index_out_of_bounds"});
    CHECK(sorted.size() == 3);
    CHECK(sorted.back() == "uncaught_exception_main");
}

TEST_CASE("fix history: error excerpt keeps the error lines and drops Maven noise and colours") {
    const std::string ex = FixHistory::errorExcerpt(kNoDriver);
    CHECK(ex.find("No suitable driver found") != std::string::npos);
    CHECK(ex.find("Exception in thread \"main\"") != std::string::npos);
    CHECK(ex.find("BUILD SUCCESS") == std::string::npos);
    CHECK(ex.find("Errors: 0") == std::string::npos);
    CHECK(ex.find('\x1B') == std::string::npos);
    // no error line at all: the last lines are used
    CHECK(FixHistory::errorExcerpt(kServerRuns).find("Server started") != std::string::npos);
}

TEST_CASE("fix history: attempt description for the next request") {
    const std::string d = FixHistory::describeAttempt(2, "/p/src/main/java/database/DatabaseManager.java",
        "Load the driver with Class.forName.", "error still present", kClassNotFound);
    CHECK(d.rfind("- Attempt 2: changed /p/src/main/java/database/DatabaseManager.java", 0) == 0);
    CHECK(d.find("Change: Load the driver with Class.forName.") != std::string::npos);
    CHECK(d.find("Result: error still present.") != std::string::npos);
    CHECK(d.find("ClassNotFoundException: org.postgresql.Driver") != std::string::npos);

    const std::string notApplied = FixHistory::describeAttempt(1, "", "", "not applied (Verifier rejected)", "");
    CHECK(notApplied == "- Attempt 1: no file changed. Result: not applied (Verifier rejected).\n");
}
