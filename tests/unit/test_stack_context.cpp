#include "minitest.h"
#include "test_support.h"

#include "FileCollector.h"
#include "StackContext.h"
#include "StackDetector.h"

#include <filesystem>

using support::TempProject;
using support::contains;

namespace {

// Project-relative source files, the same list StackPipeline passes in.
std::vector<std::string> sourceFiles(const std::string& root) {
    FileCollector collector;
    std::vector<std::string> files;
    for (const auto& f : collector.collectSourceFiles(root, true)) {
        files.push_back(std::filesystem::path(f).lexically_relative(root).generic_string());
    }
    return files;
}

CodeError error(const std::string& file, const std::string& message) {
    CodeError e;
    e.file = file;
    e.message = message;
    return e;
}

FixContext selectFor(const std::string& root, const std::string& file, const std::string& message) {
    const ProjectStack stack = StackDetector::detect(root);
    return StackContext::select(stack, sourceFiles(root), {file}, {error(file, message)});
}

}

TEST_CASE("context: identifiers from error messages") {
    CHECK(StackContext::identifiers("error TS2339: Property 'name' does not exist on type 'User'.")
          == (std::vector<std::string>{"name", "User"}));
    CHECK(StackContext::identifiers("ModuleNotFoundError: No module named 'app.modles'")
          == (std::vector<std::string>{"app", "modles"}));
    CHECK(StackContext::identifiers("no such column: full_name") == std::vector<std::string>{"full_name"});
    CHECK(contains(StackContext::identifiers("table users has no column named email_address"), "users"));
    CHECK(contains(StackContext::identifiers("table users has no column named email_address"), "email_address"));
    // unquoted camelCase / snake_case words are picked up, plain words are not
    CHECK(StackContext::identifiers("response is missing userId and created_at fields")
          == (std::vector<std::string>{"userId", "created_at"}));
    CHECK(StackContext::identifiers("syntax error near line 3").empty());
}

TEST_CASE("context: snake_case and camelCase variants") {
    CHECK(StackContext::nameVariants("full_name") == (std::vector<std::string>{"full_name", "fullName"}));
    CHECK(StackContext::nameVariants("fullName") == (std::vector<std::string>{"fullName", "full_name"}));
    CHECK(StackContext::nameVariants("UserProfile") == (std::vector<std::string>{"UserProfile", "user_profile"}));
    CHECK(StackContext::nameVariants("email") == std::vector<std::string>{"email"});
}

TEST_CASE("context: sensitive files") {
    CHECK(StackContext::isSensitiveFile(".env"));
    CHECK(StackContext::isSensitiveFile("backend/.env.production"));
    CHECK(StackContext::isSensitiveFile("deploy/server.pem"));
    CHECK(StackContext::isSensitiveFile("config/secrets.py"));
    CHECK(StackContext::isSensitiveFile("aws_credentials.json"));
    CHECK(!StackContext::isSensitiveFile("backend/app/models.py"));
    CHECK(!StackContext::isSensitiveFile("frontend/src/environment.ts"));
}

TEST_CASE("context: frontend type error pulls in the backend model (API field rename)") {
    const FixContext context = selectFor(support::FIXTURE, "frontend/src/format.ts",
        "error TS2339: Property 'name' does not exist on type 'User'.");

    REQUIRE(!context.files.empty());
    CHECK(context.files.front() == "backend/app/models.py");
    CHECK(!contains(context.files, "db/001_schema.sql"));        // db is not a neighbour of the frontend
    CHECK(context.note.find("'frontend'") != std::string::npos);
    CHECK(context.note.find("'backend'") != std::string::npos);
    CHECK(context.note.find("the failing component depends on it") != std::string::npos);
    CHECK(context.note.find("backend/app/models.py") != std::string::npos);
}

TEST_CASE("context: ORM / schema mismatch in the backend pulls in the migration") {
    const FixContext context = selectFor(support::FIXTURE, "backend/app/models.py",
        "sqlite3.OperationalError: table users has no column named full_name");

    CHECK(contains(context.files, "db/001_schema.sql"));
    // frontend/src/types.ts also declares full_name on User: the other side of the API
    CHECK(contains(context.files, "frontend/src/types.ts"));
    CHECK(context.note.find("it depends on the failing component") != std::string::npos);
}

