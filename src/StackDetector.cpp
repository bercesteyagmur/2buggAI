#include "StackDetector.h"
#include "LanguageDetector.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr int MAX_MARKER_DEPTH = 3;   // root = 0; markers deeper than this are ignored

std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    size_t end = s.find_last_not_of(" \t\r\n");
    return start == std::string::npos ? "" : s.substr(start, end - start + 1);
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

// "./backend/" -> "backend", "" -> "."
std::string normalizeRoot(const std::string& path) {
    std::string s = fs::path(path).lexically_normal().generic_string();
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    if (s.rfind("./", 0) == 0) s = s.substr(2);
    return (s.empty() || s == "./") ? "." : s;
}

// true if file/dir `path` is `root` itself or below it (both project-relative)
bool isInside(const std::string& path, const std::string& root) {
    if (root == ".") return true;
    return path == root || path.rfind(root + "/", 0) == 0;
}

std::string lastSegment(const std::string& root) {
    return root == "." ? "" : fs::path(root).filename().string();
}

std::string firstSegment(const std::string& root) {
    return root == "." ? "" : root.substr(0, root.find('/'));
}

// Directories that never contain project components. Same idea as
// FileCollector::isBuildDirectory, plus folders that typically hold samples.
bool isSkippedDirectory(const std::string& name) {
    static const std::set<std::string> skipped = {
        "build", "venv", "site-packages", "node_modules", "vendor", "bower_components",
        "dist", "target", "coverage", "__pycache__", "backups",
        "fixtures", "testdata", "examples", "docs"
    };
    return name.empty() || name[0] == '.' || skipped.count(name) > 0 || name.rfind("cmake-build", 0) == 0;
}

bool hasAny(const fs::path& dir, std::initializer_list<const char*> names) {
    for (const char* n : names) {
        if (fs::exists(dir / n)) return true;
    }
    return false;
}

bool hasSqlFiles(const fs::path& dir) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".sql") return true;
    }
    return false;
}

std::string kindOfLanguage(const std::string& language) {
    if (language == "python") return "python";
    if (language == "sql") return "sql";
    if (language == "javascript" || language == "typescript" || language == "vue") return "node";
    return "";
}

void addUnique(std::vector<std::string>& list, const std::string& value) {
    if (std::find(list.begin(), list.end(), value) == list.end()) list.push_back(value);
}

std::string unquote(std::string s) {
    s = trim(s);
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front()) {
        s = s.substr(1, s.size() - 2);
    }
    return s;
}

// Marker-file scan: one component per (directory, kind).
void scanMarkers(const fs::path& projectRoot, const fs::path& dir, int depth,
                 std::vector<StackComponent>& out, std::vector<std::string>& sqlDirs) {
    const std::string rel = normalizeRoot(dir.lexically_relative(projectRoot).generic_string());

    if (hasAny(dir, {"pyproject.toml", "requirements.txt", "setup.py", "Pipfile", "manage.py"})) {
        out.push_back({"", "", "python", "python", rel, {}});
    }
    if (fs::exists(dir / "package.json")) {
        const bool ts = fs::exists(dir / "tsconfig.json");
        out.push_back({"", "", "node", ts ? "typescript" : "javascript", rel, {}});
    }
    if (fs::exists(dir / "alembic.ini") || hasSqlFiles(dir)) {
        sqlDirs.push_back(rel);
    }

    if (depth >= MAX_MARKER_DEPTH) {
        // .sql files are often one level deeper than the other markers
        // (e.g. services/api/db/migrations), so look one more level for them only.
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (!entry.is_directory() || isSkippedDirectory(entry.path().filename().string())) continue;
            if (hasSqlFiles(entry.path())) {
                sqlDirs.push_back(normalizeRoot(entry.path().lexically_relative(projectRoot).generic_string()));
            }
        }
        return;
    }

    std::vector<fs::path> children;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.is_directory() && !isSkippedDirectory(entry.path().filename().string())) {
            children.push_back(entry.path());
        }
    }
    std::sort(children.begin(), children.end());   // stable component order on every machine
    for (const auto& child : children) scanMarkers(projectRoot, child, depth + 1, out, sqlDirs);
}

// Nested SQL folders (db/, db/migrations/) form one component rooted at the topmost one.
// A project whose only .sql files are in the root gets a component with root ".".
void addSqlComponents(std::vector<std::string> sqlDirs, std::vector<StackComponent>& out) {
    std::sort(sqlDirs.begin(), sqlDirs.end(), [](const std::string& a, const std::string& b) {
        return a.size() != b.size() ? a.size() < b.size() : a < b;
    });
    std::vector<std::string> roots;
    for (const auto& dir : sqlDirs) {
        bool nested = false;
        for (const auto& r : roots) nested = nested || isInside(dir, r);
        if (!nested) roots.push_back(dir);
    }
    for (const auto& r : roots) out.push_back({"", "", "sql", "sql", r, {}});
}

