#ifndef INC_2BUGGAI_STACKDETECTOR_H
#define INC_2BUGGAI_STACKDETECTOR_H

#include <map>
#include <string>
#include <vector>

// One independently checkable part of a repository: a Python backend, a Node (JS/TS)
// frontend or service, or a set of SQL schema/migration files.
struct StackComponent {
    std::string name;       // unique within the project, e.g. "backend", "frontend", "db"
    std::string service;    // docker-compose service or services/<name>; may be empty
    std::string kind;       // "python" | "node" | "sql"
    std::string language;   // "python" | "typescript" | "javascript" | "sql"
    std::string root;       // relative to the project root, "." for the root itself
    std::vector<std::string> dependsOn;   // names of the components this one needs
};

struct ComposeService {
    std::string name;
    std::string context;    // build context relative to the compose file, empty if image-only
    std::string image;
    std::vector<std::string> dependsOn;
};

// One entry of "commands" in .buggai.json: replace/add a tool command or disable a tool.
struct CommandOverride {
    bool disabled = false;
    std::string command;
};

// Optional per-project configuration, read from <project>/.buggai.json.
struct StackConfig {
    std::string stack;        // "fullstack" | "legacy" | "" (auto)
    int timeoutSec = 0;       // 0 = default
    std::string sqlDialect;   // "sqlite" | "postgres" | "mysql" | "" (auto)
    std::vector<StackComponent> components;   // non-empty: replaces auto-detection
    // key: component name or kind  ->  tool name  ->  override
    std::map<std::string, std::map<std::string, CommandOverride>> commands;
    std::string error;        // set when the file exists but could not be parsed
};

struct ProjectStack {
    std::string projectRoot;
    std::vector<StackComponent> components;
    std::string composeFile;                    // relative path, empty if none
    std::vector<ComposeService> composeServices;
    std::string sqlDialect = "sqlite";
    StackConfig config;

    // True when the repository should be handled by StackPipeline instead of the
    // single-language paths.
    bool isFullstack() const;

    const StackComponent* find(const std::string& name) const;

    // Component that owns a project-relative file: deepest root containing the file,
    // preferring a component whose kind matches the file's language.
    const StackComponent* componentOf(const std::string& relativeFile) const;

    // Components that (transitively) depend on the given one.
    std::vector<std::string> dependentsOf(const std::string& name) const;

    // Direct neighbours across a boundary: dependencies and direct dependents.
    std::vector<std::string> neighboursOf(const std::string& name) const;
};

class StackDetector {
public:
    static ProjectStack detect(const std::string& projectPath);

    // Minimal docker-compose reader (services, build context, image, depends_on).
    static std::vector<ComposeService> parseCompose(const std::string& yamlText);

    static StackConfig parseConfig(const std::string& jsonText);

    static const char* configFileName() { return ".buggai.json"; }
};

#endif //INC_2BUGGAI_STACKDETECTOR_H
