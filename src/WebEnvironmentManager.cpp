#include "WebEnvironmentManager.h"
#include "ShellQuote.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <sstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    size_t end = s.find_last_not_of(" \t\r\n");
    return start == std::string::npos ? "" : s.substr(start, end - start + 1);
}

static json readJsonFile(const std::string& path) {
    std::ifstream in(path);
    if (!in) return json::object();
    try {
        return json::parse(in);
    } catch (...) {
        return json::object();
    }
}

static int firstMajorVersion(const std::string& constraint, int fallback) {
    std::smatch m;
    if (std::regex_search(constraint, m, std::regex("(\\d+)"))) {
        return std::stoi(m[1]);
    }
    return fallback;
}

static bool isSafeName(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-';
    });
}

std::string WebEnvironmentManager::readCommandOutput(const std::string& command) {
    std::string out;
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return out;
    char buf[512];
    while (fgets(buf, sizeof(buf), pipe)) out += buf;
    pclose(pipe);
    return out;
}

SetupStep WebEnvironmentManager::prepareTestDatabase(const std::string& projectPath) {
    std::string php =
        "require 'vendor/autoload.php'; $app = require 'bootstrap/app.php'; "
        "$app->make(Illuminate\\Contracts\\Console\\Kernel::class)->bootstrap(); "
        "$c = config('database.connections.testing'); "
        "echo $c ? $c['driver'] . '|' . $c['database'] : '';";
    std::string config = trim(readCommandOutput("cd " + ShellQuote::quote(projectPath)
                                                + " && php -r " + ShellQuote::quote(php) + " 2>/dev/null"));

    SetupStep step;
    step.name = "runtime: prepare test database";
    step.result.exit_code = 0;
    if (config.empty()) return step;

    std::string driver = config.substr(0, config.find('|'));
    std::string database = config.substr(config.find('|') + 1);
    if (driver == "sqlite" && database != ":memory:" && !database.empty() && !fs::exists(database)) {
        fs::create_directories(fs::path(database).parent_path());
        std::ofstream(database).close();
    }

    return runStep(step.name, projectPath, "php artisan migrate:fresh --database=testing --force");
}

bool WebEnvironmentManager::commandExists(const std::string& command) {
    std::string cmd = "p=$(command -v " + command + ") && case \"$p\" in /mnt/*) exit 1;; esac";
    return system(cmd.c_str()) == 0;
}

bool WebEnvironmentManager::aptInstall(const std::vector<std::string>& packages) {
    if (packages.empty()) return true;

    std::string list;
    for (const auto& p : packages) list += " " + p;

    std::cout << "Installing:" << list << "\n";
    std::string cmd = "sudo apt-get update -qq && sudo apt-get install -y" + list;
    return system(cmd.c_str()) == 0;
}

SetupStep WebEnvironmentManager::runStep(const std::string& name,
                                         const std::string& projectPath,
                                         const std::string& command) {
    std::cout << "\n--- " << name << " ---\n$ " << command << "\n";

    SetupStep step;
    step.name = name;
    step.command = command;
    step.result = run_capture("cd " + ShellQuote::quote(projectPath) + " && " + command);
    return step;
}

bool WebEnvironmentManager::isWebProject(const std::string& projectPath) {
    return fs::exists(fs::path(projectPath) / "composer.json")
        || fs::exists(fs::path(projectPath) / "package.json");
}

