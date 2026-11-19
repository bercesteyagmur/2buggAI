#include "minitest.h"
#include "test_support.h"

#include "StackDetector.h"
#include "StackPipeline.h"
#include "StackToolchain.h"

#include <filesystem>
#include <iostream>
#include <sstream>

using support::TempProject;
using support::contains;

namespace {

const StackCommand* commandFor(const std::vector<StackCommand>& commands, const std::string& tool) {
    for (const auto& c : commands) {
        if (c.tool == tool) return &c;
    }
    return nullptr;
}

// run_capture echoes the output of every command; keep the test report readable
struct QuietStdout {
    std::streambuf* old = std::cout.rdbuf(sink.rdbuf());
    std::ostringstream sink;
    ~QuietStdout() { std::cout.rdbuf(old); }
};

}

TEST_CASE("toolchain: step names") {
    CHECK(StackToolchain::stepName("backend", "pytest") == "backend: pytest");
    std::string component, tool;
    REQUIRE(StackToolchain::splitStepName("frontend: npm install", component, tool));
    CHECK(component == "frontend");
    CHECK(tool == "npm install");
    CHECK(!StackToolchain::splitStepName("composer install", component, tool));
}

TEST_CASE("toolchain: a fix re-checks the changed component and its dependents") {
    const StackToolchain toolchain(StackDetector::detect(support::FIXTURE));

    // schema change: backend uses the schema, frontend uses the backend
    CHECK(toolchain.affectedComponents("db/002_orders.sql") == (std::vector<std::string>{"db", "backend", "frontend"}));
    // backend change: frontend depends on the API, the schema does not
    CHECK(toolchain.affectedComponents("backend/app/main.py") == (std::vector<std::string>{"backend", "frontend"}));
    // frontend change: nothing depends on it
    CHECK(toolchain.affectedComponents("frontend/src/format.ts") == std::vector<std::string>{"frontend"});
    // unknown file or initial run: everything
    CHECK(toolchain.affectedComponents("README.md").size() == 3);
    CHECK(toolchain.affectedComponents("").size() == 3);
}

TEST_CASE("toolchain: default commands of the fixture components") {
    const ProjectStack stack = StackDetector::detect(support::FIXTURE);
    const StackToolchain toolchain(stack);

    const auto backend = toolchain.commandsFor(*stack.find("backend"), "check");
    REQUIRE(commandFor(backend, "compileall"));
    CHECK(commandFor(backend, "compileall")->command.find("-m compileall -q") != std::string::npos);
    REQUIRE(commandFor(backend, "pytest"));       // the fixture has tests
    CHECK(commandFor(backend, "pytest")->command.find("--continue-on-collection-errors") != std::string::npos);

    const auto setup = toolchain.commandsFor(*stack.find("backend"), "setup");
    REQUIRE(commandFor(setup, "pip"));
    CHECK(commandFor(setup, "pip")->command.find("-r requirements.txt") != std::string::npos);
    CHECK(commandFor(setup, "pip")->command.find("sudo") == std::string::npos);

    const auto frontend = toolchain.commandsFor(*stack.find("frontend"), "check");
    REQUIRE(commandFor(frontend, "tsc"));
    REQUIRE(commandFor(frontend, "vitest"));       // "test": "vitest run"
    CHECK(commandFor(frontend, "vitest")->command == "npm run test");

    // migrations in file-name order, executed by the in-memory dry run
    const auto db = toolchain.commandsFor(*stack.find("db"), "check");
    REQUIRE(db.size() == 1);
    CHECK(db[0].tool == "sql");
    CHECK(db[0].command.find("'001_schema.sql' '002_orders.sql'") != std::string::npos);
    CHECK(db[0].skipReason.empty());
    CHECK(toolchain.commandsFor(*stack.find("db"), "setup").empty());
}

