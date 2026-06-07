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


    std::error_code ec;

    for (const auto& entry : fs::recursive_directory_iterator( projectPath, fs::directory_options::skip_permission_denied, ec )) {

            if (ec) {
                std::cerr << "Directory iteration error: "<< ec.message() << "\n";
                continue;
            }

            // only real python files
            if (!entry.is_regular_file()) {
                continue;
            }

            if (entry.path().extension() != ".py") {
                continue;
            }

            std::string path = entry.path().string();

            // skip irrelevant folders
            if (path.find(".venv") != std::string::npos ||
                path.find("__pycache__") != std::string::npos ||
                path.find(".git") != std::string::npos ||
                path.find("dataset") != std::string::npos ||
                path.find("images") != std::string::npos ||
                path.find("outputs") != std::string::npos) {
                continue;
                }
        std::ifstream file(entry.path());

        if (!file) {
            continue;
        }

        std::string line;

        while (std::getline(file, line)) {

            // import x
            if (line.starts_with("import ")) {

                std::string imports = line.substr(7);

                std::stringstream ss(imports);

                std::string pkg;

                while (std::getline(ss, pkg, ',')) {

                    // trim spaces
                    pkg.erase(0, pkg.find_first_not_of(" \t"));
                    pkg.erase(pkg.find_last_not_of(" \t") + 1);

                    // remove aliases
                    size_t asPos = pkg.find(" as ");

                    if (asPos != std::string::npos) {
                        pkg = pkg.substr(0, asPos);
                    }

                    if (fs::exists(projectPath + "/" + pkg + ".py")) {
                        continue;
                    }

                    if (fs::exists(projectPath + "/" + pkg)) {
                        continue;
                    }

                    if (!pkg.starts_with("_") &&
                        std::find(stdlib.begin(),stdlib.end(),pkg) == stdlib.end()) {

                        packages.insert(pkg);
                                  }
                }
            }

            // from x import y
            else if (line.starts_with("from ")) {

                std::string pkg = line.substr(5);

                size_t space = pkg.find(" ");

                if (space != std::string::npos) {
                    pkg = pkg.substr(0, space);
                }

                if (fs::exists(projectPath + "/" + pkg + ".py")) {
                    continue;
                }

                if (fs::exists(projectPath + "/" + pkg)) {
                    continue;
                }

                if (!pkg.starts_with("_") &&
                    std::find(stdlib.begin(),stdlib.end(),pkg) == stdlib.end()) {

                    packages.insert(pkg);
                              }
            }
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

    for (auto pkg : packages) {
        // import name -> pip package mapping
        if (pkg == "cv2") {
            pkg = "opencv-python";
        }

        else if (pkg == "PIL") {
            pkg = "pillow";
        }

        else if (pkg == "sklearn") {
            pkg = "scikit-learn";
        }
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