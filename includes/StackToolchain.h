#ifndef INC_2BUGGAI_STACKTOOLCHAIN_H
#define INC_2BUGGAI_STACKTOOLCHAIN_H

#include <map>
#include <string>
#include <vector>
#include "StackDetector.h"
#include "WebEnvironmentManager.h"   // SetupStep

// One command of a component's toolchain. It becomes one SetupStep named
// "<component>: <tool>".
struct StackCommand {
    std::string component;
    std::string tool;         // "pytest", "tsc", "sql", ... selects the output parser
    std::string phase;        // "setup" | "check"
    std::string command;      // shell command, executed in the component root
    std::string skipReason;   // non-empty: the command cannot run on this machine
};

// Resolves and runs the build / lint / test commands of every component.
//
// Safety rules implemented here:
//  - every command runs under `timeout`
//  - nothing is installed system-wide and sudo is never used; a missing tool skips the step
//  - SQL is only executed against an in-memory SQLite database
//  - check commands get DATABASE_URL=sqlite:// so tests cannot reach a configured database
class StackToolchain {
public:
    explicit StackToolchain(ProjectStack stack);

    // Commands of one component for a phase: defaults for its kind, then the overrides
    // from .buggai.json ("commands"). Depends on what is installed and on the files
    // present (e.g. no pytest step without tests).
    std::vector<StackCommand> commandsFor(const StackComponent& component, const std::string& phase) const;

    // Dependency installation for all components.
    std::vector<SetupStep> setup();

    // Runs the checks. changedFile empty: all components. Otherwise only the component
    // that owns the file and every component that depends on it are re-run; the other
    // components keep their previous result. Always returns the steps of all components.
    std::vector<SetupStep> check(const std::string& changedFile);

    // Component names whose checks must be re-run after a change to the file.
    std::vector<std::string> affectedComponents(const std::string& changedFile) const;

    // "<component>: <tool>" and back.
    static std::string stepName(const std::string& component, const std::string& tool);
    static bool splitStepName(const std::string& step, std::string& component, std::string& tool);

    // Steps that could not run, e.g. "frontend: tsc (node is not installed)".
    const std::vector<std::string>& skipped() const { return skipped_; }

    const ProjectStack& stack() const { return stack_; }

    // Python source of the SQL dry run (stdlib sqlite3, in-memory database).
    static const char* sqlCheckScript();

private:
    std::vector<StackCommand> pythonCommands(const StackComponent& c, const std::string& phase) const;
    std::vector<StackCommand> nodeCommands(const StackComponent& c, const std::string& phase) const;
    std::vector<StackCommand> sqlCommands(const StackComponent& c, const std::string& phase) const;

    std::string absoluteRoot(const StackComponent& c) const;
    std::string pythonOf(const StackComponent& c) const;   // interpreter used for the component
    int timeoutFor(const std::string& phase) const;
    std::vector<SetupStep> runCommands(const std::vector<StackCommand>& commands);

    ProjectStack stack_;
    std::map<std::string, std::vector<SetupStep>> lastChecks_;   // component -> last check steps
    std::vector<std::string> skipped_;
};

#endif //INC_2BUGGAI_STACKTOOLCHAIN_H