TEST_CASE("toolchain: .buggai.json overrides replace, add and disable commands") {
    TempProject p;
    p.write("api/requirements.txt", "flask\n");
    p.write("api/tests/test_x.py", "def test_x(): pass\n");
    p.write("db/001.sql", "CREATE TABLE t (id INT);\n");
    p.write("db/001.down.sql", "DROP TABLE t;\n");
    p.write(".buggai.json", R"({
        "commands": {
            "python": {"compileall": false, "pytest": "{python} -m pytest -x"},
            "api": {"contract": "python3 scripts/check_contract.py"},
            "sql": {"sql": false}
        }
    })");

    const ProjectStack stack = StackDetector::detect(p.path());
    const StackToolchain toolchain(stack);

    const auto api = toolchain.commandsFor(*stack.find("api"), "check");
    CHECK(commandFor(api, "compileall") == nullptr);                    // disabled for the kind
    REQUIRE(commandFor(api, "pytest"));
    CHECK(commandFor(api, "pytest")->command == "python3 -m pytest -x"); // no venv yet -> python3
    CHECK(commandFor(api, "pytest")->skipReason.empty());                // user command: never skipped
    REQUIRE(commandFor(api, "contract"));                               // added for the component
    CHECK(commandFor(api, "contract")->command == "python3 scripts/check_contract.py");

    CHECK(toolchain.commandsFor(*stack.find("db"), "check").empty());
}

TEST_CASE("toolchain: rollback scripts are not part of the SQL dry run") {
    TempProject p;
    p.write("backend/requirements.txt");
    p.write("frontend/package.json", "{}");
    p.write("db/001_init.sql", "CREATE TABLE t (id INT);\n");
    p.write("db/001_init.down.sql", "DROP TABLE t;\n");
    p.write("db/migrations/002_more.sql", "ALTER TABLE t ADD COLUMN n INT;\n");

    const ProjectStack stack = StackDetector::detect(p.path());
    const StackToolchain toolchain(stack);
    const auto db = toolchain.commandsFor(*stack.find("db"), "check");
    REQUIRE(db.size() == 1);
    CHECK(db[0].command.find("'001_init.sql' 'migrations/002_more.sql'") != std::string::npos);
    CHECK(db[0].command.find("down") == std::string::npos);
}

TEST_CASE("toolchain: other SQL dialects are never executed") {
    TempProject p;
    p.write("docker-compose.yml", "services:\n  db:\n    image: postgres:16\n");
    p.write("db/001.sql", "CREATE EXTENSION pgcrypto;\n");

    const ProjectStack stack = StackDetector::detect(p.path());
    const StackToolchain toolchain(stack);
    const auto db = toolchain.commandsFor(*stack.find("db"), "check");
    REQUIRE(db.size() == 1);
    CHECK(db[0].tool == "sqlfluff");
    CHECK(db[0].command.find("--dialect postgres") != std::string::npos);
    CHECK(db[0].command.find("buggai_sql_check") == std::string::npos);
}

TEST_CASE("toolchain: SQL dry run finds the broken migration and passes after the fix") {
    TempProject p;
    p.write("db/001_schema.sql", support::readFile(support::FIXTURE + "/db/001_schema.sql"));
    p.write("db/002_orders.sql", support::readFile(support::FIXTURE + "/db/002_orders.sql"));

    const ProjectStack stack = StackDetector::detect(p.path());
    StackToolchain toolchain(stack);

    std::vector<SetupStep> steps;
    { QuietStdout quiet; steps = toolchain.check(""); }
    REQUIRE(steps.size() == 1);
    CHECK(steps[0].name == "db: sql");            // component name = folder name
    CHECK(steps[0].result.exit_code == 1);
    CHECK(steps[0].result.output == support::readFile(support::OUTPUTS + "sql_dry_run.txt"));

    // same output through the pipeline's parser: file, line, tool, component
    const auto errors = StackPipeline::parseErrors(WebPipeline::collectErrorOutput(steps), stack);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].file == "db/002_orders.sql");
    CHECK(errors[0].line == 2);
    CHECK(errors[0].tool == "sql");
    CHECK(errors[0].component == "db");

    p.write("db/002_orders.sql", support::replaceAll(support::readFile(support::FIXTURE + "/db/002_orders.sql"), "TABEL", "TABLE"));
    { QuietStdout quiet; steps = toolchain.check("db/002_orders.sql"); }
    REQUIRE(steps.size() == 1);
    CHECK(steps[0].result.exit_code == 0);
    CHECK(steps[0].result.output.find("2 SQL file(s) applied to an in-memory SQLite database") != std::string::npos);
}

