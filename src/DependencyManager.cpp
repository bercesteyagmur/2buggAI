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

    if (fs::exists(projectPath + "/requirements.txt")) {
        return true;
    }

    // Also search one level deep inside subfolders (e.g. requirements/)
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(projectPath, ec)) {
        if (ec) break;
        if (!entry.is_directory()) continue;
        if (fs::exists(entry.path().string() + "/requirements.txt")) {
            return true;
        }
    }

    return false;
}

// Returns the path to the first requirements.txt found (root or subfolder)
std::string DependencyManager::findRequirementsTxt(const std::string& projectPath) {
    if (fs::exists(projectPath + "/requirements.txt")) {
        return projectPath + "/requirements.txt";
    }
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(projectPath, ec)) {
        if (ec) break;
        if (!entry.is_directory()) continue;
        std::string candidate = entry.path().string() + "/requirements.txt";
        if (fs::exists(candidate)) {
            return candidate;
        }
    }
    return "";
}

bool DependencyManager::installPythonDependencies(

const std::string& projectPath) {

    std::string requirements = findRequirementsTxt(projectPath);

    if (!requirements.empty()) {

        std::cout << "Installing Python dependencies...\n";

        std::string pip = EnvironmentManager::getPipExecutable(projectPath);

        std::string cmd = pip + " install -r '" + requirements + "'";

        return system(cmd.c_str()) == 0;
    }

    return true;
}

