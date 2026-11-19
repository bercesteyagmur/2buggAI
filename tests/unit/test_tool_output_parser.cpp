#include "minitest.h"
#include "test_support.h"

#include "ToolOutputParser.h"

using support::errorIn;
using support::toolOutput;

namespace {
const std::string BACKEND = support::FIXTURE + "/backend";
const std::string FRONTEND = support::FIXTURE + "/frontend";
const std::string DB = support::FIXTURE + "/db";

std::vector<CodeError> parse(const std::string& tool, const std::string& sample, const std::string& componentRoot) {
    return ToolOutputParser::parse(tool, toolOutput(sample), componentRoot, support::FIXTURE);
}
}

TEST_CASE("parser: registry maps tools to parsers") {
    CHECK(ToolOutputParser::parserFor("pytest") == "pytest");
    CHECK(ToolOutputParser::parserFor("compileall") == "python");
    CHECK(ToolOutputParser::parserFor("ruff") == "lint");
    CHECK(ToolOutputParser::parserFor("mypy") == "lint");
    CHECK(ToolOutputParser::parserFor("vitest") == "jest");
    CHECK(ToolOutputParser::parserFor("alembic") == "sql");
    CHECK(ToolOutputParser::parserFor("my-custom-check") == "generic");
}

TEST_CASE("parser: pytest --tb=short (captured)") {
    const auto errors = parse("pytest", "pytest_short.txt", BACKEND);

    // collection error: innermost project frame first, stdlib frame dropped
    REQUIRE(errors.size() == 3);
    CHECK(errors[0].file == "backend/app/main.py");
    CHECK(errors[0].line == 3);
    CHECK(errors[0].message == "ModuleNotFoundError: No module named 'app.modles'");
    CHECK(errors[0].severity == "error");
    CHECK(errors[0].tool == "pytest");
    CHECK(errors[1].file == "backend/tests/test_api.py");
    CHECK(errors[1].line == 1);
    CHECK(errors[1].message == errors[0].message);

    // failing test
    CHECK(errors[2].file == "backend/tests/test_utils.py");
    CHECK(errors[2].line == 5);
    CHECK(errors[2].message == "AssertionError: assert 'hello_world' == 'hello-world'");
}

TEST_CASE("parser: pytest default long format (captured)") {
    const auto errors = parse("pytest", "pytest_long.txt", BACKEND);
    REQUIRE(errors.size() == 3);
    CHECK(errors[0].file == "backend/app/main.py");
    CHECK(errors[0].line == 3);
    const CodeError* failure = errorIn(errors, "backend/tests/test_utils.py");
    REQUIRE(failure);
    CHECK(failure->line == 5);
    CHECK(failure->message.rfind("AssertionError: assert", 0) == 0);
}

TEST_CASE("parser: pytest summary lines are the fallback without tracebacks") {
    const auto errors = ToolOutputParser::parsePytest(
        "FAILED tests/test_utils.py::test_slugify - AssertionError: nope\n"
        "ERROR tests/test_api.py\n"
        "1 failed, 1 error in 0.08s\n");
    REQUIRE(errors.size() == 2);
    CHECK(errors[0].file == "tests/test_utils.py");
    CHECK(errors[0].line == 0);
    CHECK(errors[0].message == "FAILED tests/test_utils.py - AssertionError: nope");
    CHECK(errors[1].file == "tests/test_api.py");
}

TEST_CASE("parser: Python traceback (captured)") {
    const auto errors = parse("python", "python_traceback.txt", BACKEND);
    // the "<string>" frame of python -c is not a file and is dropped
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].file == "backend/app/main.py");
    CHECK(errors[0].line == 3);
    CHECK(errors[0].message == "ModuleNotFoundError: No module named 'app.modles'");
}

TEST_CASE("parser: compileall syntax error (captured)") {
    // app/broken.py is not part of the fixture, so check the raw parser
    const auto errors = ToolOutputParser::parsePython(toolOutput("python_compileall.txt"));
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].file == "app/broken.py");
    CHECK(errors[0].line == 1);
    CHECK(errors[0].message == "SyntaxError: invalid syntax");
}

