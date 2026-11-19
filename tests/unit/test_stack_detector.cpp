#include "minitest.h"
#include "test_support.h"

#include "StackDetector.h"

using support::TempProject;
using support::contains;

TEST_CASE("detector: fixture project is a three component fullstack repo") {
    const ProjectStack stack = StackDetector::detect(support::FIXTURE);

    REQUIRE(stack.components.size() == 3);
    const StackComponent* backend = stack.find("backend");
    const StackComponent* frontend = stack.find("frontend");
    const StackComponent* db = stack.find("db");
    REQUIRE(backend);
    REQUIRE(frontend);
    REQUIRE(db);

    CHECK(backend->kind == "python");
    CHECK(backend->root == "backend");
    CHECK(backend->service == "backend");
    CHECK(frontend->kind == "node");
    CHECK(frontend->language == "typescript");
    CHECK(frontend->service == "frontend");
    CHECK(db->kind == "sql");
    CHECK(db->root == "db");

    // compose: frontend depends_on backend; heuristic: python code depends on shared SQL
    CHECK(frontend->dependsOn == std::vector<std::string>{"backend"});
    CHECK(backend->dependsOn == std::vector<std::string>{"db"});
    CHECK(db->dependsOn.empty());

    CHECK(stack.composeFile == "docker-compose.yml");
    CHECK(stack.composeServices.size() == 2);
    CHECK(stack.sqlDialect == "sqlite");
    CHECK(stack.isFullstack());
}

TEST_CASE("detector: single service Python project is not fullstack") {
    TempProject p;
    p.write("requirements.txt", "flask\n");
    p.write("app.py", "print('hi')\n");
    p.write("tests/test_app.py", "def test_x(): pass\n");

    const ProjectStack stack = StackDetector::detect(p.path());
    REQUIRE(stack.components.size() == 1);
    CHECK(stack.components[0].kind == "python");
    CHECK(stack.components[0].root == ".");
    CHECK(stack.components[0].name == "python");
    CHECK(!stack.isFullstack());
}

TEST_CASE("detector: single Node project is not fullstack") {
    TempProject p;
    p.write("package.json", "{}");
    p.write("src/index.js", "");

    const ProjectStack stack = StackDetector::detect(p.path());
    REQUIRE(stack.components.size() == 1);
    CHECK(stack.components[0].kind == "node");
    CHECK(stack.components[0].language == "javascript");
    CHECK(!stack.isFullstack());
}

TEST_CASE("detector: multi-service repo with compose, services/ folder and per-service SQL") {
    TempProject p;
    p.write("docker-compose.yml",
        "version: '3.9'\n"
        "services:\n"
        "  users:   # python service\n"
        "    build:\n"
        "      context: ./services/users\n"
        "    depends_on:\n"
        "      postgres:\n"
        "        condition: service_healthy\n"
        "  orders:\n"
        "    build: services/orders\n"
        "    depends_on: [users, postgres]\n"
        "  web:\n"
        "    build: \"./web\"\n"
        "    depends_on:\n"
        "      - orders\n"
        "  postgres:\n"
        "    image: postgres:16\n"
        "volumes:\n"
        "  data:\n");
    p.write("services/users/pyproject.toml", "[project]\nname='users'\n");
    p.write("services/users/migrations/001_init.sql", "CREATE TABLE users (id INT);\n");
    p.write("services/orders/requirements.txt", "fastapi\n");
    p.write("web/package.json", "{}");
    p.write("web/tsconfig.json", "{}");

    const ProjectStack stack = StackDetector::detect(p.path());
    REQUIRE(stack.composeServices.size() == 4);
    CHECK(stack.composeServices[0].name == "users");
    CHECK(stack.composeServices[0].context == "./services/users");
    CHECK(stack.composeServices[0].dependsOn == std::vector<std::string>{"postgres"});
    CHECK(stack.composeServices[1].dependsOn == (std::vector<std::string>{"users", "postgres"}));
    CHECK(stack.composeServices[2].context == "./web");
    CHECK(stack.composeServices[3].image == "postgres:16");

    const StackComponent* users = stack.find("users");
    const StackComponent* orders = stack.find("orders");
    const StackComponent* web = stack.find("web");
    const StackComponent* migrations = stack.find("migrations");
    REQUIRE(users);
    REQUIRE(orders);
    REQUIRE(web);
    REQUIRE(migrations);

    CHECK(users->service == "users");
    CHECK(orders->service == "orders");
    CHECK(web->service == "web");
    CHECK(migrations->kind == "sql");
    CHECK(migrations->service == "users");              // inside the users build context
    CHECK(contains(users->dependsOn, "migrations"));     // owns its SQL
    CHECK(!contains(orders->dependsOn, "migrations"));   // SQL of another service
    CHECK(contains(orders->dependsOn, "users"));         // compose depends_on
    CHECK(web->dependsOn == std::vector<std::string>{"orders"});

    CHECK(stack.sqlDialect == "postgres");               // from the compose image
    CHECK(stack.isFullstack());

    // a schema change reaches every service above it
    const auto dependents = stack.dependentsOf("migrations");
    CHECK(contains(dependents, "users"));
    CHECK(contains(dependents, "orders"));
    CHECK(contains(dependents, "web"));
    CHECK(stack.dependentsOf("web").empty());
}

