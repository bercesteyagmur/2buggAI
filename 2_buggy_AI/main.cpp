#include "argumentParser.h"
#include "OpenAiClient.h"
#include "ReportJson.h"
#include "GdbRunner.h"
#include "ValgrindRunner.h"
#include "ProcessRunner.h"
#include "ShellQuote.h"

#include <fstream>
#include <iostream>
#include <filesystem>
#include <unistd.h>
#include <array>
#include <sstream>
#include <algorithm>
#include <vector>
#include <string>

// Liest den Quellcode einer Datei
std::string readSourceCode(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        return "[ERROR: Could not read file]";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// Prüft, ob Datei eine erlaubte Quellcode-Datei ist
bool isSourceFile(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    return ext == ".c" || ext == ".cpp" || ext == ".cc" || ext == ".h" || ext == ".hpp" || ext == ".java" || ext == ".py";
}

// it selects only files that can actually be compiled, because h. and hpp. cannot be compiled
bool isCompilableSourceFile(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    return ext == ".c" || ext == ".cpp" || ext == ".cc";
}

//collect all C/C++ source files
std::vector<std::string> collectCompilableSources(const std::string& targetPath, bool recursive) {
    namespace fs = std::filesystem;
    std::vector<std::string> sources;

    fs::path p(targetPath);

    if (fs::is_regular_file(p)) {
        if (isCompilableSourceFile(p)) {
            fs::path parent = p.parent_path();
            if (parent.empty()) parent = ".";

            for (const auto& entry : fs::directory_iterator(parent)) {
                if (entry.is_regular_file() && isCompilableSourceFile(entry.path())) {
                    sources.push_back(entry.path().string());
                }
            }
        }
    } else if (fs::is_directory(p)) {
        if (recursive) {
            for (const auto& entry : fs::recursive_directory_iterator(p)) {
                if (entry.is_regular_file() && isCompilableSourceFile(entry.path())) {
                    sources.push_back(entry.path().string());
                }
            }
        } else {
            for (const auto& entry : fs::directory_iterator(p)) {
                if (entry.is_regular_file() && isCompilableSourceFile(entry.path())) {
                    sources.push_back(entry.path().string());
                }
            }
        }
    }

    return sources;
}

// detect if C++ compiler is needed
bool containsCppFile(const std::vector<std::string>& sources) {
    for (const auto& file : sources) {
        std::string ext = std::filesystem::path(file).extension().string();
        if (ext == ".cpp" || ext == ".cc") {
            return true;
        }
    }
    return false;
}

// detect common include directories like "include/", "src/", etc.
// this is needed because many C/C++ projects store headers in separate folders
// Without -I flags, compiler cannot find these header files
std::vector<std::string> detectIncludeDirs(const std::string& targetPath) {
    namespace fs = std::filesystem;
    std::vector<std::string> includes;

    fs::path base = std::filesystem::absolute(targetPath);

    // if target is a file, use its parent directory
    if (fs::is_regular_file(base)) {
        base = base.parent_path();
    }

    // common folder names used in real world projects
    std::vector<std::string> commonDirs = {
        "include", "includes", "inc", "src"
    };

    for (const auto& dir : commonDirs) {
        fs::path p = base / dir;
        if (fs::exists(p) && fs::is_directory(p)) {
            includes.push_back(p.string());
        }
    }

    return includes;
}

// Auto-Compile Funktion
std::string compileIfNeeded(const std::string& targetPath) {
    namespace fs = std::filesystem;

    if (!fs::exists(targetPath)) {
        throw std::runtime_error("File does not exist: " + targetPath);
    }

    std::string ext = fs::path(targetPath).extension();

    // Wenn es eine Quelldatei ist (.c, .cpp, .cc)
    if (ext == ".c" || ext == ".cpp" || ext == ".cc") {
        std::string outFile = "/tmp/buggy_compiled_" + std::to_string(getpid());
        std::string compiler = (ext == ".c") ? "gcc" : "g++";
        std::string compileCmd = compiler + " -g -pthread -o " + outFile + " " + targetPath + " 2>&1";

        std::cout << "\n╔════════════════════════════════════════╗\n";
        std::cout << "║         KOMPILIERUNG STARTET           ║\n";
        std::cout << "╚════════════════════════════════════════╝\n";
        std::cout << "Compiler: " << compiler << "\n";
        std::cout << "Datei:    " << targetPath << "\n\n";

        FILE* pipe = popen(compileCmd.c_str(), "r");
        if (!pipe) {
            throw std::runtime_error("Kompilierung fehlgeschlagen - popen error");
        }

        std::array<char, 256> buffer;
        bool hasOutput = false;
        while (fgets(buffer.data(), buffer.size(), pipe)) {
            std::cout << "  " << buffer.data();
            hasOutput = true;
        }

        if (!hasOutput) {
            std::cout << "  (keine Compiler-Warnungen)\n";
        }

        int status = pclose(pipe);

        if (status != 0) {
            std::cerr << "\nKOMPILIERUNGSFEHLER!\n";
            std::cerr << "════════════════════════════════════════\n\n";
            throw std::runtime_error("Kompilierung fehlgeschlagen");
        }

        std::cout << "\nErfolgreich kompiliert: " << outFile << "\n";
        std::cout << "════════════════════════════════════════\n\n";
        return outFile;
    }

    // Ist bereits eine ausführbare Datei
    return targetPath;
}

//  multifile compilation
std::string compileMultiple(const std::string& targetPath, bool recursive) {
    auto sources = collectCompilableSources(targetPath, recursive);

    if (sources.empty()) return "";

    std::string outFile = "/tmp/buggy_multi_" + std::to_string(getpid());
    bool useCpp = containsCppFile(sources);
    std::string compiler = useCpp ? "g++" : "gcc";

    std::string cmd = compiler + " -g -pthread -o " + ShellQuote::quote(outFile);

    // add include directories to the compile command
    // this ensures that header files in folders like "include/" can be found
    auto includeDirs = detectIncludeDirs(targetPath);

    std::cout << "Include directories:\n";
    for (const auto& inc : includeDirs) {
        std::cout << "  - " << inc << "\n";
    }

    for (const auto& inc : includeDirs) {
        std::string absPath = std::filesystem::absolute(inc).string();
        cmd += " -I" + ShellQuote::quote(absPath);
    }

    for (const auto& src : sources) {
        cmd += " " + ShellQuote::quote(src);
    }

    cmd += " 2>&1";

    std::cout << "\n=== MULTI FILE COMPILATION ===\n";
    for (const auto& s : sources) {
        std::cout << "  " << s << "\n";
    }

    RunResult res = run_capture(cmd);

    std::cout << res.output << "\n";

    if (res.exit_code != 0) {
        throw std::runtime_error("Multi-file compilation failed");
    }

    return outFile;
}

int main(int argc, char** argv) {
    try {
        ArgumentParser parser;
        parser.parse(argc, argv);

        if (parser.isVerbose()) {
            parser.printConfig();
        }

        const std::string targetPath = parser.getTargetPath();
        namespace fs = std::filesystem;

        if (!fs::exists(targetPath)) {
            throw std::runtime_error("Path does not exist: " + targetPath);
        }

        // Alle Dateiinhalte sammeln
        std::string sourceCode;
        std::stringstream collected;

        if (fs::is_regular_file(targetPath)) {
            if (isSourceFile(fs::path(targetPath))) {
                collected << "===== FILE: " << targetPath << " =====\n";
                collected << readSourceCode(targetPath) << "\n";
            }
        }
        else if (fs::is_directory(targetPath)) {
            if (parser.isRecursive()) {
                for (const auto& entry : fs::recursive_directory_iterator(targetPath)) {
                    if (entry.is_regular_file() && isSourceFile(entry.path())) {
                        const std::string filePath = entry.path().string();
                        collected << "===== FILE: " << filePath << " =====\n";
                        collected << readSourceCode(filePath) << "\n\n";
                    }
                }
            } else {
                for (const auto& entry : fs::directory_iterator(targetPath)) {
                    if (entry.is_regular_file() && isSourceFile(entry.path())) {
                        const std::string filePath = entry.path().string();
                        collected << "===== FILE: " << filePath << " =====\n";
                        collected << readSourceCode(filePath) << "\n\n";
                    }
                }
            }
        }
        else {
            throw std::runtime_error("Unsupported path type: " + targetPath);
        }

        sourceCode = collected.str();
        // Debug-Ausgabe des gesammelten Quellcodes
        if (parser.isVerbose()) {
           std::cout << "\n===== GELESENER QUELLCODE =====\n";
            std::cout << sourceCode << "\n";
            std::cout << "===============================\n\n";
        }

        if (parser.isVerbose()) {
            std::cout << "Gesammelter Quellcode: " << sourceCode.size() << " bytes\n\n";
        }

        const auto& passArgs = parser.getPassthroughArgs();

        // Nur kompilieren/ausführen, wenn targetPath eine Datei ist
        std::string program;

        std::vector<std::string> sources = collectCompilableSources(targetPath, parser.isRecursive());

        bool canRunProgram = false;

        if (!sources.empty()) {
            if (sources.size() > 1) {
                // multi-file
                program = compileMultiple(targetPath, parser.isRecursive());
            } else {
                // single file
                std::cout << "\n=== SINGLE FILE COMPILATION ===\n";
                std::cout << "  " << sources[0] << "\n";
                program = compileIfNeeded(sources[0]);
            }
            canRunProgram = true;
        }

        // Runner optional
        RunResult gdbRes;
        ValgrindResult vgRes;
        RunResult runRes;
        RunResult* gdbPtr = nullptr;
        ValgrindResult* vgPtr = nullptr;
        RunResult* runPtr = nullptr;

        if (canRunProgram && parser.isGdbUsed()) {
            std::cout << "╔════════════════════════════════════════╗\n";
            std::cout << "║           GDB WIRD AUSGEFÜHRT          ║\n";
            std::cout << "╚════════════════════════════════════════╝\n\n";
            gdbRes = run_gdb(program, passArgs);
            gdbPtr = &gdbRes;
            std::cout << "GDB Beendet mit Exit-Code: " << gdbRes.exit_code << "\n\n";
        }

        if (canRunProgram && parser.isValgrindUsed()) {
            std::cout << "╔════════════════════════════════════════╗\n";
            std::cout << "║        VALGRIND WIRD AUSGEFÜHRT        ║\n";
            std::cout << "╚════════════════════════════════════════╝\n\n";
            vgRes = run_valgrind(program, passArgs);
            vgPtr = &vgRes;
            std::cout << "Valgrind Beendet mit Exit-Code: " << vgRes.run.exit_code << "\n\n";
        }

        // Normaler Run nur für Dateien
        if (canRunProgram && !parser.isGdbUsed() && !parser.isValgrindUsed()) {
            std::cout << "╔════════════════════════════════════════╗\n";
            std::cout << "║   NORMALES PROGRAMM WIRD AUSGEFÜHRT    ║\n";
            std::cout << "╚════════════════════════════════════════╝\n\n";

            std::string cmd = "timeout 5s " + ShellQuote::quote(program);
            for (const auto& a : passArgs) {
                cmd += " ";
                cmd += ShellQuote::quote(a);
            }

            runRes = run_capture(cmd);
            runPtr = &runRes;
            std::cout << runRes.output << "\n";
            std::cout << "Programm Beendet mit Exit-Code: " << runRes.exit_code << "\n\n";
        }

        if (!canRunProgram && parser.isVerbose()) {
            std::cout << "Hinweis: Ziel ist ein Ordner. Es werden nur passende Quellcode-Dateien gelesen, nichts kompiliert oder ausgeführt.\n\n";
        }

        // Report JSON bauen
        std::string report = make_report_json(
            parser.getTargetPath(),
            parser.getFixDescription(),
            parser.isRecursive(),
            parser.isVerbose(),
            parser.getFileExtensions(),
            parser.getPassthroughArgs(),
            gdbPtr,
            vgPtr,
            runPtr,
            sourceCode
        );

        // Optional: JSON in Datei schreiben
        if (!parser.getJsonOutFile().empty()) {
            std::ofstream out(parser.getJsonOutFile());
            out << report;
            std::cout << "JSON Report gespeichert: " << parser.getJsonOutFile() << "\n\n";
        }

        std::cout << "╔════════════════════════════════════════╗\n";
        std::cout << "║      OPENAI ANALYSE WIRD GESTARTET     ║\n";
        std::cout << "╚════════════════════════════════════════╝\n\n";

        // OpenAI Debug
        OpenAIClient client;
        OpenAIResult r = client.debug_report(report);

        if (r.http_status >= 200 && r.http_status < 300) {
            std::cout << "\n===== DEBUG REPORT (OpenAI) =====\n";
            std::cout << (r.text.empty() ? r.raw_json : r.text) << "\n";
            std::cout << "=================================\n";
            return 0;
        }

        std::cerr << "OpenAI HTTP " << r.http_status << "\n" << r.raw_json << "\n";
        return 5;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 2;
    }
}