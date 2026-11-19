#include "CodeChanger.h"
#include "UnifiedDiff.h"
#include <fstream>
#include <filesystem>
#include <sstream>

bool CodeChanger::apply_fix(FixResult fix_result){

    if (fix_result.file_path.empty()) {
        throw std::runtime_error("No file path provided in FixResult.");
    }

    // Relativen Pfad ggf. mit dem Projekt-Basisverzeichnis kombinieren
    std::filesystem::path fullPath = fix_result.file_path;
    if (fullPath.is_relative() && !basePath_.empty()) {
        fullPath = std::filesystem::path(basePath_) / fullPath;
    }

    if (!std::filesystem::exists(fullPath)) {
        throw std::runtime_error("Target file does not exist: " + fullPath.string());
    }

    // Backup-Ordner bestimmen: basePath_/backups/<relative Unterordner>/
    // Dadurch landet z.B. src/Foo.java als backups/src/Foo.java.bak
    // und Namenskollisionen bei gleichen Dateinamen werden vermieden.
    std::filesystem::path backupDir;
    if (!basePath_.empty()) {
        std::error_code relEc;
        auto relPath = std::filesystem::relative(fullPath, basePath_, relEc);
        backupDir = relEc
            ? fullPath.parent_path() / "backups"
            : std::filesystem::path(basePath_) / "backups" / relPath.parent_path();
    } else {
        backupDir = fullPath.parent_path() / "backups";
    }

    std::filesystem::create_directories(backupDir);

    {
        std::ifstream current(fullPath, std::ios::binary);
        std::stringstream before;
        before << current.rdbuf();
        previous_ = before.str();
    }

    std::filesystem::path backupFile = backupDir / (fullPath.filename().string() + ".bak");

    try {
        std::filesystem::copy_file(fullPath, backupFile, std::filesystem::copy_options::overwrite_existing);
    } catch (const std::filesystem::filesystem_error& e) {
        throw std::runtime_error("Failed to create backup for " + fullPath.string() + ": " + e.what());
    }


    // In addition to the .bak (previous version, overwritten by the next fix):
    //  <name>.orig      state before the very first fix, never overwritten
    //  <name>.<n>.diff  unified diff of every applied fix, in order
    // Both are best effort; a failure here must not prevent the fix.
    try {
        const std::string name = fullPath.filename().string();
        const std::filesystem::path origFile = backupDir / (name + ".orig");
        if (!std::filesystem::exists(origFile)) std::filesystem::copy_file(fullPath, origFile);

        int n = 1;
        while (std::filesystem::exists(backupDir / (name + "." + std::to_string(n) + ".diff"))) ++n;
        std::ofstream(backupDir / (name + "." + std::to_string(n) + ".diff"))
            << unified_diff(previous_, fix_result.fixed_code, fix_result.file_path);
    } catch (const std::exception&) {
    }

    // Write the fixed code back to the original file
    std::ofstream outFile(fullPath, std::ios::trunc);

    if (!outFile.is_open()) {
        throw std::runtime_error("Failed to open file for writing: " + fullPath.string());
    }

    outFile << fix_result.fixed_code;
    outFile.close();

    return true;
}