bool WebEnvironmentManager::ensurePhp(const std::string& projectPath, const std::string& dbConnection) {
    std::vector<std::string> packages;

    if (!commandExists("php")) {
        packages.push_back("php-cli");
    }
    if (!commandExists("unzip")) {
        packages.push_back("unzip");
    }

    std::set<std::string> loaded;
    std::string modulesOutput;
    if (packages.empty() || packages.front() != "php-cli") {
        modulesOutput = readCommandOutput("php -m 2>/dev/null");
    }
    std::istringstream lines(modulesOutput);
    for (std::string line; std::getline(lines, line);) {
        std::transform(line.begin(), line.end(), line.begin(), ::tolower);
        loaded.insert(line);
    }

    std::set<std::string> required = {"mbstring", "xml", "curl", "zip", "bcmath", "intl"};
    json composer = readJsonFile(projectPath + "/composer.json");
    if (composer.contains("require") && composer["require"].is_object()) {
        for (const auto& [name, _] : composer["require"].items()) {
            if (name.rfind("ext-", 0) == 0) {
                std::string ext = name.substr(4);
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                required.insert(ext);
            }
        }
    }

    if (dbConnection == "sqlite") required.insert("pdo_sqlite");
    if (dbConnection == "mysql" || dbConnection == "mariadb") required.insert("pdo_mysql");
    if (dbConnection == "pgsql") required.insert("pdo_pgsql");

    static const std::set<std::string> builtIn = {
        "json", "fileinfo", "ctype", "tokenizer", "pdo", "openssl", "filter", "hash", "session",
        "pcre", "spl", "reflection", "iconv", "exif", "phar", "posix", "sockets", "calendar",
        "ftp", "gettext", "shmop", "sysvmsg", "sysvsem", "sysvshm", "standard", "date", "zlib"
    };
    static const std::map<std::string, std::string> aptName = {
        {"simplexml", "xml"}, {"dom", "xml"}, {"xmlreader", "xml"}, {"xmlwriter", "xml"},
        {"pdo_mysql", "mysql"}, {"mysqli", "mysql"},
        {"pdo_pgsql", "pgsql"},
        {"pdo_sqlite", "sqlite3"}, {"sqlite3", "sqlite3"},
    };

    std::set<std::string> extPackages;
    for (const auto& ext : required) {
        if (builtIn.count(ext) || loaded.count(ext)) continue;
        auto it = aptName.find(ext);
        extPackages.insert("php-" + (it == aptName.end() ? ext : it->second));
    }
    packages.insert(packages.end(), extPackages.begin(), extPackages.end());

    if (composer.contains("require") && composer["require"].contains("php")) {
        std::cout << "Project requires PHP " << composer["require"]["php"].get<std::string>() << "\n";
    }

    return aptInstall(packages);
}

bool WebEnvironmentManager::ensureComposer() {
    if (commandExists("composer")) return true;
    return aptInstall({"composer"});
}

bool WebEnvironmentManager::ensureNode(const std::string& projectPath) {
    int required = 22;
    json pkg = readJsonFile(projectPath + "/package.json");
    if (pkg.contains("engines") && pkg["engines"].contains("node") && pkg["engines"]["node"].is_string()) {
        required = firstMajorVersion(pkg["engines"]["node"].get<std::string>(), 22);
    }

    if (commandExists("node")) {
        RunResult v = run_capture("node --version");
        int installed = firstMajorVersion(v.output, 0);
        if (installed >= required) return true;
        std::cout << "Node.js " << installed << " is too old, project needs " << required << "\n";
    }

    std::cout << "Installing Node.js " << required << " from NodeSource...\n";
    if (!commandExists("curl")) aptInstall({"curl"});

    std::string cmd = "curl -fsSL https://deb.nodesource.com/setup_" + std::to_string(required)
                    + ".x | sudo -E bash - && sudo apt-get install -y nodejs";
    return system(cmd.c_str()) == 0;
}

bool WebEnvironmentManager::ensurePackageManager(const std::string& pm) {
    if (pm == "npm" || commandExists(pm)) return true;

    if (system("sudo corepack enable > /dev/null 2>&1") == 0 && commandExists(pm)) {
        return true;
    }
    return system(("sudo npm install -g " + pm).c_str()) == 0;
}

std::string WebEnvironmentManager::detectPackageManager(const std::string& projectPath) {
    fs::path p(projectPath);
    if (fs::exists(p / "pnpm-lock.yaml")) return "pnpm";
    if (fs::exists(p / "yarn.lock")) return "yarn";
    return "npm";
}

std::string WebEnvironmentManager::readEnvValue(const std::string& envFile, const std::string& key) {
    std::ifstream in(envFile);
    for (std::string line; std::getline(in, line);) {
        if (line.rfind(key + "=", 0) == 0) {
            std::string value = line.substr(key.size() + 1);
            if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'')) {
                value = value.substr(1, value.size() - 2);
            }
            return value;
        }
    }
    return "";
}

void WebEnvironmentManager::writeEnvValue(const std::string& envFile, const std::string& key, const std::string& value) {
    std::ifstream in(envFile);
    std::vector<std::string> lines;
    bool found = false;

    for (std::string line; std::getline(in, line);) {
        if (!found && line.rfind(key + "=", 0) == 0) {
            line = key + "=" + value;
            found = true;
        }
        lines.push_back(line);
    }
    in.close();

    if (!found) lines.push_back(key + "=" + value);

    std::ofstream out(envFile);
    for (const auto& l : lines) out << l << "\n";
}

