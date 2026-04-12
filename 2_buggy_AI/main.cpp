#include "/home/nikog/projects/2buggAI/2_buggy_AI/includes/argumentParser.h"
#include "/home/nikog/projects/2buggAI/2_buggy_AI/includes/OpenAiClient.h"
#include "/home/nikog/projects/2buggAI/2_buggy_AI/includes/ReportJson.h"
#include "/home/nikog/projects/2buggAI/2_buggy_AI/includes/GdbRunner.h"
#include "/home/nikog/projects/2buggAI/2_buggy_AI/includes/ValgrindRunner.h"
#include "/home/nikog/projects/2buggAI/2_buggy_AI/includes/ProcessRunner.h"
#include "/home/nikog/projects/2buggAI/2_buggy_AI/includes/ShellQuote.h"
#include <fstream>
#include <iostream>
#include <filesystem>
#include <unistd.h>
#include <array>
#include <sstream>

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
        bool canRunProgram = fs::is_regular_file(targetPath);

        if (canRunProgram) {
            program = compileIfNeeded(targetPath);
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