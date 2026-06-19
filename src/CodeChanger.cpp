#include "CodeChanger.h"
#include <fstream>
#include <filesystem>

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

    std::filesystem::path backupFile = backupDir / (fullPath.filename().string() + ".bak");

    try {
        std::filesystem::copy_file(fullPath, backupFile, std::filesystem::copy_options::overwrite_existing);
    } catch (const std::filesystem::filesystem_error& e) {
        throw std::runtime_error("Failed to create backup for " + fullPath.string() + ": " + e.what());
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