TEST_CASE("context: a failing test gets the code under test, not other components") {
    const FixContext context = selectFor(support::FIXTURE, "backend/tests/test_utils.py",
        "AssertionError: assert 'hello_world' == 'hello-world'");

    CHECK(context.files == std::vector<std::string>{"backend/app/utils.py"});
    CHECK(context.note.empty());              // nothing crosses a component boundary
}

TEST_CASE("context: JS/TS test files get their relative imports") {
    const FixContext context = selectFor(support::FIXTURE, "frontend/src/format.test.ts",
        "formatUser shows the full name and the email: AssertionError: expected 'undefined <ada@example.com>'");
    CHECK(contains(context.files, "frontend/src/format.ts"));
}

TEST_CASE("context: no context without matching names or without neighbours") {
    // syntax error in a migration: no name that another component mentions
    const FixContext sql = selectFor(support::FIXTURE, "db/002_orders.sql", "near \"TABEL\": syntax error");
    CHECK(sql.files.empty());
    CHECK(sql.note.empty());

    // a weak single match ("app" in a comment) must not pull in an unrelated file
    const FixContext import = selectFor(support::FIXTURE, "backend/app/main.py",
        "ModuleNotFoundError: No module named 'app.modles'");
    CHECK(import.files.empty());

    // single component project: nothing on the other side
    TempProject p;
    p.write("requirements.txt");
    p.write("app.py", "class User: pass\n");
    p.write("models.py", "class User: pass\n");
    const FixContext single = selectFor(p.path(), "app.py", "NameError: name 'User' is not defined");
    CHECK(single.files.empty());
}

TEST_CASE("context: size budget, read-only excerpts, tests and secrets excluded") {
    TempProject p;
    p.write("backend/requirements.txt");
    p.write("frontend/package.json", "{}");
    p.write("frontend/src/view.ts", "export const x = user.displayName;\n");

    // too large to send in full: must become an excerpt, never a FILE block
    std::string large = "# generated API schema\n";
    while (large.size() < StackContext::MAX_FILE_CHARS + 2000) large += "filler = 'lorem ipsum dolor sit amet'\n";
    large += "class UserSchema:\n    display_name: str\n";
    p.write("backend/app/schemas.py", large);

    p.write("backend/app/models.py", "class User:\n    display_name = ''\n");
    p.write("backend/tests/test_models.py", "def test_display_name(): assert User.display_name == ''\n");
    p.write("backend/secrets.py", "display_name = 'do not send'\n");
    p.write("backend/.env", "DISPLAY_NAME=secret\n");

    const FixContext context = selectFor(p.path(), "frontend/src/view.ts",
        "error TS2339: Property 'displayName' does not exist on type 'User'.");

    CHECK(contains(context.files, "backend/app/models.py"));
    CHECK(!contains(context.files, "backend/app/schemas.py"));
    CHECK(!contains(context.files, "backend/tests/test_models.py"));
    CHECK(!contains(context.files, "backend/secrets.py"));
    CHECK(context.files.size() <= StackContext::MAX_RELATED_FILES);

    CHECK(context.note.find("Read-only excerpts") != std::string::npos);
    CHECK(context.note.find("backend/app/schemas.py") != std::string::npos);
    CHECK(context.note.find("display_name: str") != std::string::npos);     // the relevant lines
    CHECK(context.note.find("do not send") == std::string::npos);
    CHECK(context.note.find("secret") == std::string::npos);
    CHECK(context.note.size() < StackContext::MAX_EXCERPT_CHARS * StackContext::MAX_RELATED_FILES + 2000);

    // total size of the files sent in full stays inside the budget
    size_t total = 0;
    for (const auto& f : context.files) total += std::filesystem::file_size(std::filesystem::path(p.path()) / f);
    CHECK(total <= StackContext::MAX_TOTAL_CHARS);
}