void WebEnvironmentManager::prepareEnvFile(const std::string& projectPath) {
    fs::path env = fs::path(projectPath) / ".env";
    fs::path example = fs::path(projectPath) / ".env.example";

    if (!fs::exists(env) && fs::exists(example)) {
        std::cout << "Creating .env from .env.example\n";
        fs::copy_file(example, env);
    }
}

bool WebEnvironmentManager::setupDatabase(const std::string& projectPath, const std::string& dbConnection) {
    std::string envFile = projectPath + "/.env";
    std::string projectName = fs::path(projectPath).filename().string();

    if (dbConnection == "sqlite") {
        std::string dbFile = readEnvValue(envFile, "DB_DATABASE");
        if (dbFile.empty() || dbFile[0] != '/') {
            dbFile = fs::absolute(fs::path(projectPath) / "database" / "database.sqlite").string();
            writeEnvValue(envFile, "DB_DATABASE", dbFile);
        }
        fs::create_directories(fs::path(dbFile).parent_path());
        if (!fs::exists(dbFile)) std::ofstream(dbFile).close();
        std::cout << "SQLite database: " << dbFile << "\n";
        return true;
    }

    std::string db = readEnvValue(envFile, "DB_DATABASE");
    std::string user = readEnvValue(envFile, "DB_USERNAME");
    std::string password = readEnvValue(envFile, "DB_PASSWORD");

    std::string safeProject = projectName;
    std::replace_if(safeProject.begin(), safeProject.end(), [](unsigned char c) { return !std::isalnum(c); }, '_');

    if (!isSafeName(db)) db = "buggy_" + safeProject;
    if (!isSafeName(user) || user == "root") user = "buggy";
    if (!isSafeName(password)) password = "buggy";

    writeEnvValue(envFile, "DB_HOST", "127.0.0.1");
    writeEnvValue(envFile, "DB_DATABASE", db);
    writeEnvValue(envFile, "DB_USERNAME", user);
    writeEnvValue(envFile, "DB_PASSWORD", password);

    if (dbConnection == "mysql" || dbConnection == "mariadb") {
        writeEnvValue(envFile, "DB_PORT", "3306");

        if (!commandExists("mariadb") && !commandExists("mysql")) {
            if (!aptInstall({"mariadb-server"})) return false;
        }
        system("sudo service mariadb start > /dev/null 2>&1 || sudo service mysql start > /dev/null 2>&1");

        std::string sql =
            "CREATE DATABASE IF NOT EXISTS `" + db + "`;"
            "CREATE USER IF NOT EXISTS '" + user + "'@'localhost' IDENTIFIED BY '" + password + "';"
            "CREATE USER IF NOT EXISTS '" + user + "'@'127.0.0.1' IDENTIFIED BY '" + password + "';"
            "ALTER USER '" + user + "'@'localhost' IDENTIFIED BY '" + password + "';"
            "ALTER USER '" + user + "'@'127.0.0.1' IDENTIFIED BY '" + password + "';"
            "GRANT ALL PRIVILEGES ON `" + db + "`.* TO '" + user + "'@'localhost';"
            "GRANT ALL PRIVILEGES ON `" + db + "`.* TO '" + user + "'@'127.0.0.1';"
            "FLUSH PRIVILEGES;";
        std::cout << "MySQL/MariaDB database '" << db << "' for user '" << user << "'\n";
        return system(("sudo mysql -e " + ShellQuote::quote(sql)).c_str()) == 0;
    }

    if (dbConnection == "pgsql") {
        writeEnvValue(envFile, "DB_PORT", "5432");

        if (!commandExists("psql")) {
            if (!aptInstall({"postgresql"})) return false;
        }
        system("sudo service postgresql start > /dev/null 2>&1");

        std::string role =
            "DO $$ BEGIN "
            "IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = '" + user + "') THEN "
            "CREATE ROLE \"" + user + "\" LOGIN; END IF; END $$;"
            "ALTER ROLE \"" + user + "\" WITH LOGIN CREATEDB PASSWORD '" + password + "';";
        std::cout << "PostgreSQL database '" << db << "' for user '" << user << "'\n";
        if (system(("sudo -u postgres psql -q -c " + ShellQuote::quote(role)).c_str()) != 0) return false;

        system(("sudo -u postgres createdb -O " + ShellQuote::quote(user) + " " + ShellQuote::quote(db)
                + " > /dev/null 2>&1").c_str());
        return true;
    }

    std::cout << "Unknown DB_CONNECTION '" << dbConnection << "', skipping database setup\n";
    return true;
}

