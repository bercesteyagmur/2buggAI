#ifndef INC_2BUGGAI_WEBENVIRONMENTMANAGER_H
#define INC_2BUGGAI_WEBENVIRONMENTMANAGER_H

#include <string>
#include <vector>
#include "ProcessRunner.h"

struct SetupStep {
    std::string name;
    std::string command;
    RunResult result;
    bool ok() const { return result.exit_code == 0; }
};

class WebEnvironmentManager {
public:
    static bool isWebProject(const std::string& projectPath);

    static std::vector<SetupStep> setupProject(const std::string& projectPath);

    static std::vector<SetupStep> runChecks(const std::string& projectPath, bool runtime);

    static void printSummary(const std::vector<SetupStep>& steps, const std::string& title = "SETUP SUMMARY");

private:
    static bool ensurePhp(const std::string& projectPath, const std::string& dbConnection);
    static bool ensureComposer();
    static bool ensureNode(const std::string& projectPath);
    static bool ensurePackageManager(const std::string& pm);

    static void prepareEnvFile(const std::string& projectPath);
    static bool setupDatabase(const std::string& projectPath, const std::string& dbConnection);

    static std::string typeCheckCommand(const std::string& projectPath);
    static std::string testCommand(const std::string& projectPath);
    static SetupStep smokeTest(const std::string& projectPath);
    static SetupStep prepareTestDatabase(const std::string& projectPath);
    static std::string readCommandOutput(const std::string& command);

    static std::string detectPackageManager(const std::string& projectPath);
    static std::string readEnvValue(const std::string& envFile, const std::string& key);
    static void writeEnvValue(const std::string& envFile, const std::string& key, const std::string& value);
    static bool commandExists(const std::string& command);
    static bool aptInstall(const std::vector<std::string>& packages);
    static SetupStep runStep(const std::string& name, const std::string& projectPath, const std::string& command);
};

#endif //INC_2BUGGAI_WEBENVIRONMENTMANAGER_H
