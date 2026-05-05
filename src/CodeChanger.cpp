#include "CodeChanger.h"
#include <fstream>
#include <filesystem>

bool CodeChanger::apply_fix(FixResult fix_result){

    std::string filePath = fix_result.file_path;

    if (filePath.empty()) {
        throw std::runtime_error("No file path provided in FixResult.");
    }

    //create backup before overwriting
    std::string backupFile = filePath + ".bak";

    std::filesystem::copy_file(filePath, backupFile, std::filesystem::copy_options::overwrite_existing);

    if (!std::filesystem::exists(backupFile)) {
        throw std::runtime_error("Failed to create backup file: " + backupFile);
    }


    // Write the fixed code back to the original file
    std::ofstream outFile(filePath, std::ios::trunc);

    if (!outFile.is_open()) {
        throw std::runtime_error("Failed to open file for writing: " + filePath);
    }

    outFile << fix_result.fixed_code;
    outFile.close();



    return true;
}