#include "StackToolchain.h"
#include "EnvironmentManager.h"
#include "ProcessRunner.h"
#include "ShellQuote.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <unistd.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr int DEFAULT_CHECK_TIMEOUT = 300;   // seconds
constexpr int DEFAULT_SETUP_TIMEOUT = 900;   // dependency installation can be slow

std::string readFile(const fs::path& p) {
    std::ifstream in(p);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

json readJsonFile(const fs::path& p) {
    json j = json::parse(readFile(p), nullptr, false);
    return j.is_object() ? j : json::object();
}

bool isSkippedDirectory(const std::string& name) {
    static const std::set<std::string> skipped = {
        "node_modules", "venv", "site-packages", "__pycache__", "build", "dist", "backups", "vendor"
    };
    return name.empty() || name[0] == '.' || skipped.count(name) > 0;
}

// Files below root (recursive, skipping dependency/build folders) accepted by `accept`,
// as sorted paths relative to root.
template <typename Pred>
std::vector<std::string> findFiles(const fs::path& root, int maxDepth, Pred accept) {
    std::vector<std::string> files;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_directory()) {
            if (isSkippedDirectory(it->path().filename().string()) || it.depth() >= maxDepth) it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file() && accept(it->path())) {
            files.push_back(it->path().lexically_relative(root).generic_string());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

bool hasPythonTests(const fs::path& root) {
    return !findFiles(root, 4, [](const fs::path& p) {
        const std::string name = p.filename().string();
        if (p.extension() != ".py") return false;
        return name.rfind("test_", 0) == 0 || (name.size() > 8 && name.compare(name.size() - 8, 8, "_test.py") == 0);
    }).empty();
}

// Migrations are applied in file-name order; "down"/rollback scripts are not part of
// the forward path and would undo the schema in the middle of the run.
std::vector<std::string> sqlFiles(const fs::path& root) {
    return findFiles(root, 5, [](const fs::path& p) {
        if (p.extension() != ".sql") return false;
        const std::string name = p.filename().string();
        return name.find(".down.") == std::string::npos && name != "down.sql"
            && name.find("rollback") == std::string::npos;
    });
}

bool shellSucceeds(const std::string& command) {
    return system((command + " > /dev/null 2>&1").c_str()) == 0;
}

// First existing executable: inside the component's virtualenv, otherwise on PATH.
std::string findTool(const std::string& venvBin, const std::string& tool) {
    if (!venvBin.empty() && fs::exists(fs::path(venvBin) / tool)) return ShellQuote::quote((fs::path(venvBin) / tool).string());
    return WebEnvironmentManager::commandExists(tool) ? tool : "";
}

std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
    for (size_t pos = 0; (pos = s.find(from, pos)) != std::string::npos; pos += to.size()) {
        s.replace(pos, from.size(), to);
    }
    return s;
}

// The dry-run script is written to a private temp file once per process.
std::string sqlCheckScriptPath() {
    static const std::string path = [] {
        const fs::path p = fs::temp_directory_path() / ("buggai_sql_check_" + std::to_string(getpid()) + ".py");
        std::ofstream(p) << StackToolchain::sqlCheckScript();
        return p.string();
    }();
    return path;
}

} // namespace

// Applies every file, statement by statement, to ONE in-memory SQLite database, in the
// given order (later migrations see the tables of earlier ones). Nothing is written to
// disk and no configured database is contacted.
//
// sqlite3.complete_statement() is used to find statement boundaries, so semicolons inside
// strings, comments and trigger bodies are handled by SQLite itself. The run stops at the
// first error, like a real migration would, and prints it as
//     <file>:<line of the statement>: error: <sqlite message>
const char* StackToolchain::sqlCheckScript() {
    return R"PY(import sqlite3
import sys

con = sqlite3.connect(":memory:")


def run(path, statement, line):
    try:
        # executescript, not execute: one line may hold several statements
        con.executescript(statement)
        return True
    except sqlite3.Error as exc:
        print(f"{path}:{line}: error: {exc}")
        print(f"migration stopped at {path}; later files were not applied")
        return False


for path in sys.argv[1:]:
    buffer, start = "", 0
    with open(path, encoding="utf-8", errors="replace") as handle:
        for number, text in enumerate(handle, 1):
            stripped = text.strip()
            if not buffer.strip() and (not stripped or stripped.startswith("--")):
                continue                  # blank lines / comments between statements
            if not buffer:
                start = number
            buffer += text
            if sqlite3.complete_statement(buffer):
                if not run(path, buffer, start):
                    sys.exit(1)
                buffer = ""
    if buffer.strip() and not run(path, buffer, start):   # last statement without ';'
        sys.exit(1)

print(f"{len(sys.argv) - 1} SQL file(s) applied to an in-memory SQLite database")
)PY";
}