TEST_CASE("parser: chained tracebacks give one error per exception") {
    const auto errors = ToolOutputParser::parsePython(
        "Traceback (most recent call last):\n"
        "  File \"a.py\", line 2, in load\n"
        "    return data[key]\n"
        "KeyError: 'DATABASE_URL'\n"
        "\n"
        "During handling of the above exception, another exception occurred:\n"
        "\n"
        "Traceback (most recent call last):\n"
        "  File \"b.py\", line 9, in <module>\n"
        "    main()\n"
        "  File \"a.py\", line 4, in load\n"
        "    raise RuntimeError(\"missing setting\")\n"
        "RuntimeError: missing setting\n");
    REQUIRE(errors.size() == 3);
    CHECK(errors[0].file == "a.py");
    CHECK(errors[0].message == "KeyError: 'DATABASE_URL'");
    CHECK(errors[1].file == "a.py");            // innermost frame of the second exception
    CHECK(errors[1].line == 4);
    CHECK(errors[1].message == "RuntimeError: missing setting");
    CHECK(errors[2].file == "b.py");
}

TEST_CASE("parser: flake8 (captured)") {
    const auto errors = ToolOutputParser::parseLint(toolOutput("flake8.txt"));
    REQUIRE(errors.size() == 5);
    CHECK(errors[1].file == "app/lint_me.py");
    CHECK(errors[1].line == 5);
    CHECK(errors[1].column == 26);
    CHECK(errors[1].message == "F821 undefined name 'undefined_bonus'");
    CHECK(errors[1].severity == "error");

    // only files that exist in the project survive path resolution
    const auto resolved = parse("flake8", "flake8.txt", BACKEND);
    REQUIRE(resolved.size() == 3);
    CHECK(resolved[0].file == "backend/app/models.py");
    CHECK(resolved[0].tool == "flake8");
}

TEST_CASE("parser: ruff concise (captured)") {
    const auto errors = ToolOutputParser::parseLint(toolOutput("ruff_concise.txt"));
    REQUIRE(errors.size() == 2);            // "Found 2 errors." is not a location
    CHECK(errors[1].line == 5);
    CHECK(errors[1].column == 26);
    CHECK(errors[1].message == "F821 Undefined name `undefined_bonus`");
}

TEST_CASE("parser: mypy (captured)") {
    const auto errors = ToolOutputParser::parseLint(toolOutput("mypy.txt"));
    REQUIRE(errors.size() == 2);
    CHECK(errors[1].file == "app/lint_me.py");
    CHECK(errors[1].line == 9);
    CHECK(errors[1].column == 0);
    CHECK(errors[1].message == "error: Incompatible return value type (got \"int\", expected \"str\")  [return-value]");
    CHECK(errors[1].severity == "error");

    const auto notes = ToolOutputParser::parseLint("a.py:3: note: See https://mypy.readthedocs.io\n"
                                                   "a.py:4:1: W605 invalid escape sequence\n");
    REQUIRE(notes.size() == 1);
    CHECK(notes[0].severity == "warning");
}

TEST_CASE("parser: tsc --pretty false (hand-written sample)") {
    const auto errors = parse("tsc", "tsc.txt", FRONTEND);
    REQUIRE(errors.size() == 2);
    CHECK(errors[0].file == "frontend/src/format.ts");
    CHECK(errors[0].line == 4);
    CHECK(errors[0].column == 18);
    CHECK(errors[0].message == "error TS2339: Property 'name' does not exist on type 'User'.");
    CHECK(errors[0].severity == "error");
    CHECK(errors[0].tool == "tsc");
    CHECK(errors[1].file == "frontend/src/format.test.ts");
    CHECK(errors[1].line == 6);
}

TEST_CASE("parser: tsc pretty format (hand-written sample)") {
    const auto errors = parse("tsc", "tsc_pretty.txt", FRONTEND);
    REQUIRE(errors.size() == 1);            // "Found 1 error in src/format.ts:4" is not an error line
    CHECK(errors[0].file == "frontend/src/format.ts");
    CHECK(errors[0].line == 4);
    CHECK(errors[0].column == 18);
}