TEST_CASE("toolchain: SQL dry run handles triggers, comments and a missing final semicolon") {
    TempProject p;
    p.write("db/001.sql",
        "-- users; with a semicolon in the comment\n"
        "CREATE TABLE users (id INTEGER PRIMARY KEY, note TEXT DEFAULT 'a;b');\n"
        "\n"
        "CREATE TABLE audit (user_id INTEGER);\n"
        "CREATE TRIGGER users_audit AFTER INSERT ON users BEGIN\n"
        "    INSERT INTO audit (user_id) VALUES (new.id);\n"
        "END;\n"
        "INSERT INTO users (id) VALUES (1); INSERT INTO users (id) VALUES (2);\n");
    p.write("db/002.sql", "\n\nSELECT missing_column\nFROM users");

    StackToolchain toolchain(StackDetector::detect(p.path()));
    std::vector<SetupStep> steps;
    { QuietStdout quiet; steps = toolchain.check(""); }
    REQUIRE(steps.size() == 1);
    CHECK(steps[0].result.exit_code == 1);
    // first file is fine; the error is reported at the first line of the failing statement
    CHECK(steps[0].result.output.rfind("002.sql:3: error: no such column: missing_column", 0) == 0);
}

TEST_CASE("toolchain: commands are stopped by the timeout") {
    TempProject p;
    p.write("db/001.sql", "SELECT 1;\n");
    p.write(".buggai.json", R"({"timeout_sec": 1, "commands": {"sql": {"sql": "sleep 30"}}})");

    StackToolchain toolchain(StackDetector::detect(p.path()));
    std::vector<SetupStep> steps;
    { QuietStdout quiet; steps = toolchain.check(""); }
    REQUIRE(steps.size() == 1);
    CHECK(steps[0].result.exit_code == 124);
    CHECK(steps[0].result.output.find("command timed out after 1 seconds") != std::string::npos);
}

TEST_CASE("toolchain: check commands cannot see a configured database URL") {
    TempProject p;
    p.write("db/001.sql", "SELECT 1;\n");
    p.write(".buggai.json", R"({"commands": {"sql": {"sql": "echo url=$DATABASE_URL"}}})");
    setenv("DATABASE_URL", "postgres://prod-user:pw@prod-host/prod", 1);

    StackToolchain toolchain(StackDetector::detect(p.path()));
    std::vector<SetupStep> steps;
    { QuietStdout quiet; steps = toolchain.check(""); }
    unsetenv("DATABASE_URL");
    REQUIRE(steps.size() == 1);
    CHECK(steps[0].result.output == "url=sqlite://\n");
}

TEST_CASE("pipeline: errors carry step, tool, service and component") {
    const ProjectStack stack = StackDetector::detect(support::FIXTURE);
    const std::string output =
        "===== STEP: backend: pytest | exit code 1 =====\n" + support::toolOutput("pytest_short.txt") +
        "===== STEP: frontend: tsc | exit code 2 =====\n" + support::toolOutput("tsc.txt") +
        "===== STEP: db: sql | exit code 1 =====\n" + support::toolOutput("sql_dry_run.txt") +
        "===== STEP: backend: pip | exit code 1 =====\nERROR: No matching distribution found for flask==99\n";

    const auto errors = StackPipeline::parseErrors(output, stack);
    REQUIRE(errors.size() == 6);

    const CodeError* py = support::errorIn(errors, "backend/app/main.py");
    REQUIRE(py);
    CHECK(py->step == "backend: pytest");
    CHECK(py->tool == "pytest");
    CHECK(py->component == "backend");
    CHECK(py->service == "backend");

    const CodeError* ts = support::errorIn(errors, "frontend/src/format.ts");
    REQUIRE(ts);
    CHECK(ts->tool == "tsc");
    CHECK(ts->component == "frontend");
    CHECK(ts->service == "frontend");
    CHECK(ts->line == 4);
    CHECK(ts->column == 18);

    const CodeError* sql = support::errorIn(errors, "db/002_orders.sql");
    REQUIRE(sql);
    CHECK(sql->tool == "sql");
    CHECK(sql->component == "db");
}
