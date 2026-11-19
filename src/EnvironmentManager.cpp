#include "EnvironmentManager.h"
#include "ProcessRunner.h"
#include "ShellQuote.h"
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
    // sudo -n: never wait for a password (a run without a terminal would hang). With
    // passwordless sudo pip is installed, otherwise the user gets the command to run.
    std::cout << "Installing pip...\n";

    if (system("sudo -n apt-get install -y python3-pip > /dev/null 2>&1") == 0) return true;
    std::cout << "[UMGEBUNGSPROBLEM] pip fehlt und konnte ohne Passwort nicht installiert werden.\n"
              << "  Bitte ausführen: sudo apt install python3-pip\n";
    return false;
}

bool EnvironmentManager::createVirtualEnv(
    const std::string& projectPath) {

    std::string venvPath = resolveVenvPath(projectPath);

    if (fs::exists(venvPath)) {

        std::cout << "Virtual environment already exists\n";

        return true;
    }

    std::cout << "Creating virtual environment at: " << venvPath << "\n";

    // Just try it: the venv module is what matters, whatever Python version / distribution
    // provides it (the old check asked dpkg for "python3.12-venv" and otherwise ran sudo).
    const std::string cmd = "python3 -m venv " + ShellQuote::quote(venvPath) + " > /dev/null 2>&1";
    if (system(cmd.c_str()) == 0) return true;

    // A failed attempt can leave a half-created folder behind.
    std::error_code ec;
    fs::remove_all(venvPath, ec);

    // Missing venv package (Debian/Ubuntu): install it only with passwordless sudo, never wait
    // for a password.
    std::cout << "python3 -m venv failed, trying to install python3-venv (sudo -n)...\n";
    if (system("sudo -n apt-get install -y python3-venv > /dev/null 2>&1") == 0 && system(cmd.c_str()) == 0) {
        return true;
    }
    fs::remove_all(venvPath, ec);
    std::cout << "[UMGEBUNGSPROBLEM] Das Python-Modul venv fehlt und konnte ohne Passwort nicht installiert werden.\n"
              << "  Bitte ausführen: sudo apt install python3-venv\n";
    return false;
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

    std::string cmd = ShellQuote::quote(pip) + " install -r " + ShellQuote::quote(requirements);

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
        if (system(binaryFix.c_str()) != 0) {}  // Ergebnis bewusst ignoriert
        std::string retryFull = pip + " install -r '" + requirements + "' --progress-bar on";
        result = system(retryFull.c_str());
    }

    RunResult res;
    res.exit_code = result;

    return res;

}