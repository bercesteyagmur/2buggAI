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
#include <nlohmann/json.hpp>

#include "Compiler.h"
#include "FileCollector.h"
#include "ChecklistReader.h"
#include "ErrorMatcher.h"
#include "ErrorCollector.h"
#include "LanguageDetector.h"
#include "CodeChanger.h"

#include "JavaProjectDetector.h"
#include "JavaProjectType.h"

#include "JdbRunner.h"
#include "PdbRunner.h"
#include "DependencyManager.h"
#include "EnvironmentManager.h"

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

        std::vector<std::string> passArgs = parser.getPassthroughArgs();

        // Nur kompilieren/ausführen, wenn targetPath eine Datei ist
        std::string program;
        bool canRunProgram = false;
        std::string detectedLanguage = "unknown";
        std::string cmd;

        if (!files.empty()) {
            LanguageDetector detectorForCompile;
            detectedLanguage = detectorForCompile.detect(files);

            if (detectedLanguage == "python") {

                EnvironmentManager::ensurePipInstalled();

                if (!EnvironmentManager::createVirtualEnv(targetPath)) {

                    std::cerr << "Failed to create virtual environment\n";

                    return 1;
                }

                std::cout << "Detected Python project\n";

                // install requirements immediately
                RunResult installRes = EnvironmentManager::installRequirements(targetPath);

                if (installRes.exit_code != 0) {

                    std::cerr << "Failed to install requirements\n";
                    std::cerr << installRes.output<< "\n";

                    bool repaired =DependencyManager::removeBrokenRequirement(targetPath,installRes.output);

                    if (repaired) {
                        std::cout << "Broken dependency removed\n";
                        std::cout << "Retrying dependency installation...\n";
                        installRes =EnvironmentManager::installRequirements(targetPath);
                        std::cout << installRes.output<< "\n";
                    }

                    if (installRes.exit_code != 0) {
                        std::cerr << "Dependency installation still failed\n";
                        std::cout << "Trying import-based dependency detection...\n";
                    }
                }



                std::string entryFile;
                // If the project contains multiple Python files, we first look for common entry point names such as main.py, app.py, or run.py
                // 1. Common file names
                std::vector<std::string> preferredNames = {
                    "main.py",
                    "app.py",
                    "run.py",
                    "manage.py",
                    "server.py",
                    "cli.py"
                };

                for (const auto& preferred : preferredNames) {
                    for (const auto& file : files) {
                        if (std::filesystem::path(file).filename() == preferred) {
                            entryFile = file;
                            break;
                        }
                    }
                    if (!entryFile.empty()) {
                        break;
                    }
                }

                //If none of these files exists, we scan all Python files for the standard if __name__ == "__main__": block. This is the conventional way in Python to mark the executable entry point of a script
                // 2. Search for __main__
                if (entryFile.empty()) {
                    for (const auto& file : files) {
                        std::ifstream in(file);
                        std::stringstream buffer;
                        buffer << in.rdbuf();
                        std::string content = buffer.str();

                        if (content.find("__name__ == \"__main__\"") != std::string::npos || content.find("__name__ == '__main__'") != std::string::npos) {
                            entryFile = file;
                            break;
                            }
                    }
                }
                // If the project contains multiple Python files, only one entry file is executed.
                // If this file imports other modules, Python automatically loads and uses them.
                // As a fallback, when no clear entry point can be detected, we run the first Python file.
                if (entryFile.empty()) {
                    entryFile = files[0];
                }

                // Previously the debugger used the global system Python interpreter.
                // This caused dependency and environment conflicts between projects.
                //
                // Now each Python project uses its own isolated virtual environment:
                // .venv/bin/python
                //
                // This makes dependency handling more stable and closer to real-world
                // development environments.
                program =EnvironmentManager::getPythonExecutable(targetPath);

                if (std::filesystem::path(entryFile).filename()== "manage.py") {
                    passArgs.push_back("runserver");
                }

                passArgs.insert(passArgs.begin(),entryFile);
            }

            else if (detectedLanguage == "java") {
                JavaProjectDetector javaProjectDetector;
                JavaProjectType type = javaProjectDetector.detect(targetPath);

                std::string javaBuildPath = javaProjectDetector.getBuildPath();

                switch (type) {

                    case JavaProjectType::PlainJava:
                        std::cout << "Detected Plain Java project\n";
                        program = compiler.compileJava(files);
                        break;

                    case JavaProjectType::Maven:
                        std::cout << "Detected Maven Java project\n";
                        program = compiler.compileMaven(javaBuildPath);
                        break;

                    case JavaProjectType::SpringBootMaven:
                        std::cout << "Detected Spring Boot Maven project\n";
                        program = compiler.compileMaven(javaBuildPath);
                        break;

                    case JavaProjectType::Gradle:
                        std::cout << "Detected Gradle Java project\n";
                        program = compiler.compileGradle(javaBuildPath);
                        break;

                    case JavaProjectType::SpringBootGradle:
                        std::cout << "Detected Spring Boot Gradle project\n";
                        program = compiler.compileGradle(javaBuildPath);
                        break;

                        default:

        // NEW: nested module scan fallback

        bool nestedGradle = false;
        bool nestedMaven = false;

        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(targetPath)) {

            if (!entry.is_regular_file()) {
                continue;
            }

            if (entry.path().filename() == "build.gradle") {
                nestedGradle = true;

                std::cout << "Detected nested Gradle module: "
                          << entry.path().parent_path()
                          << "\n";
                break;
            }

            if (entry.path().filename() == "pom.xml") {
                nestedMaven = true;

                std::cout << "Detected nested Maven module: "
                          << entry.path().parent_path()
                          << "\n";
                break;
            }
        }

        if (nestedGradle) {
            std::cout << "Using nested Gradle build\n";
            program = compiler.compileGradle(targetPath);

        } else if (nestedMaven) {
            std::cout << "Using nested Maven build\n";
            program = compiler.compileMaven(targetPath);

        } else {
            std::cout << "Unknown Java project type, using plain javac\n";
            program = compiler.compileJava(files);
        }
                        break;
                }

            } else {
            if (files.size() > 1) {
                program = compiler.compileMultiple(files, includeDirs);
            } else {
                program = compiler.compileIfNeeded(files[0]);
            }
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

        // For C/C++ we use GDB.
        // For Java we use JDB (Java Debugger).
        // For Python we use PDB (Python Debugger).
        // The command line option remains the same (-g), but internally we choose
        // the appropriate debugger depending on the detected programming language.
        if (canRunProgram && parser.isGdbUsed()) {
            std::cout << "╔════════════════════════════════════════╗\n";
            std::cout << "║            DEBUGGER IS RUNNING         ║\n";
            std::cout << "╚════════════════════════════════════════╝\n\n";

            if (detectedLanguage == "java") {
                gdbRes = run_jdb(program);
            }
            else if (detectedLanguage == "python") {
                gdbRes = run_pdb(program);
            }
            else {
                // Default: use GDB for C and C++
                gdbRes = run_gdb(program, passArgs);
            }

            gdbPtr = &gdbRes;
            std::cout << "Debugger finished with Exit-Code: " << gdbRes.exit_code << "\n\n";

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


            cmd = "export MPLBACKEND=Agg && cd '" + targetPath + "' && ";

            // cmd += "yes n | ";

           // cmd += "timeout 1200s ";

            cmd += ShellQuote::quote(program);

            for (const auto& arg : passArgs) {

                cmd += " ";

                cmd += ShellQuote::quote(arg);
            }



            runRes = run_capture(cmd);
            runPtr = &runRes;

            switch (runRes.status) {

                case ExecutionStatus::SUCCESS:

                    std::cout
                        << "Program finished successfully\n";

                    break;

                case ExecutionStatus::TEST_FAILURE:

                    std::cout
                        << "Program executed but unit tests failed\n";

                    break;

                case ExecutionStatus::TIMEOUT:

                    std::cout
                        << "Program timeout\n";

                    break;

                case ExecutionStatus::SIGNAL_TERMINATED:

                    std::cout
                        << "Program terminated by signal\n";

                    break;

                case ExecutionStatus::RUNTIME_ERROR:

                    std::cout
                        << "Program exited with runtime errors\n";

                    break;
            }

            std::cout
                << "Exit-Code: "
                << runRes.exit_code
                << "\n\n";


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

        if (detectedLanguage == "python") {

            if (error_output.find("No module named")
                != std::string::npos) {

                std::cout << "\nMissing Python dependency detected\n";


                std::cout << "\n[IMPORT DETECTION STARTED]\n";

                auto packages =DependencyManager::detectPythonImports(targetPath);

                std::cout << "Detected package count: "<< packages.size()<< "\n";

                for (const auto& pkg : packages) {
                    std::cout << "Detected package: " << pkg<< "\n";
                }

                bool installOk = DependencyManager::installPythonPackages(targetPath,packages);

                std::cout << "Package install result: "
                          << installOk
                          << "\n";


                std::cout << "Retrying Python project...\n";

                runRes = run_capture(cmd);

                error_output += "\n" + runRes.output;
                }
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

        // 4b. nach Schwierigkeit sortieren (leicht -> mittel -> schwer),
        // damit der Fix Loop mit den einfachsten Fehlern beginnt
        ErrorCollector errorCollector;
        detectedErrors = errorCollector.sortedErrors(detectedErrors);

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

        // Fix Loop
    if (!detectedErrors.empty()) {

    OpenAIClient fixClient;
    std::string checkListRaw = reader.loadRaw(); // einmal laden, nicht bei jedem Fehler

    const int MAX_ITER = 5;
    const int MAX_TOTAL_ERRORS = 20; // um endlose Fehler-Explosion zu verhindern

    for (size_t i = 0; i < detectedErrors.size(); ++i) {

        int iteration = 0;
        bool fixed = false;

        while (iteration < MAX_ITER) {

            std::cout << "Versuch " << (iteration + 1) << "/" << MAX_ITER
                      << " fuer Fehler: " << detectedErrors[i]
                      << " (Schwierigkeit: " << ErrorCollector::difficultyOf(detectedErrors[i]) << ")\n";

            FixRequest fix_req;
            fix_req.error_name   = detectedErrors[i];
            fix_req.error_output = error_output;
            fix_req.language     = language;
            fix_req.source_code  = sourceCode;
            fix_req.checklist    = checkListRaw;

            FixResult fix_res = fixClient.fix_code(fix_req);

            if (fix_res.success) {
                sourceCode = fix_res.fixed_code;

                CodeChanger changer;
                changer.apply_fix(fix_res);

                // Neu entdeckte/erzeugte Fehler ebenfalls collecten und nach
                // Schwierigkeit sortieren (leicht -> mittel -> schwer), bevor
                // sie in die Warteschlange eingefuegt werden
                auto newErrors = errorCollector.sortedErrors(fix_res.new_errors);

                size_t insertPos = i + 1;
                for (const auto& newErr : newErrors) {
                    if(detectedErrors.size() < MAX_TOTAL_ERRORS) {
                        detectedErrors.insert(detectedErrors.begin() + insertPos, newErr);
                        error_output += "\n" + newErr;
                        insertPos++;
                    }
                }

                fixed = true;
                break;
            }

            iteration++;
        }

        if (!fixed) {
            std::cerr << "Fehler konnte nicht behoben werden: " << detectedErrors[i] << "\n";
        }
    }
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
            try {
                nlohmann::json final_json = nlohmann::json::parse(report);

                // Erweitere mit Error Detection Info
                final_json["detected_errors"] = detectedErrors;
                final_json["language"] = language;
                final_json["compile_output"] = compiler.getLastCompileOutput();

            std::ofstream out(parser.getJsonOutFile());
            out << final_json.dump(2);
            std::cout << "JSON Report gespeichert: " << parser.getJsonOutFile() << "\n\n";
            } catch (const std::exception& e) {
                // Fallback: Wenn Parse fehlschlägt, speichere original
                std::ofstream out(parser.getJsonOutFile());
                out << report;
                std::cout << "JSON Report gespeichert: " << parser.getJsonOutFile() << "\n\n";
            }
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

            // Extract categories from AI response and add to checklist
            std::istringstream iss(r.text);
            std::string textLine;
            const std::string marker = "**Category:**";

            while (std::getline(iss, textLine)) {
                size_t pos = textLine.find(marker);
                if (pos == std::string::npos) continue;

                // Take everything after Category
                std::string category = textLine.substr(pos + marker.size());

                // The line might contain multiple categories separated by "/"
                //take only the first one
                size_t slash = category.find('/');
                if (slash != std::string::npos) category = category.substr(0, slash);

                    category = reader.trim(category);

                if (category.empty() || category == "other") continue;

                if (reader.appendIfNew(category, checklist)) {
                std::cout << "New Checklist Entry: " << category << "\n";
                }
            }

            if (!parser.getJsonOutFile().empty()) {
                try {
                    nlohmann::json final_json = nlohmann::json::parse(report);
                    final_json["detected_errors"] = detectedErrors;
                    final_json["language"] = language;
                    final_json["compile_output"] = compiler.getLastCompileOutput();
                    final_json["ai_analysis"] = r.text;  // Markdown-Text vom AI

                    std::ofstream out(parser.getJsonOutFile());
                    out << final_json.dump(2);
                    std::cout << "Final JSON Report aktualisiert: " << parser.getJsonOutFile() << "\n";
                } catch (...) {
                    // Ignore - JSON wurde schon vorher gespeichert
                }
            }
            return 0;
        }

        std::cerr << "OpenAI HTTP " << r.http_status << "\n" << r.raw_json << "\n";
        return 5;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 2;
    }
}