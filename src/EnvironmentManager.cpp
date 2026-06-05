#include "EnvironmentManager.h"

#include <filesystem>
#include <cstdlib>
#include <iostream>

namespace fs = std::filesystem;

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

    std::string venvPath = projectPath + "/.venv";

    // If the project already contains a virtual environment,
    // reuse it instead of creating a new one.
    if (fs::exists(venvPath)) {

        std::cout << "Virtual environment already exists\n";

        return true;
    }

    std::cout << "Creating virtual environment...\n";

    // Ubuntu/Debian systems require the python3-venv package
    // to create isolated Python environments.
    //
    // Without this package, the debugger cannot create:
    // .venv/bin/python
    //
    // which is needed for isolated dependency handling.
    int venvCheck =system("dpkg -s python3.12-venv > /dev/null 2>&1");

    if (venvCheck != 0) {

        system("sudo apt install -y python3.12-venv");
    }



    // Create a local project-specific virtual environment.
    //
    // Previously the debugger used the global system Python
    // interpreter. This caused dependency conflicts between
    // different projects.
    //
    // Now every Python project uses:
    // .venv/bin/python
    //
    // This is closer to real-world Python development workflows.
    std::string cmd = "cd '" + projectPath + "' && python3 -m venv .venv";

    return system(cmd.c_str()) == 0;
}

std::string EnvironmentManager::getPythonExecutable(
    const std::string& projectPath) {

    // Return the Python executable inside the
    // project virtual environment.
    return projectPath + "/.venv/bin/python";
}

std::string EnvironmentManager::getPipExecutable(
    const std::string& projectPath) {

    // Return the pip executable inside the
    // project virtual environment.
    return projectPath + "/.venv/bin/pip";
}

RunResult EnvironmentManager::installRequirements(
    const std::string& projectPath) {

    std::string requirements = projectPath + "/requirements.txt";

    if (!fs::exists(requirements)) {

        std::cout << "requirements.txt not found, skipping dependency installation\n";
        RunResult result;
        result.exit_code = 0;
        return result;
    }

    std::cout << "Installing requirements.txt dependencies...\n";

    std::string pip = getPipExecutable(projectPath);

    std::string cmd = pip + " install -r '" + requirements + "'";

    cmd += " --progress-bar on";

    std::cout << "Running pip command:\n"<< cmd<< "\n";

    std::cout << "Dependency installation started...\n";


    int result = system(cmd.c_str());

    RunResult res;
    res.exit_code = result;

    return res;

}