StackToolchain::StackToolchain(ProjectStack stack) : stack_(std::move(stack)) {}

std::string StackToolchain::stepName(const std::string& component, const std::string& tool) {
    return component + ": " + tool;
}

bool StackToolchain::splitStepName(const std::string& step, std::string& component, std::string& tool) {
    const size_t sep = step.find(": ");
    if (sep == std::string::npos) return false;
    component = step.substr(0, sep);
    tool = step.substr(sep + 2);
    return true;
}

std::string StackToolchain::absoluteRoot(const StackComponent& c) const {
    std::error_code ec;
    const fs::path p = fs::weakly_canonical(fs::path(stack_.projectRoot) / c.root, ec);
    return ec ? (fs::path(stack_.projectRoot) / c.root).string() : p.string();
}

int StackToolchain::timeoutFor(const std::string& phase) const {
    const int configured = stack_.config.timeoutSec;
    if (phase == "setup") return std::max(DEFAULT_SETUP_TIMEOUT, configured);
    return configured > 0 ? configured : DEFAULT_CHECK_TIMEOUT;
}

// The component's own virtualenv if it exists (created by the setup phase), else python3.
std::string StackToolchain::pythonOf(const StackComponent& c) const {
    const std::string venvPython = EnvironmentManager::getPythonExecutable(absoluteRoot(c));
    return fs::exists(venvPython) ? ShellQuote::quote(venvPython) : "python3";
}

// ---------------------------------------------------------------------------
// Python
// ---------------------------------------------------------------------------
std::vector<StackCommand> StackToolchain::pythonCommands(const StackComponent& c, const std::string& phase) const {
    std::vector<StackCommand> commands;
    const fs::path root = absoluteRoot(c);
    // EnvironmentManager decides where the venv lives (.venv, or /tmp for /mnt/... projects)
    const std::string venvPython = EnvironmentManager::getPythonExecutable(root.string());
    const std::string venvBin = fs::path(venvPython).parent_path().string();
    const std::string venvDir = fs::path(venvBin).parent_path().string();
    const bool tests = hasPythonTests(root);

    auto add = [&](const std::string& tool, const std::string& command, const std::string& skip = "") {
        commands.push_back({c.name, tool, phase, command, skip});
    };

    if (phase == "setup") {
        std::string install;
        if (fs::exists(root / "requirements.txt")) install = "-r requirements.txt";
        else if (readFile(root / "pyproject.toml").find("[project]") != std::string::npos) install = "-e .";
        if (install.empty()) return commands;   // nothing declared: use the system interpreter

        const std::string py = ShellQuote::quote(venvPython);
        if (!fs::exists(venvPython)) add("venv", "python3 -m venv " + ShellQuote::quote(venvDir));
        std::string pip = py + " -m pip install --disable-pip-version-check -q " + install;
        // pytest is needed to run the component's tests even if it is not a declared dependency
        if (tests) pip += " && (" + py + " -c 'import pytest' 2>/dev/null || " + py + " -m pip install --disable-pip-version-check -q pytest)";
        add("pip", pip);
        return commands;
    }

    const std::string py = pythonOf(c);
    const std::string exclude = "(\\.venv|venv|node_modules|backups|build|dist)";

    // Syntax check of every file (py_compile for the whole tree). The byte code goes to
    // a temp folder (PYTHONPYCACHEPREFIX, see runCommands), not into the project.
    add("compileall", py + " -m compileall -q -x '/" + exclude + "/' .");

    // Linters only report real defects (syntax errors, undefined names), not style.
    const std::string ruff = findTool(venvBin, "ruff");
    const std::string flake8 = findTool(venvBin, "flake8");
    if (!ruff.empty()) {
        add("ruff", ruff + " check --select E9,F63,F7,F82 --output-format concise --no-cache .");
    } else if (!flake8.empty()) {
        add("flake8", flake8 + " --select=E9,F63,F7,F82 --exclude=.venv,venv,node_modules,backups,build,dist .");
    }

    const bool mypyConfigured = fs::exists(root / "mypy.ini")
        || readFile(root / "pyproject.toml").find("[tool.mypy]") != std::string::npos
        || readFile(root / "setup.cfg").find("[mypy]") != std::string::npos;
    if (mypyConfigured) {
        const std::string mypy = findTool(venvBin, "mypy");
        add("mypy", mypy + " --no-error-summary .", mypy.empty() ? "mypy is not installed" : "");
    }

    if (tests) {
        const bool available = shellSucceeds(py + " -c 'import pytest'");
        add("pytest", py + " -m pytest -q --tb=short -p no:cacheprovider --continue-on-collection-errors",
            available ? "" : "pytest is not installed");
    }
    return commands;
}

