#include "DependencyManager.h"

#include <filesystem>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <sstream>
#include <regex>
#include <set>
#include "EnvironmentManager.h"



namespace fs = std::filesystem;

bool DependencyManager::hasRequirementsTxt(
    const std::string& projectPath) {

    return fs::exists(projectPath + "/requirements.txt");
}

bool DependencyManager::installPythonDependencies(

const std::string& projectPath) {

    if (hasRequirementsTxt(projectPath)) {

        std::cout << "Installing Python dependencies...\n";

        std::string pip = EnvironmentManager::getPipExecutable(projectPath);

        std::string requirements = projectPath + "/requirements.txt";

        std::string cmd =pip + " install -r '" + requirements + "'";


        return system(cmd.c_str()) == 0;
    }

    return true;
}

std::vector<std::string>DependencyManager::detectPythonImports(const std::string& projectPath) {

    std::set<std::string> packages;

    std::vector<std::string> stdlib = {
        "os",
        "sys",
        "json",
        "math",
        "time",
        "re",
        "pathlib",
        "subprocess",
        "threading",
        "logging",
        "collections",
        "datetime",
        "random",
        "typing"
    };

    std::regex importRegex(R"(import\s+([a-zA-Z0-9_]+))");

    std::regex fromRegex(R"(from\s+([a-zA-Z0-9_]+))");

    std::error_code ec;

    for (const auto& entry : fs::recursive_directory_iterator( projectPath, fs::directory_options::skip_permission_denied, ec )) {

        std::string path = entry.path().string();

        if (path.find(".venv") != std::string::npos ||path.find("__pycache__") != std::string::npos || path.find(".git") != std::string::npos) {
            continue;
            }

        std::cout << "Scanning: " << entry.path() << "\n";

        if (ec) {
            std::cerr << "Directory iteration error: "<< ec.message()<< "\n";
            continue;
        }

        if (!entry.is_regular_file()) {
            continue;
        }

        if (entry.path().extension() != ".py") {
            continue;
        }

        std::ifstream file(entry.path());

        if (!file) {
            continue;
        }

        std::stringstream buffer;

        buffer << file.rdbuf();

        std::string content = buffer.str();

        std::smatch match;

        std::string::const_iterator searchStart(content.cbegin());

        while (std::regex_search(searchStart,content.cend(),match,importRegex)) {
            std::string pkg = match[1];

            // Ignore private/internal Python symbols
            // such as:
            // _BaseGenericAlias
            // _abc
            // _typeshed
            if (pkg.starts_with("_")) {
                continue;
            }

            if (fs::exists(projectPath + "/" + pkg + ".py")) {
                continue;
            }

            if (fs::exists(projectPath + "/" + pkg)) {
                continue;
            }

            if (std::find(stdlib.begin(),stdlib.end(),pkg) == stdlib.end()) {
                packages.insert(pkg);
            }

            searchStart = match.suffix().first;
        }

        searchStart = content.cbegin();

        while (std::regex_search(searchStart,content.cend(),match,fromRegex)) {
            std::string pkg = match[1];

            if (fs::exists(projectPath + "/" + pkg + ".py")) {
                continue;
            }

            if (fs::exists(projectPath + "/" + pkg)) {
                continue;
            }


            if (pkg.starts_with("_")) {
                continue;
            }

            if (std::find(stdlib.begin(),stdlib.end(),pkg) == stdlib.end()) {
                packages.insert(pkg);
            }

            searchStart = match.suffix().first;
        }
    }

    return std::vector<std::string>(packages.begin(),packages.end());
}

bool DependencyManager::installPythonPackages(const std::string& projectPath,const std::vector<std::string>& packages) {

    if (packages.empty()) {

        std::cout << "No external Python packages detected\n";

        return true;
    }

    std::string cmd =projectPath + "/.venv/bin/pip install";

    for (const auto& pkg : packages) {
        cmd += " " + pkg;
    }

    std::cout << "Installing detected Python packages...\n";

    std::cout << "PIP COMMAND: "<< cmd<< "\n";

    return system(cmd.c_str()) == 0;
}

bool DependencyManager::removeBrokenRequirement(const std::string& projectPath,const std::string& pipOutput) {

    std::regex brokenRegex(R"(No matching distribution found for ([^\s]+))");

    std::smatch match;

    if (!std::regex_search(pipOutput,match,brokenRegex)) {
        return false;
    }

    std::string brokenPackage = match[1];

    std::cout << "Broken package detected: "
              << brokenPackage
              << "\n";

    std::string requirementsPath = projectPath + "/requirements.txt";

    std::ifstream in(requirementsPath);

    if (!in) {
        return false;
    }

    std::vector<std::string> lines;

    std::string line;

    while (std::getline(in, line)) {

        std::string packageName = brokenPackage;

        size_t versionPos = packageName.find("==");

        if (versionPos != std::string::npos) {

            packageName =packageName.substr(0, versionPos);
        }
        if (line.find(packageName) == std::string::npos) {

            lines.push_back(line);
            }
    }

    in.close();

    std::ofstream out(requirementsPath);

    for (const auto& l : lines) {
        out << l << "\n";
    }

    std::cout << "Removed broken dependency from requirements.txt\n";

    return true;
}