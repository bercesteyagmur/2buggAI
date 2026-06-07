
#ifndef INC_2BUGGAI_INNOLAB_ENVIRONMENTMANAGER_H
#define INC_2BUGGAI_INNOLAB_ENVIRONMENTMANAGER_H

#include <string>
#include "ProcessRunner.h"
#include "DependencyManager.h"

class EnvironmentManager {

public:

    // Step 1:
    // Check if pip exists in the current Python environment.
    // Example:
    // python3 -m pip --version
    //
    // If pip is missing, automatically install it:
    // sudo apt install python3-pip
    static bool ensurePipInstalled();

    // Step 2:
    // Create a Python virtual environment inside the project.
    //
    // Example:
    // python3 -m venv .venv
    //
    // This isolates project dependencies from the global system.
    static bool createVirtualEnv(
        const std::string& projectPath
    );

    // Step 3:
    // Return the Python executable inside the virtual environment.
    //
    // Example:
    // .venv/bin/python
    //
    // The debugger will execute Python projects using this interpreter.
    static std::string getPythonExecutable(const std::string& projectPath);

    // Step 4:
    // Return the pip executable inside the virtual environment.
    //
    // Example:
    // .venv/bin/pip
    //
    // Dependencies will be installed using this pip executable.
    static std::string getPipExecutable(const std::string& projectPath);

    // Step 5:
    // Install Python dependencies automatically.
    //
    // First try:
    // pip install -r requirements.txt
    //
    // If no requirements.txt exists:
    // scan Python imports automatically
    // and install detected packages.
    static RunResult  installRequirements(const std::string& projectPath);
};

#endif //INC_2BUGGAI_INNOLAB_ENVIRONMENTMANAGER_H