// ---------------------------------------------------------------------------
// Node (JavaScript / TypeScript)
// ---------------------------------------------------------------------------
std::vector<StackCommand> StackToolchain::nodeCommands(const StackComponent& c, const std::string& phase) const {
    std::vector<StackCommand> commands;
    const fs::path root = absoluteRoot(c);
    const std::string pm = WebEnvironmentManager::detectPackageManager(root.string());
    const json pkg = readJsonFile(root / "package.json");

    // commandExists ignores Windows executables under /mnt/ (WSL), which cannot run here
    std::string missing;
    if (!WebEnvironmentManager::commandExists("node")) missing = "node is not installed";
    else if (!WebEnvironmentManager::commandExists(pm)) missing = pm + " is not installed";

    auto add = [&](const std::string& tool, const std::string& command, std::string skip = "") {
        commands.push_back({c.name, tool, phase, command, missing.empty() ? skip : missing});
    };

    if (phase == "setup") {
        const bool hasDeps = (pkg.contains("dependencies") && !pkg["dependencies"].empty())
                          || (pkg.contains("devDependencies") && !pkg["devDependencies"].empty());
        if (!hasDeps) return commands;

        std::string install = pm + " install";
        if (pm == "npm") install = fs::exists(root / "package-lock.json") ? "npm ci --no-audit --no-fund" : "npm install --no-audit --no-fund";
        if (pm == "pnpm" || pm == "yarn") install += " --frozen-lockfile";
        add(pm + " install", install);
        return commands;
    }

    const std::string bin = "PATH=\"$PWD/node_modules/.bin:$PATH\" ";
    const std::string notInstalled = fs::exists(root / "node_modules") ? "" : "dependencies are not installed";

    // Same detection as for PHP/JS projects: a tsc based "typecheck" script, vue-tsc or tsc.
    const std::string typeCheck = WebEnvironmentManager::typeCheckCommand(root.string());
    if (!typeCheck.empty()) add("tsc", bin + typeCheck, notInstalled);
    else if (fs::exists(root / "tsconfig.json")) add("tsc", bin + "tsc --noEmit --pretty false", "typescript is not installed");

    bool eslintConfig = pkg.contains("eslintConfig");
    for (const char* name : {"eslint.config.js", "eslint.config.mjs", "eslint.config.cjs", "eslint.config.ts",
                             ".eslintrc", ".eslintrc.js", ".eslintrc.cjs", ".eslintrc.json", ".eslintrc.yml", ".eslintrc.yaml"}) {
        eslintConfig = eslintConfig || fs::exists(root / name);
    }
    if (eslintConfig) {
        add("eslint", bin + "eslint .", fs::exists(root / "node_modules/.bin/eslint") ? "" : "eslint is not installed");
    }

    if (fs::exists(root / "prisma/schema.prisma")) {
        // "validate" only parses the schema; the URL is a placeholder that is never opened
        add("prisma", bin + "DATABASE_URL=file:./buggai-dry-run.db prisma validate",
            fs::exists(root / "node_modules/.bin/prisma") ? "" : "prisma is not installed");
    }

    const std::string testScript = WebEnvironmentManager::testCommand(root.string());   // jest / vitest scripts only
    if (!testScript.empty()) {
        const std::string script = pkg["scripts"][testScript].get<std::string>();
        add(script.find("vitest") != std::string::npos ? "vitest" : "jest", pm + " run " + testScript, notInstalled);
    }
    return commands;
}

