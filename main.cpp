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

#include "Compiler.h"
#include "FileCollector.h"
#include "ChecklistReader.h"
#include "ErrorMatcher.h"
#include "LanguageDetector.h"

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
        FileCollector collector;
        Compiler compiler;

        std::vector<std::string> files;
        std::vector<std::string> includeDirs;

        //  FILE COLLECT
        if (fs::is_directory(targetPath)) {
            // Ordner
            files = collector.collectSourceFiles(targetPath, parser.isRecursive());
            includeDirs = collector.collectIncludeDirs(targetPath, parser.isRecursive());
        } else {
            files.push_back(targetPath);
        }


        std::stringstream collected;
            // Header
            std::cout << "\n╔══════════════════════════════════════════════════════════╗\n";
            std::cout << "║  ANALYZING FOLDER                                        ║\n";
            std::cout << "╚══════════════════════════════════════════════════════════╝\n";
            std::cout << "  Path:  " << targetPath << "\n";
            std::cout << "  Files: " << files.size() << "\n";
            std::cout << "  Mode:  " << (parser.isRecursive() ? "Recursive" : "Non-recursive") << "\n";
            std::cout << "════════════════════════════════════════════════════════════\n\n";
            
            // Zeige Dateiliste im Verbose Mode
            if (parser.isVerbose()) {
                std::cout << "Files to analyze:\n";
                for (size_t i = 0; i < files.size(); ++i) {
                    std::cout << "  " << (i+1) << ". " << files[i] << "\n";
                }
                std::cout << "\n";
            }
            
            // Lese jede Datei
            for (size_t i = 0; i < files.size(); ++i) {
                const std::string& filePath = files[i];

                int lineCount = collector.countLines(filePath);
                std::string content = collector.readSourceCode(filePath);

            if (parser.isVerbose()) {
                // Verbose Zeige Datei-Header & kompletten Code
                std::cout << "\n";
                
                std::string fileNum = "  FILE " + std::to_string(i+1) + "/" + std::to_string(files.size());
                std::string pathLine = "  " + filePath;
                
                size_t boxWidth = std::max(fileNum.length(), pathLine.length()); 
                if (boxWidth < 60) boxWidth = 60; 
                
                std::cout << "╔";
                for (size_t j = 0; j < boxWidth; ++j) std::cout << "═";
                std::cout << "╗\n";

                std::cout << "║" << fileNum;
                if (fileNum.length() < boxWidth) {
                    for (size_t j = 0; j < boxWidth - fileNum.length(); ++j) std::cout << " ";
                }
                std::cout << "║\n";
                
                std::cout << "║" << pathLine;
                if (pathLine.length() < boxWidth) {
                    for (size_t j = 0; j < boxWidth - pathLine.length(); ++j) std::cout << " ";
                }
                std::cout << "║\n";
               
                std::cout << "╚";
                for (size_t j = 0; j < boxWidth; ++j) std::cout << "═";
                std::cout << "╝\n";
                
                std::cout << "Lines: " << lineCount << " | Size: " << content.size() << " bytes\n\n";
            
                // CODE ANZEIGEN
                std::cout << content << "\n";
                
                for (size_t j = 0; j < boxWidth + 2; ++j) std::cout << "-";
                std::cout << "\n";
            } else {
                // Nicht Verbose Nur kurze Info
                std::cout << "  ✓ " << filePath << " (" << lineCount << " lines, " << content.size() << " bytes)\n";
            }
            // Für JSON sammeln
            collected << "===== FILE: " << filePath << " =====\n";
            collected << content << "\n\n";
            }

            std::cout << "\n════════════════════════════════════════════════════════════\n";
            std::cout << "Collected " << files.size() << " file(s) for analysis\n";
            std::cout << "════════════════════════════════════════════════════════════\n\n";


      std::string  sourceCode = collected.str();
        
        if (parser.isVerbose()) {
            std::cout << "Gesammelter Quellcode: " << sourceCode.size() << " bytes\n\n";
        }

        const auto& passArgs = parser.getPassthroughArgs();

        // Nur kompilieren/ausführen, wenn targetPath eine Datei ist
        std::string program;
        bool canRunProgram = false;



        if (!files.empty()) {
            if (files.size() > 1) {
                program = compiler.compileMultiple(files, includeDirs);
            } else {
                program = compiler.compileIfNeeded(files[0]);
            }
            canRunProgram = !program.empty();
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
        if (canRunProgram && !program.empty() && !parser.isGdbUsed() && !parser.isValgrindUsed()) {
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

        // ================================
        // ERROR DETECTION (CHECKLIST)
        // ================================

        // 1. detect language
        LanguageDetector detector;
        std::string language = detector.detect(files);

        // 2. collect ALL error outputs
        std::string error_output;

        // compile errors
        error_output += compiler.getLastCompileOutput();

        // runtime errors
        if (runPtr) {
            error_output += "\n" + runPtr->output;
        }

        // gdb errors
        if (gdbPtr) {
            error_output += "\n" + gdbPtr->output;
        }

        // valgrind errors
        if (vgPtr) {
            error_output += "\n" + vgPtr->run.output;
        }

        // 3. load checklist
        ChecklistReader reader;
        auto checklist = reader.load();

        std::cout << "\n[RAW ERROR OUTPUT]\n";
        std::cout << error_output << "\n";

        std::cout << "[LANGUAGE] " << language << "\n";

        // 4. match errors
        ErrorMatcher matcher(checklist);
        auto detectedErrors = matcher.match(error_output, language);

        // 5. print results
        std::cout << "\n========== DETECTED ERRORS ==========\n";

        if (detectedErrors.empty()) {
            std::cout << "No known errors detected.\n";
        } else {
            for (const auto& e : detectedErrors) {
                std::cout << "- " << e << "\n";
            }
        }

        std::cout << "=====================================\n\n";

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