std::vector<std::string>DependencyManager::detectPythonImports(const std::string& projectPath) {

    std::set<std::string> packages;

    std::vector<std::string> stdlib = {
        "os", "sys", "json", "math", "time", "re", "pathlib", "subprocess",
        "threading", "logging", "collections", "datetime", "random", "typing",
        "string", "unittest", "argparse", "hashlib", "urllib", "base64",
        "itertools", "functools", "asyncio", "socket", "ast", "csv", "io",
        "gc", "gzip", "zlib", "shutil", "signal", "struct", "tempfile",
        "traceback", "types", "uuid", "warnings", "contextlib", "dataclasses",
        "secrets", "runpy", "locale", "platform", "importlib", "multiprocessing",
        "concurrent", "ipaddress", "unicodedata", "webbrowser", "copy",
        "weakref", "enum", "abc", "inspect", "textwrap", "pprint", "queue",
        "heapq", "bisect", "array", "decimal", "fractions", "statistics",
        "cmath", "pickle", "shelve", "sqlite3", "configparser", "tomllib",
        "codecs", "encodings", "html", "xml", "http", "ftplib", "smtplib",
        "email", "mailbox", "mimetypes", "uu", "binascii", "quopri",
        "getopt", "getpass", "glob", "fnmatch", "linecache", "fileinput",
        "stat", "filecmp", "pwd", "grp", "pty", "fcntl", "termios",
        "resource", "syslog", "select", "selectors", "ssl", "hmac",
        "difflib", "pdb", "profile", "cProfile", "timeit", "trace",
        "tokenize", "keyword", "token", "dis", "compileall", "py_compile",
        "zipfile", "tarfile", "lzma", "bz2", "zipimport", "pkgutil"
    };


    std::error_code ec;

    for (const auto& entry : fs::recursive_directory_iterator( projectPath, fs::directory_options::skip_permission_denied, ec )) {

            if (ec) {
                std::cerr << "Directory iteration error: "<< ec.message() << "\n";
                ec.clear();
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
                path.find("virtual_env") != std::string::npos ||
                path.find("venv") != std::string::npos ||
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

        // For Django settings files, also scan INSTALLED_APPS entries
        bool isSettings = (entry.path().filename() == "settings.py");
        bool inInstalledApps = false;

        std::string line;

        while (std::getline(file, line)) {

            // Strip inline comments before processing
            size_t commentPos = line.find(" #");
            if (commentPos != std::string::npos) {
                line = line.substr(0, commentPos);
            }

            // Track whether we are inside the INSTALLED_APPS block
            if (isSettings) {
                if (line.find("INSTALLED_APPS") != std::string::npos &&
                    line.find("=") != std::string::npos) {
                    inInstalledApps = true;
                }
                if (inInstalledApps) {
                    // Closing bracket ends the block
                    if (line.find("]") != std::string::npos) {
                        inInstalledApps = false;
                    }
                    // Extract quoted app names: 'app_name' or "app_name"
                    std::regex appRegex(R"(['"]([a-zA-Z][a-zA-Z0-9_]*)['"])");
                    std::sregex_iterator it(line.begin(), line.end(), appRegex);
                    std::sregex_iterator end;
                    for (; it != end; ++it) {
                        std::string app = (*it)[1].str();
                        // Skip Django built-in apps
                        if (app.starts_with("django.") || app == "django") continue;
                        if (fs::exists(projectPath + "/" + app)) continue;
                        if (fs::exists(projectPath + "/" + app + ".py")) continue;
                        if (std::find(stdlib.begin(), stdlib.end(), app) != stdlib.end()) continue;
                        packages.insert(app);
                    }
                }
            }

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

                    // only keep the top-level package name
                    size_t dotPos = pkg.find(".");
                    if (dotPos != std::string::npos) {
                        pkg = pkg.substr(0, dotPos);
                    }

                    // skip empty, private, relative, or local file/folder modules
                    if (pkg.empty() || pkg.starts_with("_") || pkg.starts_with(".")) continue;
                    if (fs::exists(projectPath + "/" + pkg + ".py")) continue;
                    if (fs::exists(projectPath + "/" + pkg)) continue;
                    if (std::find(stdlib.begin(), stdlib.end(), pkg) != stdlib.end()) continue;

                    packages.insert(pkg);
                }
            }

            // from x import y
            else if (line.starts_with("from ")) {

                std::string pkg = line.substr(5);

                size_t space = pkg.find(" ");
                if (space != std::string::npos) {
                    pkg = pkg.substr(0, space);
                }

                // skip relative imports (from . import x  or  from .module import x)
                if (pkg.starts_with(".")) continue;

                // only keep the top-level package name
                size_t dotPos = pkg.find(".");
                if (dotPos != std::string::npos) {
                    pkg = pkg.substr(0, dotPos);
                }

                // skip empty, private, or local file/folder modules
                if (pkg.empty() || pkg.starts_with("_")) continue;
                if (fs::exists(projectPath + "/" + pkg + ".py")) continue;
                if (fs::exists(projectPath + "/" + pkg)) continue;
                if (std::find(stdlib.begin(), stdlib.end(), pkg) != stdlib.end()) continue;

                packages.insert(pkg);
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

    std::string pip = EnvironmentManager::getPipExecutable(projectPath);

    // import name -> pip package name mapping
    auto mapPackage = [](std::string pkg) -> std::string {
        if (pkg == "cv2")                          return "opencv-python";
        if (pkg == "PIL")                          return "pillow";
        if (pkg == "sklearn")                      return "scikit-learn";
        if (pkg == "faiss")                        return "faiss-cpu";
        if (pkg == "skimage")                      return "scikit-image";
        if (pkg == "bs4")                          return "beautifulsoup4";
        if (pkg == "yaml")                         return "pyyaml";
        if (pkg == "dotenv")                       return "python-dotenv";
        if (pkg == "environ")                      return "django-environ";
        if (pkg == "Crypto")                       return "pycryptodome";
        if (pkg == "gi")                           return "PyGObject";
        if (pkg == "wx")                           return "wxPython";
        if (pkg == "factory")                      return "factory-boy";
        if (pkg == "rest_framework")               return "djangorestframework";
        if (pkg == "django_filters")               return "django-filter";
        if (pkg == "simple_history")               return "django-simple-history";
        if (pkg == "webpack_loader")               return "django-webpack-loader";
        if (pkg == "smart_selects")                return "django-smart-selects";
        if (pkg == "mapbox_location_field")        return "django-mapbox-location-field";
        if (pkg == "admin_auto_filters")           return "django-admin-autocomplete-filter";
        if (pkg == "admin_numeric_filter")         return "django-admin-numeric-filter";
        if (pkg == "django_admin_listfilter_dropdown") return "django-admin-listfilter-dropdown";
        if (pkg == "computed_property")            return "django-computed-property";
        if (pkg == "easy_select2")                 return "django-easy-select2";
        if (pkg == "admin_interface")              return "django-admin-interface";
        if (pkg == "colorfield")                   return "django-colorfield";
        if (pkg == "corsheaders")                  return "django-cors-headers";
        if (pkg == "storages")                     return "django-storages";
        if (pkg == "crispy_forms")                 return "django-crispy-forms";
        if (pkg == "allauth")                      return "django-allauth";
        if (pkg == "guardian")                     return "django-guardian";
        if (pkg == "taggit")                       return "django-taggit";
        if (pkg == "mptt")                         return "django-mptt";
        if (pkg == "celery")                       return "celery";
        return pkg;
    };

    std::cout << "Installing detected Python packages...\n";

    // Install each package separately so one failure does not block the rest
    bool allOk = true;
    for (auto pkg : packages) {
        std::string mapped = mapPackage(pkg);
        std::string cmd = pip + " install " + mapped;
        std::cout << "PIP COMMAND: " << cmd << "\n";
        int result = system(cmd.c_str());
        if (result != 0) {
            // psycopg2 needs system PostgreSQL libs to compile — retry with binary wheel
            if (mapped == "psycopg2") {
                std::cout << "psycopg2 build failed, retrying with psycopg2-binary...\n";
                std::string retryCmd = pip + " install psycopg2-binary";
                std::cout << "PIP COMMAND: " << retryCmd << "\n";
                result = system(retryCmd.c_str());
            }
            if (result != 0) {
                std::cout << "Warning: could not install '" << mapped << "', continuing...\n";
                allOk = false;
            }
        }
    }

    return allOk;
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