// Name = last path segment (or the kind for the project root); if two components would
// get the same name, the kind is appended ("web-python", "web-node").
void assignNames(std::vector<StackComponent>& components) {
    std::map<std::string, int> count;
    for (auto& c : components) {
        if (c.name.empty()) {
            c.name = lastSegment(c.root);
            if (c.name.empty()) c.name = c.kind;
        }
        count[c.name]++;
    }
    std::set<std::string> used;
    for (auto& c : components) {
        if (count[c.name] > 1) c.name += "-" + c.kind;
        // still not unique (same folder name in two places): use the whole path
        if (!used.insert(c.name).second) {
            std::string full = c.root;
            std::replace(full.begin(), full.end(), '/', '-');
            c.name = full + "-" + c.kind;
            used.insert(c.name);
        }
    }
}

void assignServices(ProjectStack& stack) {
    const std::string composeDir = stack.composeFile.empty()
        ? "." : normalizeRoot(fs::path(stack.composeFile).parent_path().generic_string());

    for (auto& c : stack.components) {
        if (!c.service.empty()) continue;

        // 1. compose service whose build context contains the component (longest context wins)
        size_t best = 0;
        for (const auto& s : stack.composeServices) {
            if (s.context.empty()) continue;
            const std::string context = normalizeRoot((fs::path(composeDir) / s.context).generic_string());
            if (context == "." && c.root != ".") continue;   // context "." would swallow everything
            if (isInside(c.root, context) && context.size() >= best) {
                best = context.size();
                c.service = s.name;
            }
        }
        if (!c.service.empty()) continue;

        // 2. services/<name>/... or apps/<name>/...
        const std::string first = firstSegment(c.root);
        if ((first == "services" || first == "apps") && c.root.size() > first.size() + 1) {
            const std::string rest = c.root.substr(first.size() + 1);
            c.service = rest.substr(0, rest.find('/'));
            continue;
        }

        // 3. top-level folder
        c.service = first;
    }
}

bool isCode(const StackComponent& c) { return c.kind == "python" || c.kind == "node"; }

void assignDependencies(ProjectStack& stack) {
    auto& components = stack.components;

    // 1. docker-compose depends_on between services. It describes which running service
    //    calls which, so it links the code components; SQL is linked in step 2.
    for (auto& c : components) {
        if (!isCode(c)) continue;
        for (const auto& s : stack.composeServices) {
            if (s.name != c.service) continue;
            for (const auto& depService : s.dependsOn) {
                for (const auto& other : components) {
                    if (other.name != c.name && other.service == depService && isCode(other)) {
                        addUnique(c.dependsOn, other.name);
                    }
                }
            }
        }
    }

    // 2. Code depends on the schema it uses: SQL inside the component's own folder, or
    //    shared SQL that lives outside every code component (e.g. a top-level db/ folder).
    for (const auto& sql : components) {
        if (sql.kind != "sql") continue;
        bool owned = false;
        for (auto& c : components) {
            if (isCode(c) && c.root != "." && isInside(sql.root, c.root)) {
                addUnique(c.dependsOn, sql.name);
                owned = true;
            }
        }
        if (owned) continue;
        for (auto& c : components) {
            if (c.kind == "python") addUnique(c.dependsOn, sql.name);
        }
    }

    // 3. A Node component for which compose says nothing is assumed to be a frontend
    //    that consumes the API of the Python components.
    for (auto& c : components) {
        if (c.kind != "node") continue;
        bool dependsOnCode = false;
        for (const auto& d : c.dependsOn) {
            const StackComponent* dep = stack.find(d);
            dependsOnCode = dependsOnCode || (dep && isCode(*dep));
        }
        if (dependsOnCode) continue;
        for (const auto& other : components) {
            if (other.kind == "python") addUnique(c.dependsOn, other.name);
        }
    }
}

std::string detectSqlDialect(const ProjectStack& stack) {
    if (!stack.config.sqlDialect.empty()) return stack.config.sqlDialect;
    for (const auto& s : stack.composeServices) {
        if (s.image.find("postgres") != std::string::npos) return "postgres";
        if (s.image.find("mysql") != std::string::npos || s.image.find("mariadb") != std::string::npos) return "mysql";
    }
    return "sqlite";
}

} // namespace

// ---------------------------------------------------------------------------
// docker-compose
// ---------------------------------------------------------------------------