std::vector<SetupStep> WebEnvironmentManager::setupProject(const std::string& projectPath) {
    std::vector<SetupStep> steps;
    fs::path root(projectPath);

    bool hasComposer = fs::exists(root / "composer.json");
    bool hasPackageJson = fs::exists(root / "package.json");
    bool isLaravel = fs::exists(root / "artisan");

    std::cout << "\n╔══════════════════════════════════════════════════════════╗\n";
    std::cout << "║  PROJECT SETUP (PHP / JavaScript)                        ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════╝\n";

    if (hasComposer) {
        std::string dbConnection;

        if (isLaravel) {
            prepareEnvFile(projectPath);
            dbConnection = readEnvValue(projectPath + "/.env", "DB_CONNECTION");
            if (dbConnection.empty()) dbConnection = "mysql";
        }

        if (!ensurePhp(projectPath, dbConnection) || !ensureComposer()) {
            std::cerr << "Could not install PHP or composer\n";
        }

        if (isLaravel) {
            SetupStep db;
            db.name = "database (" + dbConnection + ")";
            db.command = "automatic database setup";
            db.result.exit_code = setupDatabase(projectPath, dbConnection) ? 0 : 1;
            steps.push_back(db);
        }

        steps.push_back(runStep("composer install", projectPath,
                                "composer install --no-interaction --no-progress"));

        if (isLaravel && steps.back().ok()) {
            steps.push_back(runStep("artisan key:generate", projectPath, "php artisan key:generate --force"));
            steps.push_back(runStep("artisan migrate", projectPath, "php artisan migrate --force"));
        }
    }

    if (hasPackageJson) {
        std::string pm = detectPackageManager(projectPath);

        if (!ensureNode(projectPath) || !ensurePackageManager(pm)) {
            std::cerr << "Could not install Node.js or " << pm << "\n";
        }

        std::string install = pm + " install";
        if (pm == "npm" && fs::exists(root / "package-lock.json")) install = "npm ci";

        steps.push_back(runStep(pm + " install", projectPath, install));
    }

    return steps;
}

std::string WebEnvironmentManager::typeCheckCommand(const std::string& projectPath) {
    fs::path root(projectPath);
    json pkg = readJsonFile(projectPath + "/package.json");

    if (pkg.contains("scripts") && pkg["scripts"].is_object()) {
        for (const char* key : {"type-check", "typecheck", "types", "check-types", "tsc"}) {
            if (!pkg["scripts"].contains(key) || !pkg["scripts"][key].is_string()) continue;
            std::string script = pkg["scripts"][key].get<std::string>();
            if (script.find("tsc") != std::string::npos && script.find("&&") == std::string::npos) {
                return script + " --pretty false";
            }
        }
    }

    if (fs::exists(root / "node_modules/.bin/vue-tsc") && fs::exists(root / "tsconfig.json")) {
        return "vue-tsc --noEmit --pretty false";
    }
    if (fs::exists(root / "node_modules/.bin/tsc") && fs::exists(root / "tsconfig.json")) {
        return "tsc --noEmit --pretty false";
    }
    return "";
}

std::string WebEnvironmentManager::testCommand(const std::string& projectPath) {
    json pkg = readJsonFile(projectPath + "/package.json");
    if (!pkg.contains("scripts") || !pkg["scripts"].is_object()) return "";

    for (const char* key : {"test:unit", "test"}) {
        if (!pkg["scripts"].contains(key) || !pkg["scripts"][key].is_string()) continue;
        std::string script = pkg["scripts"][key].get<std::string>();
        if (script.find("vitest") != std::string::npos || script.find("jest") != std::string::npos
            || script.find("vp test") != std::string::npos) {
            return key;
        }
    }
    return "";
}

SetupStep WebEnvironmentManager::smokeTest(const std::string& projectPath) {
    const std::string port = "18765";
    const std::string url = "http://127.0.0.1:" + port;

    if (!commandExists("curl")) aptInstall({"curl"});

    std::string script =
        "LOG=storage/logs/laravel.log; "
        "START=$(stat -c%s \"$LOG\" 2>/dev/null || echo 0); "
        "php artisan serve --host=127.0.0.1 --port=" + port + " > /tmp/buggy_serve_" + port + ".log 2>&1 & "
        "PID=$!; "
        "for i in $(seq 1 40); do curl -s -o /dev/null " + url + "/ && break; sleep 0.25; done; "
        "FAIL=0; "
        "for u in / /login /register; do "
        "  CODE=$(curl -s -o /dev/null -w '%{http_code}' --max-time 30 " + url + "$u); "
        "  echo \"GET $u -> HTTP $CODE\"; "
        "  case $CODE in 5*|000) FAIL=1;; esac; "
        "done; "
        "pkill -P $PID 2>/dev/null; kill $PID 2>/dev/null; "
        "if [ -f \"$LOG\" ]; then tail -c +$((START+1)) \"$LOG\" | head -c 30000; fi; "
        "exit $FAIL";

    return runStep("runtime: HTTP smoke test", projectPath, "bash -c " + ShellQuote::quote(script));
}