// ---------------------------------------------------------------------------
// SQL
// ---------------------------------------------------------------------------
std::vector<StackCommand> StackToolchain::sqlCommands(const StackComponent& c, const std::string& phase) const {
    std::vector<StackCommand> commands;
    if (phase != "check") return commands;
    const fs::path root = absoluteRoot(c);

    const std::vector<std::string> files = sqlFiles(root);
    if (!files.empty()) {
        std::string list;
        for (const auto& f : files) list += " " + ShellQuote::quote(f);

        if (stack_.sqlDialect == "sqlite") {
            // -I: isolated mode, the project's files cannot shadow the stdlib
            commands.push_back({c.name, "sql", phase, "python3 -I " + ShellQuote::quote(sqlCheckScriptPath()) + list,
                                WebEnvironmentManager::commandExists("python3") ? "" : "python3 is not installed"});
        } else {
            // PostgreSQL/MySQL syntax cannot be executed safely without a server and would
            // give false errors in SQLite, so only a parser based check is possible.
            const std::string sqlfluff = findTool("", "sqlfluff");
            commands.push_back({c.name, "sqlfluff",  phase,
                                sqlfluff + " lint --dialect " + stack_.sqlDialect + " --ignore linting --nocolor" + list,
                                sqlfluff.empty() ? "dialect " + stack_.sqlDialect + ": needs sqlfluff (no database is contacted)" : ""});
        }
    }

    if (fs::exists(root / "alembic.ini")) {
        // Offline mode ("--sql") renders the migrations to SQL without opening a connection.
        std::string venvBin;
        for (const auto& other : stack_.components) {
            if (other.kind == "python" && other.root == c.root) {
                venvBin = fs::path(EnvironmentManager::getPythonExecutable(absoluteRoot(other))).parent_path().string();
            }
        }
        const std::string alembic = findTool(venvBin, "alembic");
        commands.push_back({c.name, "alembic", phase, alembic + " upgrade head --sql",
                            alembic.empty() ? "alembic is not installed" : ""});
    }
    return commands;
}

// ---------------------------------------------------------------------------
// defaults + overrides
// ---------------------------------------------------------------------------
std::vector<StackCommand> StackToolchain::commandsFor(const StackComponent& c, const std::string& phase) const {
    std::vector<StackCommand> commands;
    if (c.kind == "python") commands = pythonCommands(c, phase);
    else if (c.kind == "node") commands = nodeCommands(c, phase);
    else if (c.kind == "sql") commands = sqlCommands(c, phase);

    if (phase != "check") return commands;

    // .buggai.json "commands": first the entry for the kind, then the one for the
    // component name, so the more specific setting wins.
    for (const std::string& target : {c.kind, c.name}) {
        auto it = stack_.config.commands.find(target);
        if (it == stack_.config.commands.end()) continue;

        for (const auto& [tool, override] : it->second) {
            auto existing = std::find_if(commands.begin(), commands.end(),
                                         [&](const StackCommand& cmd) { return cmd.tool == tool; });
            if (override.disabled) {
                if (existing != commands.end()) commands.erase(existing);
                continue;
            }
            // {python} = interpreter of this component (its virtualenv when there is one)
            const std::string command = replaceAll(override.command, "{python}",
                                                   c.kind == "python" ? pythonOf(c) : "python3");
            if (existing != commands.end()) {
                existing->command = command;
                existing->skipReason.clear();   // the user's command decides what is needed
            } else {
                commands.push_back({c.name, tool, phase, command, ""});
            }
        }
    }
    return commands;
}