// Not a YAML parser. It understands the block layout that compose files use in practice:
//
//   services:
//     api:                      <- service name (first indentation level below "services:")
//       build: ./backend        <- or  build:\n  context: ./backend
//       image: postgres:16
//       depends_on: [db]        <- or a "- db" list, or a map "db:\n  condition: ..."
//
// Anchors, "extends" and flow mappings are not supported.
std::vector<ComposeService> StackDetector::parseCompose(const std::string& yamlText) {
    std::vector<ComposeService> services;

    bool inServices = false;
    int serviceIndent = -1;     // indentation of service names
    int keyIndent = -1;         // indentation of the keys inside the current service
    std::string section;        // current multi-line key: "build" | "depends_on" | ""

    std::istringstream stream(yamlText);
    for (std::string raw; std::getline(stream, raw);) {
        // strip comments (a '#' at line start or after whitespace)
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == '#' && (i == 0 || raw[i - 1] == ' ' || raw[i - 1] == '\t')) { raw.resize(i); break; }
        }
        const std::string line = trim(raw);
        if (line.empty()) continue;
        const int indent = static_cast<int>(raw.find_first_not_of(" \t"));

        if (indent == 0) {
            inServices = (line == "services:");
            serviceIndent = keyIndent = -1;
            section.clear();
            continue;
        }
        if (!inServices) continue;

        if (serviceIndent == -1) serviceIndent = indent;
        if (indent == serviceIndent) {
            if (line.back() == ':') services.push_back({unquote(line.substr(0, line.size() - 1)), "", "", {}});
            keyIndent = -1;
            section.clear();
            continue;
        }
        if (services.empty()) continue;
        ComposeService& service = services.back();

        if (keyIndent == -1) keyIndent = indent;

        if (indent == keyIndent) {
            section.clear();
            const size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            const std::string key = line.substr(0, colon);
            const std::string value = unquote(line.substr(colon + 1));

            if (key == "image") service.image = value;
            if (key == "build") {
                if (value.empty()) section = "build";
                else service.context = value;
            }
            if (key == "depends_on") {
                if (value.empty()) section = "depends_on";
                else if (value.front() == '[' && value.back() == ']') {
                    std::istringstream items(value.substr(1, value.size() - 2));
                    for (std::string item; std::getline(items, item, ',');) {
                        if (!unquote(item).empty()) service.dependsOn.push_back(unquote(item));
                    }
                }
            }
            continue;
        }

        // nested lines of "build:" / "depends_on:"
        if (section == "build" && line.rfind("context:", 0) == 0) {
            service.context = unquote(line.substr(8));
        }
        if (section == "depends_on") {
            if (line.rfind("- ", 0) == 0) {
                service.dependsOn.push_back(unquote(line.substr(2)));
            } else if (line.back() == ':') {
                // map form: only the entries directly below depends_on are service names,
                // deeper lines ("condition: service_healthy") have a value after the colon
                service.dependsOn.push_back(unquote(line.substr(0, line.size() - 1)));
            }
        }
    }
    return services;
}

// ---------------------------------------------------------------------------
// .buggai.json
// ---------------------------------------------------------------------------

StackConfig StackDetector::parseConfig(const std::string& jsonText) {
    StackConfig config;
    json j = json::parse(jsonText, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        config.error = "not a JSON object";
        return config;
    }

    auto str = [](const json& o, const char* key) {
        return (o.contains(key) && o[key].is_string()) ? o[key].get<std::string>() : std::string();
    };

    config.stack = str(j, "stack");
    config.sqlDialect = str(j, "sql_dialect");
    if (j.contains("timeout_sec") && j["timeout_sec"].is_number_integer()) {
        config.timeoutSec = std::max(0, j["timeout_sec"].get<int>());
    }

    if (j.contains("components") && j["components"].is_array()) {
        for (const auto& c : j["components"]) {
            if (!c.is_object()) continue;
            StackComponent component;
            component.name = str(c, "name");
            component.kind = str(c, "kind");
            component.root = normalizeRoot(str(c, "root"));
            component.service = str(c, "service");
            component.language = str(c, "language");
            if (c.contains("depends_on") && c["depends_on"].is_array()) {
                for (const auto& d : c["depends_on"]) {
                    if (d.is_string()) component.dependsOn.push_back(d.get<std::string>());
                }
            }
            if (component.kind != "python" && component.kind != "node" && component.kind != "sql") {
                config.error = "component '" + component.name + "': kind must be python, node or sql";
                continue;
            }
            // a root outside the project is never accepted
            if (component.root.rfind("..", 0) == 0 || component.root.front() == '/') {
                config.error = "component '" + component.name + "': root must be inside the project";
                continue;
            }
            config.components.push_back(component);
        }
    }

    if (j.contains("commands") && j["commands"].is_object()) {
        for (const auto& [target, tools] : j["commands"].items()) {
            if (!tools.is_object()) continue;
            for (const auto& [tool, value] : tools.items()) {
                CommandOverride o;
                if (value.is_string()) o.command = value.get<std::string>();
                else if (value.is_boolean() && !value.get<bool>()) o.disabled = true;
                else continue;
                config.commands[target][tool] = o;
            }
        }
    }
    return config;
}

