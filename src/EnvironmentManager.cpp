#include "EnvironmentManager.h"
#include "ProcessRunner.h"
#include <filesystem>
#include <cstdlib>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

// Returns a Linux-native venv path when the project is on a Windows
// filesystem (/mnt/c/...), where venv scripts are not executable from WSL.
static std::string resolveVenvPath(const std::string& projectPath) {
    if (projectPath.rfind("/mnt/", 0) == 0) {
        // Hash the project path to get a unique stable venv directory
        size_t h = std::hash<std::string>{}(projectPath);
        return "/tmp/buggy-venv-" + std::to_string(h);
    }
    return projectPath + "/.venv";
}

bool EnvironmentManager::ensurePipInstalled() {

    // Check whether pip already exists in the current
    // Python environment.
    //
    // We use:
    // python3 -m pip --version
    //
    // If this command succeeds, pip is already installed.
    int result = system("python3 -m pip --version > /dev/null 2>&1");

    if (result == 0) {

        std::cout << "pip already installed\n";

        return true;
    }

    // If pip is missing, automatically install it.
    //
    // This is necessary because many Python projects
    // require external dependencies such as:
    // Django, Flask, NumPy, Pandas, etc.
    std::cout << "Installing pip...\n";

    return system("sudo apt install -y python3-pip") == 0;
}

bool EnvironmentManager::createVirtualEnv(
    const std::string& projectPath) {

    std::string venvPath = resolveVenvPath(projectPath);

    if (fs::exists(venvPath)) {

        std::cout << "Virtual environment already exists\n";

        return true;
    }

    std::cout << "Creating virtual environment at: " << venvPath << "\n";

    int venvCheck = system("dpkg -s python3.12-venv > /dev/null 2>&1");

    if (venvCheck != 0) {

        system("sudo apt install -y python3.12-venv");
    }

    std::string cmd = "python3 -m venv '" + venvPath + "'";

    return system(cmd.c_str()) == 0;
}

std::string EnvironmentManager::getPythonExecutable(
    const std::string& projectPath) {

    return resolveVenvPath(projectPath) + "/bin/python";
}

std::string EnvironmentManager::getPipExecutable(
    const std::string& projectPath) {

    return resolveVenvPath(projectPath) + "/bin/pip";
}

RunResult EnvironmentManager::installRequirements(
    const std::string& projectPath) {

    std::string requirements = DependencyManager::findRequirementsTxt(projectPath);

    if (requirements.empty()) {

        std::cout << "requirements.txt not found, skipping dependency installation\n";
        std::cout << "Starting import-based dependency detection...\n";

        auto packages = DependencyManager::detectPythonImports(projectPath);

        DependencyManager::installPythonPackages(projectPath, packages);

        RunResult result;
        result.exit_code = 0;
        return result;
    }

    std::cout << "Installing requirements.txt dependencies from: " << requirements << "\n";

    std::string pip = getPipExecutable(projectPath);

    std::string cmd = pip + " install -r '" + requirements + "'";

    cmd += " --progress-bar on";

    std::cout << "Running pip command:\n" << cmd << "\n";

    std::cout << "Dependency installation started...\n";

    int result = system(cmd.c_str());

    // If requirements.txt install failed, retry with psycopg2-binary substitution
    if (result != 0) {
        std::cout << "Retrying with psycopg2-binary substitution...\n";
        std::string retryCmd = pip + " install -r '" + requirements +
                               "' --progress-bar on"
                               " || " + pip + " install psycopg2-binary";
        // Install psycopg2-binary as override and retry the full requirements
        std::string binaryFix = pip + " install psycopg2-binary 2>/dev/null";
        system(binaryFix.c_str());
        std::string retryFull = pip + " install -r '" + requirements + "' --progress-bar on";
        result = system(retryFull.c_str());
    }

    RunResult res;
    res.exit_code = result;

    return res;

}