// ---------------------------------------------------------------------------
// execution
// ---------------------------------------------------------------------------
std::vector<SetupStep> StackToolchain::runCommands(const std::vector<StackCommand>& commands) {
    std::vector<SetupStep> steps;

    for (const auto& cmd : commands) {
        const std::string name = stepName(cmd.component, cmd.tool);

        if (!cmd.skipReason.empty()) {
            const std::string entry = name + " (" + cmd.skipReason + ")";
            if (std::find(skipped_.begin(), skipped_.end(), entry) == skipped_.end()) {
                skipped_.push_back(entry);
                std::cout << "\n--- " << name << " ---\n[SKIPPED] " << cmd.skipReason << "\n";
            }
            continue;
        }

        const StackComponent* component = stack_.find(cmd.component);
        const int limit = timeoutFor(cmd.phase);

        // Environment of check commands:
        //  DATABASE_URL            in-memory SQLite instead of whatever the shell has configured
        //  PYTHONPYCACHEPREFIX     keeps .pyc files out of the project
        //  CI / NO_COLOR           one-shot test runs (no watch mode) and plain output for the parsers
        std::string env;
        if (cmd.phase == "check") {
            env = "env DATABASE_URL=sqlite:// PYTHONDONTWRITEBYTECODE=1 PYTHONPYCACHEPREFIX="
                + ShellQuote::quote((fs::temp_directory_path() / "buggai_pycache").string())
                + " CI=1 NO_COLOR=1 FORCE_COLOR=0 ";
        }

        std::cout << "\n--- " << name << " ---\n$ " << cmd.command << "\n";

        // timeout runs the command in its own process group and signals the whole group,
        // so child processes (test workers, dev servers) are stopped as well.
        SetupStep step;
        step.name = name;
        step.command = cmd.command;
        step.result = run_capture("cd " + ShellQuote::quote(component ? absoluteRoot(*component) : stack_.projectRoot)
                                  + " && " + env + "timeout --kill-after=10 " + std::to_string(limit)
                                  + " bash -c " + ShellQuote::quote(cmd.command));

        if (step.result.exit_code == 124 || step.result.exit_code == 137) {
            // wording matches the checklist entry "command_timeout" (environment error)
            const std::string note = "command timed out after " + std::to_string(limit) + " seconds\n";
            std::cout << note;
            step.result.output += note;
        }
        steps.push_back(step);
    }
    return steps;
}

std::vector<SetupStep> StackToolchain::setup() {
    std::cout << "\n╔══════════════════════════════════════════════════════════╗\n";
    std::cout << "║  PROJECT SETUP (fullstack components)                    ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════╝\n";

    std::vector<SetupStep> steps;
    for (const auto& c : stack_.components) {
        // one command at a time: later commands depend on earlier ones (venv before pip)
        for (const auto& cmd : commandsFor(c, "setup")) {
            const std::vector<SetupStep> result = runCommands({cmd});
            if (result.empty()) continue;            // skipped
            steps.push_back(result.front());
            if (!result.front().ok()) break;         // e.g. no pip install without a venv
        }
    }
    return steps;
}

std::vector<std::string> StackToolchain::affectedComponents(const std::string& changedFile) const {
    std::vector<std::string> all;
    for (const auto& c : stack_.components) all.push_back(c.name);

    const StackComponent* owner = changedFile.empty() ? nullptr : stack_.componentOf(changedFile);
    if (!owner) return all;   // unknown file: be safe and check everything

    // The owner itself plus everything that (transitively) depends on it: a backend fix can
    // break the frontend, a schema fix can break the backend and through it the frontend.
    std::vector<std::string> affected = {owner->name};
    for (const auto& d : stack_.dependentsOf(owner->name)) affected.push_back(d);
    return affected;
}

std::vector<SetupStep> StackToolchain::check(const std::string& changedFile) {
    std::vector<std::string> affected = affectedComponents(lastChecks_.empty() ? "" : changedFile);

    std::cout << "\n╔══════════════════════════════════════════════════════════╗\n";
    std::cout << "║  CHECKS (per component: syntax, lint, types, tests)      ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════╝\n";
    if (affected.size() < stack_.components.size()) {
        std::cout << "[VERIFY] " << changedFile << " changed -> re-running:";
        for (const auto& name : affected) std::cout << " " << name;
        std::cout << " (changed component and its dependents)\n";
    }

    std::vector<SetupStep> all;
    for (const auto& c : stack_.components) {
        if (std::find(affected.begin(), affected.end(), c.name) != affected.end()) {
            lastChecks_[c.name] = runCommands(commandsFor(c, "check"));
        }
        const auto& steps = lastChecks_[c.name];
        all.insert(all.end(), steps.begin(), steps.end());
    }
    return all;
}