// ---------------------------------------------------------------------------
// detection
// ---------------------------------------------------------------------------

ProjectStack StackDetector::detect(const std::string& projectPath) {
    ProjectStack stack;
    stack.projectRoot = projectPath;

    std::error_code ec;
    const fs::path root(projectPath);
    if (!fs::is_directory(root, ec)) return stack;

    if (fs::exists(root / configFileName())) {
        stack.config = parseConfig(readFile(root / configFileName()));
    }

    for (const char* name : {"docker-compose.yml", "docker-compose.yaml", "compose.yml", "compose.yaml"}) {
        if (!fs::exists(root / name)) continue;
        stack.composeFile = name;
        stack.composeServices = parseCompose(readFile(root / name));
        break;
    }

    const bool configured = !stack.config.components.empty();
    if (configured) {
        // Explicit components replace auto-detection, including their dependencies.
        stack.components = stack.config.components;
        for (auto& c : stack.components) {
            if (c.language.empty()) {
                c.language = c.kind == "node"
                    ? (fs::exists(root / c.root / "tsconfig.json") ? "typescript" : "javascript")
                    : c.kind;
            }
        }
    } else {
        std::vector<std::string> sqlDirs;
        scanMarkers(root, root, 0, stack.components, sqlDirs);
        addSqlComponents(sqlDirs, stack.components);
    }

    assignNames(stack.components);
    assignServices(stack);
    if (!configured) assignDependencies(stack);
    stack.sqlDialect = detectSqlDialect(stack);
    return stack;
}

// ---------------------------------------------------------------------------
// ProjectStack
// ---------------------------------------------------------------------------

bool ProjectStack::isFullstack() const {
    if (config.stack == "fullstack") return !components.empty();
    if (config.stack == "legacy") return false;

    bool python = false, node = false;
    int serviceComponents = 0;   // code components that are clearly separate services
    for (const auto& c : components) {
        python = python || c.kind == "python";
        node = node || c.kind == "node";
        if (!isCode(c)) continue;

        bool composeService = false;
        for (const auto& s : composeServices) composeService = composeService || (s.name == c.service && !s.context.empty());
        if (composeService || firstSegment(c.root) == "services") serviceComponents++;
    }
    return (python && node) || serviceComponents >= 2;
}

const StackComponent* ProjectStack::find(const std::string& name) const {
    for (const auto& c : components) {
        if (c.name == name) return &c;
    }
    return nullptr;
}

const StackComponent* ProjectStack::componentOf(const std::string& relativeFile) const {
    const std::string file = normalizeRoot(relativeFile);
    const std::string kind = kindOfLanguage(LanguageDetector::languageOf(file));

    const StackComponent* best = nullptr;
    auto better = [&](const StackComponent& c) {
        if (!best) return true;
        const bool cKind = c.kind == kind, bestKind = best->kind == kind;
        if (cKind != bestKind) return cKind;                 // matching kind first
        const size_t cLen = c.root == "." ? 0 : c.root.size();
        const size_t bestLen = best->root == "." ? 0 : best->root.size();
        return cLen > bestLen;                               // then the deepest root
    };
    for (const auto& c : components) {
        if (isInside(file, c.root) && better(c)) best = &c;
    }
    return best;
}

std::vector<std::string> ProjectStack::dependentsOf(const std::string& name) const {
    std::vector<std::string> result;
    std::vector<std::string> queue = {name};
    while (!queue.empty()) {
        const std::string current = queue.back();
        queue.pop_back();
        for (const auto& c : components) {
            if (c.name == name) continue;
            if (std::find(c.dependsOn.begin(), c.dependsOn.end(), current) == c.dependsOn.end()) continue;
            if (std::find(result.begin(), result.end(), c.name) != result.end()) continue;
            result.push_back(c.name);
            queue.push_back(c.name);
        }
    }
    return result;
}

std::vector<std::string> ProjectStack::neighboursOf(const std::string& name) const {
    std::vector<std::string> result;
    if (const StackComponent* c = find(name)) {
        for (const auto& d : c->dependsOn) {
            if (find(d)) addUnique(result, d);
        }
    }
    for (const auto& c : components) {
        if (c.name != name && std::find(c.dependsOn.begin(), c.dependsOn.end(), name) != c.dependsOn.end()) {
            addUnique(result, c.name);
        }
    }
    return result;
}