TEST_CASE("detector: two Python services under services/ without compose") {
    TempProject p;
    p.write("services/a/requirements.txt");
    p.write("services/b/requirements.txt");

    const ProjectStack stack = StackDetector::detect(p.path());
    REQUIRE(stack.components.size() == 2);
    CHECK(stack.components[0].service == "a");
    CHECK(stack.components[1].service == "b");
    CHECK(stack.isFullstack());
}

TEST_CASE("detector: mixed repo without a fullstack layout keeps the legacy path") {
    // C project with a helper script folder and a stray SQL file
    TempProject p;
    p.write("main.c", "int main(void){return 0;}\n");
    p.write("tools/requirements.txt", "requests\n");
    p.write("schema.sql", "CREATE TABLE t (id INT);\n");
    p.write("docs/requirements.txt", "sphinx\n");          // docs/ is ignored
    p.write("node_modules/x/package.json", "{}");          // dependencies are ignored

    const ProjectStack stack = StackDetector::detect(p.path());
    CHECK(stack.components.size() == 2);                   // tools (python) + sql
    CHECK(stack.find("tools") != nullptr);
    CHECK(stack.find("sql") != nullptr);
    CHECK(!stack.isFullstack());
}

TEST_CASE("detector: no match") {
    TempProject p;
    p.write("main.cpp", "int main(){}\n");
    p.write("README.md", "# x\n");

    const ProjectStack stack = StackDetector::detect(p.path());
    CHECK(stack.components.empty());
    CHECK(!stack.isFullstack());
    CHECK(StackDetector::detect(p.path() + "/does-not-exist").components.empty());
}

TEST_CASE("detector: Python and Node in the same folder get distinct names") {
    TempProject p;
    p.write("app/requirements.txt");
    p.write("app/package.json", "{}");
    p.write("app/server.py");
    p.write("app/src/index.ts");

    const ProjectStack stack = StackDetector::detect(p.path());
    REQUIRE(stack.find("app-python"));
    REQUIRE(stack.find("app-node"));
    CHECK(stack.isFullstack());
    // files are assigned by language when components share a root
    CHECK(stack.componentOf("app/server.py")->name == "app-python");
    CHECK(stack.componentOf("app/src/index.ts")->name == "app-node");
}

TEST_CASE("detector: componentOf picks the deepest matching root") {
    const ProjectStack stack = StackDetector::detect(support::FIXTURE);
    CHECK(stack.componentOf("backend/app/main.py")->name == "backend");
    CHECK(stack.componentOf("./frontend/src/format.ts")->name == "frontend");
    CHECK(stack.componentOf("db/002_orders.sql")->name == "db");
    CHECK(stack.componentOf("docker-compose.yml") == nullptr);

    CHECK(stack.neighboursOf("backend") == (std::vector<std::string>{"db", "frontend"}));
    CHECK(stack.neighboursOf("frontend") == std::vector<std::string>{"backend"});
    CHECK(stack.dependentsOf("db") == (std::vector<std::string>{"backend", "frontend"}));
}

TEST_CASE("detector: .buggai.json replaces detection and carries command overrides") {
    TempProject p;
    p.write("api/requirements.txt");
    p.write("ui/package.json", "{}");
    p.write(".buggai.json", R"({
        "timeout_sec": 120,
        "sql_dialect": "mysql",
        "components": [
            {"name": "api", "kind": "python", "root": "./api/", "service": "core"},
            {"name": "ui", "kind": "node", "root": "ui", "depends_on": ["api"]},
            {"name": "evil", "kind": "python", "root": "../outside"},
            {"name": "odd", "kind": "ruby", "root": "x"}
        ],
        "commands": {
            "python": {"ruff": false, "pytest": "{python} -m pytest -x"},
            "api": {"contract": "python scripts/check.py"}
        }
    })");

    const ProjectStack stack = StackDetector::detect(p.path());
    REQUIRE(stack.components.size() == 2);                 // "evil" and "odd" are rejected
    CHECK(stack.find("api")->root == "api");
    CHECK(stack.find("api")->service == "core");
    CHECK(stack.find("ui")->dependsOn == std::vector<std::string>{"api"});
    CHECK(stack.find("ui")->language == "javascript");
    CHECK(!stack.config.error.empty());
    CHECK(stack.config.timeoutSec == 120);
    CHECK(stack.sqlDialect == "mysql");
    CHECK(stack.config.commands.at("python").at("ruff").disabled);
    CHECK(stack.config.commands.at("python").at("pytest").command == "{python} -m pytest -x");
    CHECK(stack.config.commands.at("api").at("contract").command == "python scripts/check.py");
    CHECK(stack.isFullstack());
}

TEST_CASE("detector: stack=legacy in the config switches the stack pipeline off") {
    TempProject p;
    p.write("backend/requirements.txt");
    p.write("frontend/package.json", "{}");
    CHECK(StackDetector::detect(p.path()).isFullstack());

    p.write(".buggai.json", R"({"stack": "legacy"})");
    CHECK(!StackDetector::detect(p.path()).isFullstack());

    p.write(".buggai.json", "this is not json");
    const ProjectStack broken = StackDetector::detect(p.path());
    CHECK(!broken.config.error.empty());
    CHECK(broken.isFullstack());                           // broken config: detection still works
}