std::vector<SetupStep> WebEnvironmentManager::runChecks(const std::string& projectPath, bool runtime) {
    std::vector<SetupStep> steps;
    fs::path root(projectPath);

    bool hasComposer = fs::exists(root / "composer.json");
    bool hasPackageJson = fs::exists(root / "package.json");
    bool isLaravel = fs::exists(root / "artisan");
    bool hasVendor = fs::exists(root / "vendor");
    bool hasNodeModules = fs::exists(root / "node_modules");

    std::cout << "\n╔══════════════════════════════════════════════════════════╗\n";
    std::cout << "║  CHECKS (static analysis, build, runtime)                ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════╝\n";

    if (hasComposer) {
        bool phpstanConfig = fs::exists(root / "phpstan.neon") || fs::exists(root / "phpstan.neon.dist")
                          || fs::exists(root / "phpstan.dist.neon");

        if (hasVendor && fs::exists(root / "vendor/bin/phpstan") && phpstanConfig) {
            steps.push_back(runStep("php: phpstan", projectPath,
                "vendor/bin/phpstan analyse --no-progress --error-format=raw --memory-limit=2G"));
        } else {
            std::string lint =
                "OUT=$(find . -name '*.php' -not -path './vendor/*' -not -path './node_modules/*' "
                "-not -path './storage/*' -print0 | xargs -0 -n1 -P8 php -l 2>&1 "
                "| grep -v '^No syntax errors detected'); echo \"$OUT\"; [ -z \"$OUT\" ]";
            steps.push_back(runStep("php: syntax check (php -l)", projectPath, "bash -c " + ShellQuote::quote(lint)));
        }
    }

    if (hasPackageJson && hasNodeModules) {
        std::string pm = detectPackageManager(projectPath);
        std::string binPath = "PATH=\"$PWD/node_modules/.bin:$PATH\" ";

        std::string typeCheck = typeCheckCommand(projectPath);
        if (!typeCheck.empty()) {
            steps.push_back(runStep("typescript: type check", projectPath, binPath + typeCheck));
        }

        json pkg = readJsonFile(projectPath + "/package.json");
        if (pkg.contains("scripts") && pkg["scripts"].contains("build")) {
            steps.push_back(runStep("frontend: " + pm + " run build", projectPath, pm + " run build"));
        }
    }

    if (isLaravel && hasVendor) {
        steps.push_back(smokeTest(projectPath));
    }

    if (runtime) {
        if (isLaravel && hasVendor) {
            std::string php = "php";
            RunResult modules = run_capture("php -m | grep -i '^xdebug$'");
            if (modules.exit_code != 0) {
                aptInstall({"php-xdebug"});
                modules = run_capture("php -m | grep -i '^xdebug$'");
            }
            if (modules.exit_code == 0) {
                php = "XDEBUG_MODE=develop php -d xdebug.mode=develop";
            }
            steps.push_back(prepareTestDatabase(projectPath));
            steps.push_back(runStep("runtime: php tests", projectPath,
                                    "timeout 900 env " + php + " artisan test --stop-on-failure"));
        }

        std::string test = hasPackageJson && hasNodeModules ? testCommand(projectPath) : "";
        if (!test.empty()) {
            std::string pm = detectPackageManager(projectPath);
            steps.push_back(runStep("runtime: js tests", projectPath,
                "NODE_OPTIONS=\"--enable-source-maps --stack-trace-limit=50\" timeout 900 " + pm + " run " + test));
        }
    }

    return steps;
}

void WebEnvironmentManager::printSummary(const std::vector<SetupStep>& steps, const std::string& title) {
    std::cout << "\n════════════════════════════════════════════════════════════\n";
    std::cout << title << "\n";
    for (const auto& s : steps) {
        std::cout << "  " << (s.ok() ? "✓ " : "✗ ") << s.name;
        if (!s.ok()) std::cout << "  (exit code " << s.result.exit_code << ")";
        std::cout << "\n";
    }
    std::cout << "════════════════════════════════════════════════════════════\n";
}
