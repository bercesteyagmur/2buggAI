#include "minitest.h"
#include "test_support.h"

#include "ChecklistReader.h"
#include "ErrorCollector.h"
#include "ErrorMatcher.h"
#include "WebPipeline.h"

using support::contains;
using support::toolOutput;

namespace {
// Matches against the real errorchecklist.txt of the repository (cwd = repo root).
std::vector<std::string> match(const std::string& output, const std::string& language) {
    ChecklistReader reader;
    ErrorMatcher matcher(reader.load());
    return matcher.match(output, language);
}
}

TEST_CASE("checklist: Python import error and failing test from pytest output") {
    const auto errors = match(toolOutput("pytest_short.txt"), "python");
    CHECK(contains(errors, "import_error"));
    CHECK(contains(errors, "assertion_error"));
    CHECK(contains(errors, "pytest_collection_error"));
}

TEST_CASE("checklist: TypeScript errors") {
    const auto errors = match(toolOutput("tsc.txt"), "typescript");
    CHECK(contains(errors, "ts_property_missing"));     // TS2339
    CHECK(contains(errors, "ts_type_mismatch"));        // TS2345

    CHECK(contains(match("a.ts(1,1): error TS2353: Object literal may only specify known properties", "typescript"),
                   "ts_unknown_property"));
    CHECK(contains(match("a.ts(1,1): error TS2739: Type '{}' is missing the following properties from type 'User': id", "typescript"),
                   "ts_missing_properties"));
    CHECK(contains(match("a.ts(1,1): error TS7006: Parameter 'x' implicitly has an 'any' type.", "typescript"),
                   "ts_implicit_any"));
}

TEST_CASE("checklist: failing migrations and schema / ORM mismatches") {
    const auto dryRun = match(toolOutput("sql_dry_run.txt"), "sql");
    CHECK(contains(dryRun, "sql_migration_failed"));
    CHECK(contains(dryRun, "syntax_error"));             // existing general entry still applies

    CHECK(contains(match("db/003.sql:1: error: table users already exists", "sql"), "sql_object_exists"));
    CHECK(contains(match("db/003.sql:4: error: foreign key mismatch - \"orders\" referencing \"users\"", "sql"),
                   "sql_foreign_key_error"));
    CHECK(contains(match("sqlalchemy.exc.OperationalError: (sqlite3.OperationalError) table users has no column named full_name", "sql"),
                   "orm_schema_mismatch"));
    CHECK(contains(match("db/003.sql:2: error: no such column: fullname", "sql"), "sql_column_not_found"));
}

TEST_CASE("checklist: missing environment variables and API contract errors") {
    CHECK(contains(match("RuntimeError: environment variable DATABASE_URL is required", "python"), "missing_env_var"));
    CHECK(contains(match("    return os.environ[\"SECRET_KEY\"]\nKeyError: 'SECRET_KEY'", "python"), "missing_env_var"));
    CHECK(contains(match("fastapi.exceptions.ResponseValidationError: 1 validation errors: Field required", "python"),
                   "api_contract_mismatch"));
    CHECK(contains(match("POST /api/users -> 422 Unprocessable Entity", "typescript"), "api_contract_mismatch"));
}

TEST_CASE("checklist: Python lint findings and JS test / eslint failures") {
    CHECK(contains(match(toolOutput("flake8.txt"), "python"), "undefined_name_python"));
    CHECK(contains(match(toolOutput("mypy.txt"), "python"), "mypy_type_error"));
    CHECK(contains(match("ImportError: cannot import name 'db' from partially initialized module 'app' (most likely due to a circular import)", "python"),
                   "python_circular_import"));
    CHECK(contains(match(toolOutput("vitest.txt"), "javascript"), "js_test_failed"));
    CHECK(contains(match(toolOutput("jest.txt"), "javascript"), "js_test_failed"));
    CHECK(contains(match(toolOutput("eslint_stylish.txt"), "javascript"), "eslint_error"));
}

