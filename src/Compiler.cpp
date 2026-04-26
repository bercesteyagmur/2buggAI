#include "Compiler.h"
#include "ShellQuote.h"
#include "ProcessRunner.h"

#include <filesystem>
#include <iostream>
#include <unistd.h>
#include <array>

std::string Compiler::compileIfNeeded(const std::string& targetPath) {
    namespace fs = std::filesystem;

    if (!fs::exists(targetPath)) {
        //throw std::runtime_error("File does not exist: " + targetPath);
        std::cout << "Compilation failed, continuing with error analysis...\n";
        return "";
    }

    std::string ext = fs::path(targetPath).extension();

    if (ext == ".c" || ext == ".cpp" || ext == ".cc") {
        std::string outFile = "/tmp/buggy_compiled_" + std::to_string(getpid());
        std::string compiler = (ext == ".c") ? "gcc" : "g++";
        std::string compileCmd = compiler + " -g -pthread -o " + outFile + " " + targetPath + " 2>&1";

        FILE* pipe = popen(compileCmd.c_str(), "r");
        if (!pipe) {
            throw std::runtime_error("Compilation failed");
        }

        std::array<char, 256> buffer;
        std::string output;
        while (fgets(buffer.data(), buffer.size(), pipe)) {
            output += buffer.data();
            std::cout << buffer.data();
        }

        lastCompileOutput = output;

        int status = pclose(pipe);

        if (status != 0) {
           // throw std::runtime_error("Compilation failed");
            std::cout << "Compilation failed, continuing with error analysis...\n";
            return "";
        }

        return outFile;
    }

    return targetPath;
}


std::string Compiler::compileMultiple(
    const std::vector<std::string>& sources,
    const std::vector<std::string>& includeDirs
) {
    if (sources.empty()) return "";

    std::string outFile = "/tmp/buggy_multi_" + std::to_string(getpid());

    bool useCpp = false;

    for (const auto& file : sources) {
        std::string ext = std::filesystem::path(file).extension().string();
        if (ext == ".cpp" || ext == ".cc") {
            useCpp = true;
            break;
        }
    }

    std::string compiler = useCpp ? "g++" : "gcc";

    std::string cmd = compiler + " -g -pthread -o " + ShellQuote::quote(outFile);

    for (const auto& inc : includeDirs) {
        cmd += " -I" + ShellQuote::quote(inc);
    }

    for (const auto& src : sources) {
        cmd += " " + ShellQuote::quote(src);
    }

    cmd += " 2>&1";

    RunResult res = run_capture(cmd);

    lastCompileOutput = res.output;

    if (res.exit_code != 0) {
       // throw std::runtime_error("Multi-file compilation failed");
        std::cout << "Compilation failed, continuing with error analysis...\n";
        return "";
    }

    return outFile;
}

const std::string& Compiler::getLastCompileOutput() const {
    return lastCompileOutput;
}