#include "FileCollector.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>

namespace fs = std::filesystem;

bool FileCollector::isSourceFile(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    return ext == ".c" || ext == ".cpp" || ext == ".cc"
        || ext == ".h" || ext == ".hpp"
        || ext == ".java"
        || ext == ".py";
}

bool FileCollector::isBuildDirectory(const std::string& path) {

    // Check if path contains common build directories
    // These folders are generated and should be ignored during analysis
    if (path.find("cmake-build") < path.size()) return true;
    if (path.find("build") < path.size()) return true;
    if (path.find(".venv") < path.size()) return true;
    if (path.find("venv") < path.size()) return true;
    if (path.find("site-packages") < path.size()) return true;

    return false;
}

std::vector<std::string> FileCollector::collectSourceFiles(const std::string& path, bool recursive) {
    std::vector<std::string> files;

    if (recursive) {
        for (const auto& entry : fs::recursive_directory_iterator(path)) {
            std::string pathStr = entry.path().string();
            if (isBuildDirectory(pathStr)) {
                continue;
            }

            if (entry.is_regular_file() && isSourceFile(entry.path().string())) {
                files.push_back(entry.path().string());
            }
        }
    } else {
        for (const auto& entry : fs::directory_iterator(path)) {
            if (entry.is_regular_file() && isSourceFile(entry.path().string())) {
                files.push_back(entry.path().string());
            }
        }
    }

    std::sort(files.begin(), files.end());
    return files;
}

std::string FileCollector::readSourceCode(const std::string& path) {
    std::ifstream file(path);
    if (!file) return "[ERROR: Could not read file]";

    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

int FileCollector::countLines(const std::string& filePath) {
    std::ifstream file(filePath);
    if (!file) return 0;

    return std::count(
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>(),
        '\n'
    );
}

std::string FileCollector::collectSourceCode(const std::vector<std::string>& files) {
    std::stringstream collected;

    for (const auto& file : files) {
        collected << "===== FILE: " << file << " =====\n";
        collected << readSourceCode(file) << "\n\n";
    }

    return collected.str();
}

std::vector<std::string> FileCollector::collectIncludeDirs(const std::string& path, bool recursive) {
    std::vector<std::string> includeDirs;

    auto process = [&](const fs::path& p) {
        std::string ext = p.extension().string();

        if (ext == ".h" || ext == ".hpp") {
            std::string dir = p.parent_path().string();

            if (std::find(includeDirs.begin(), includeDirs.end(), dir) == includeDirs.end()) {
                includeDirs.push_back(dir);
            }
        }
    };

    if (recursive) {
        for (const auto& entry : fs::recursive_directory_iterator(path)) {
            std::string pathStr = entry.path().string();

            if (isBuildDirectory(pathStr)) {
                continue;
            }


            if (entry.is_regular_file()) {
                process(entry.path());
            }
        }
    } else {
        for (const auto& entry : fs::directory_iterator(path)) {
            if (entry.is_regular_file()) {
                process(entry.path());
            }
        }
    }

    return includeDirs;
}