TEST_CASE("parser: eslint stylish (hand-written sample)") {
    const auto errors = parse("eslint", "eslint_stylish.txt", FRONTEND);
    REQUIRE(errors.size() == 3);
    CHECK(errors[0].file == "frontend/src/format.ts");
    CHECK(errors[0].line == 4);
    CHECK(errors[0].column == 13);
    CHECK(errors[0].message == "'usr' is not defined (no-undef)");
    CHECK(errors[0].severity == "error");
    CHECK(errors[1].severity == "warning");
    CHECK(errors[1].message == "Unexpected console statement (no-console)");
    CHECK(errors[2].file == "frontend/src/types.ts");
}

TEST_CASE("parser: eslint unix format (hand-written sample)") {
    const auto errors = parse("eslint", "eslint_unix.txt", FRONTEND);
    REQUIRE(errors.size() == 2);
    CHECK(errors[0].line == 4);
    CHECK(errors[0].message == "'usr' is not defined. (no-undef)");
    CHECK(errors[1].severity == "warning");
}

TEST_CASE("parser: vitest (hand-written sample)") {
    const auto errors = parse("vitest", "vitest.txt", FRONTEND);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].file == "frontend/src/format.test.ts");
    CHECK(errors[0].line == 6);
    CHECK(errors[0].column == 28);
    CHECK(errors[0].message.find("formatUser shows the full name and the email") != std::string::npos);
    CHECK(errors[0].message.find("AssertionError: expected 'undefined <ada@example.com>'") != std::string::npos);
}

TEST_CASE("parser: jest (hand-written sample)") {
    const auto errors = parse("jest", "jest.txt", FRONTEND);
    // the node_modules frame is dropped
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].file == "frontend/src/format.test.ts");
    CHECK(errors[0].line == 6);
    CHECK(errors[0].message == "formatUser shows the full name and the email: expect(received).toBe(expected) // Object.is equality");
}

TEST_CASE("parser: jest failure without a usable location still names the file") {
    const auto errors = ToolOutputParser::parseJest("FAIL src/api.test.js\n  ● Test suite failed to run\n\n    Cannot find module './api'\n");
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].file == "src/api.test.js");
    CHECK(errors[0].line == 0);
}

TEST_CASE("parser: SQL dry run (captured)") {
    const auto errors = parse("sql", "sql_dry_run.txt", DB);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].file == "db/002_orders.sql");
    CHECK(errors[0].line == 2);
    CHECK(errors[0].message == "near \"TABEL\": syntax error");
    CHECK(errors[0].severity == "error");
}

TEST_CASE("parser: sqlfluff (captured)") {
    const auto errors = ToolOutputParser::parse("sqlfluff", toolOutput("sqlfluff.txt"), support::FIXTURE, support::FIXTURE);
    REQUIRE(errors.size() == 1);            // continuation lines of the message are ignored
    CHECK(errors[0].file == "db/002_orders.sql");
    CHECK(errors[0].line == 2);
    CHECK(errors[0].column == 1);
    CHECK(errors[0].severity == "error");
    CHECK(errors[0].message.rfind("PRS: Line 2, Position 1: Found unparsable section", 0) == 0);
}

TEST_CASE("parser: files outside the project and dependencies are dropped") {
    support::TempProject p;
    p.write("app/main.py");
    p.write("node_modules/lib/index.ts");
    p.write(".venv/lib/python3.12/site-packages/flask/app.py");

    const std::string output =
        "Traceback (most recent call last):\n"
        "  File \"/usr/lib/python3.12/runpy.py\", line 198, in _run_module_as_main\n"
        "  File \"" + p.path() + "/.venv/lib/python3.12/site-packages/flask/app.py\", line 7, in run\n"
        "  File \"app/main.py\", line 12, in <module>\n"
        "  File \"../../etc/passwd\", line 1, in <module>\n"
        "ValueError: boom\n";
    const auto errors = ToolOutputParser::parse("python", output, p.path(), p.path());
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].file == "app/main.py");

    const auto ts = ToolOutputParser::parse("tsc", "node_modules/lib/index.ts(1,1): error TS1005: ';' expected.\n", p.path(), p.path());
    CHECK(ts.empty());
}

TEST_CASE("parser: unknown tools use the generic parser") {
    support::TempProject p;
    p.write("scripts/check.py");
    const auto errors = ToolOutputParser::parse("contract", "scripts/check.py:7:3: response field 'full_name' missing\n", p.path(), p.path());
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].file == "scripts/check.py");
    CHECK(errors[0].line == 7);
    CHECK(errors[0].tool == "contract");
}