TEST_CASE("checklist: environment problems are recognised and never sent to the model") {
    CHECK(contains(match("ERROR: No matching distribution found for flask==99", "python"), "pip_install_failed"));
    CHECK(contains(match("npm error code E404", "javascript"), "npm_install_failed"));
    CHECK(contains(match("command timed out after 300 seconds", "python"), "command_timeout"));

    CHECK(WebPipeline::isEnvironmentError("pip_install_failed"));
    CHECK(WebPipeline::isEnvironmentError("npm_install_failed"));
    CHECK(WebPipeline::isEnvironmentError("command_timeout"));
    CHECK(WebPipeline::isEnvironmentError("db_connection_failed"));      // existing entry unchanged
    CHECK(!WebPipeline::isEnvironmentError("import_error"));
}

TEST_CASE("checklist: new entries do not fire for other languages") {
    CHECK(!contains(match(toolOutput("pytest_short.txt"), "typescript"), "pytest_collection_error"));
    CHECK(!contains(match(toolOutput("tsc.txt"), "python"), "ts_property_missing"));
    CHECK(match("all good", "python").empty());
}

TEST_CASE("checklist: an error name maps back to the files of its tool output") {
    ChecklistReader reader;
    const auto checklist = reader.load();

    std::vector<CodeError> errors(2);
    errors[0].file = "backend/app/main.py";
    errors[0].message = "ModuleNotFoundError: No module named 'app.modles'";
    errors[0].step = "backend: pytest";
    errors[1].file = "db/002_orders.sql";
    errors[1].message = "near \"TABEL\": syntax error";
    errors[1].step = "db: sql";

    const std::string output =
        "===== STEP: backend: pytest | exit code 1 =====\nE   ModuleNotFoundError: No module named 'app.modles'\n"
        "===== STEP: db: sql | exit code 1 =====\n002_orders.sql:2: error: near \"TABEL\": syntax error\n"
        "migration stopped at 002_orders.sql; later files were not applied\n";

    CHECK(WebPipeline::filesForError("import_error", errors, checklist, output)
          == std::vector<std::string>{"backend/app/main.py"});
    // no keyword in the message: falls back to the files of the step that printed it
    CHECK(WebPipeline::filesForError("sql_migration_failed", errors, checklist, output)
          == std::vector<std::string>{"db/002_orders.sql"});
}

TEST_CASE("checklist: Java JAR without Main-Class and missing runtime dependencies") {
    CHECK(contains(match("no main manifest attribute, in /p/target/app-1.0-SNAPSHOT.jar", "java"),
                   "missing_main_manifest"));
    CHECK(contains(match("Exception in thread \"main\" java.lang.NoClassDefFoundError: com/fasterxml/jackson/databind/ObjectMapper", "java"),
                   "class_not_found"));
    CHECK(contains(match("Caused by: java.lang.ClassNotFoundException: org.postgresql.Driver", "java"),
                   "class_not_found"));
    // JDBC driver not packaged into the JAR (Maven dependency only on the build classpath)
    const auto noDriver = match(
        "Exception in thread \"main\" database.DataAccessException: Datenbankverbindungsaufbau nicht erfolgreich\n"
        "Caused by: java.sql.SQLException: No suitable driver found for jdbc:postgresql://localhost:5432/db", "java");
    CHECK(contains(noDriver, "jdbc_driver_missing"));
    CHECK(contains(noDriver, "uncaught_exception_main"));
    // a successful Maven build and server start must not produce these categories
    const auto ok = match("[INFO] Tests run: 33, Failures: 0, Errors: 0, Skipped: 0\n[INFO] BUILD SUCCESS\nServer started", "java");
    CHECK(!contains(ok, "missing_main_manifest"));
    CHECK(!contains(ok, "class_not_found"));
    CHECK(!contains(ok, "jdbc_driver_missing"));
    CHECK(!contains(ok, "uncaught_exception_main"));
}
