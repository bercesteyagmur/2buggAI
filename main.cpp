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
#include "WebEnvironmentManager.h"
#include "WebPipeline.h"
#include "StackDetector.h"
#include "StackPipeline.h"
#include "PdfReportExporter.h"
#include "FixRecord.h"
#include "FixHistory.h"
#include "ErrorDetection.h"
#include "ToolOutputParser.h"

int main(int argc, char** argv) {
    try {
        ArgumentParser parser;
        parser.parse(argc, argv);
        LlmProvider::validateEnvironment();  // Pipeline: fehlende Keys/Modelle sofort melden

        if (parser.isVerbose()) {
            parser.printConfig();
        }

        namespace fs = std::filesystem;
        // Absolute path: fixes, backups and reports resolve file paths against the project folder.
        // With a relative path ("./proj", "src/main.c") the fix loop looked for "proj/proj/main.c"
        // and the run ended at the first fix.
        const std::string targetPath = [&] {
            std::string p = fs::absolute(parser.getTargetPath()).lexically_normal().string();
            while (p.size() > 1 && p.back() == '/') p.pop_back();
            return p;
        }();

        if (!fs::exists(targetPath)) {
            throw std::runtime_error("Path does not exist: " + targetPath);
        }

        // PDF-Export der JSON-Reports: Defaults < <projekt>/.buggai.json (pdf_export) < CLI.
        // Der Exporter lebt bis zum Ende des Runs; sein Destruktor schreibt den Gesamt-Report.
        PdfExportConfig pdfConfig = PdfReportExporter::loadConfig(targetPath);
        if (parser.isPdfDisabled()) pdfConfig.enabled = false;
        if (parser.isPdfCombinedDisabled()) pdfConfig.combined = false;
        if (parser.isPdfPerIteration()) pdfConfig.perIteration = true;
        if (!parser.getPdfDir().empty()) pdfConfig.outputDir = parser.getPdfDir();
        PdfReportExporter pdfExporter(pdfConfig);
        const std::string pdfName = fs::path(targetPath).lexically_normal().filename().string() + "_report";
        int pdfIteration = 0;

        // Fehler-/Fix-Abschnitte des Reports: werden im Fix-Loop befüllt (docs/PLAN_REPORT_REDESIGN.md)
        const std::string reportRoot = fs::is_directory(targetPath)
            ? targetPath : fs::path(targetPath).parent_path().string();
        FixReportBuilder fixReport(reportRoot, pdfConfig.contextLines, pdfConfig.language);
        fixReport.setPipeline(LlmProvider::pipelineEnabled());
        const std::string explanationLanguage = pdfConfig.language == "en" ? "English" : "German";

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
        bool buildFailed = false;  // the compiler / build tool produced no program
        std::string detectedLanguage = "unknown";
        std::string cmd;

        if (!files.empty()) {
            LanguageDetector detectorForCompile;
            detectedLanguage = detectorForCompile.detect(files);

            std::cout << "[LANGUAGES]";
            for (const auto& [lang, count] : detectorForCompile.detectAll(files)) {
                std::cout << " " << lang << " (" << count << ")";
            }
            std::cout << "\n[PRIMARY LANGUAGE] " << detectedLanguage << "\n";

            // Fullstack / multi-service repositories (Python backend + JS/TS frontend + SQL,
            // several services) are checked per component by the stack pipeline. Projects that
            // are C/C++ or Java, PHP projects (composer.json) and everything with --stack legacy
            // keep the existing paths below.
            if (fs::is_directory(targetPath) && parser.getStackMode() != "legacy"
                && (detectedLanguage == "python" || detectedLanguage == "unknown")
                && !fs::exists(fs::path(targetPath) / "composer.json")) {
                ProjectStack stack = StackDetector::detect(targetPath);
                if (parser.getStackMode() == "fullstack") stack.config.stack = "fullstack";
                if (stack.isFullstack()) {
                    std::cout << "[STACK] fullstack project detected (use --stack legacy for the single-language mode)\n";
                    WebPipelineOptions options;
                    options.targetPath = targetPath;
                    options.fixDescription = parser.getFixDescription();
                    options.jsonOutFile = parser.getJsonOutFile();
                    options.runtime = parser.isRuntimeUsed();
                    options.pdf = &pdfExporter;
                    return StackPipeline::run(options, files, stack);
                }
                if (parser.getStackMode() == "fullstack") {
                    std::cout << "[STACK] --stack fullstack: no Python, Node or SQL component found, using the default mode\n";
                }
            }

            if (detectedLanguage == "unknown") {
                if (fs::is_directory(targetPath) && WebEnvironmentManager::isWebProject(targetPath)) {
                    WebPipelineOptions options;
                    options.targetPath = targetPath;
                    options.fixDescription = parser.getFixDescription();
                    options.jsonOutFile = parser.getJsonOutFile();
                    options.runtime = parser.isRuntimeUsed();
                    options.pdf = &pdfExporter;
                    return WebPipeline::run(options, files);
                }
                std::cout << "\nNo build/debug support for the languages of this project yet. Stopping.\n";
                return 0;
            }

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

                        // Run import-based detection NOW, before executing the program
                        auto detectedPkgs = DependencyManager::detectPythonImports(targetPath);
                        DependencyManager::installPythonPackages(targetPath, detectedPkgs);
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

                program = EnvironmentManager::getPythonExecutable(targetPath);

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
            buildFailed = !canRunProgram && detectedLanguage != "python";

            // Detect interactive usage and inform the user (but still run)
            if (canRunProgram) {
                bool isInteractive = false;
                for (const auto& f : files) {
                    std::ifstream checkFile(f);
                    std::string checkLine;
                    while (std::getline(checkFile, checkLine)) {
                        bool pythonInteractive = detectedLanguage == "python" &&
                            (checkLine.find("input(") != std::string::npos ||
                             checkLine.find("sys.stdin") != std::string::npos ||
                             checkLine.find("getpass(") != std::string::npos);
                        bool cInteractive = (detectedLanguage == "c" || detectedLanguage == "cpp") &&
                            (checkLine.find("std::cin") != std::string::npos ||
                             checkLine.find("cin >>") != std::string::npos ||
                             checkLine.find("getline(") != std::string::npos ||
                             checkLine.find("scanf(") != std::string::npos ||
                             checkLine.find("getchar(") != std::string::npos ||
                             checkLine.find("fgets(") != std::string::npos);
                        if (pythonInteractive || cInteractive) {
                            isInteractive = true;
                            break;
                        }
                    }
                    if (isInteractive) break;
                }
                if (isInteractive) {
                    std::cout << "Interactive program detected — running with empty input (yes \"\").\n";
                }
            }

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

        // Zeitlimit: Server (z. B. HttpServer, Spring Boot) enden nie von selbst und
        // würden den Lauf und jede Fix-Iteration (redetect) blockieren.
        const int runTimeout = parser.getRunTimeoutSec();

        // Command for a normal run of the current program (also used by the fix loop, which
        // needs it when the very first build failed and only a fix made the program buildable).
        auto buildRunCmd = [&]() -> std::string {
            if (program.empty()) return "";
            std::string cdPath = targetPath;
            if (fs::is_regular_file(targetPath)) cdPath = fs::path(targetPath).parent_path().string();

            std::string c = "export MPLBACKEND=Agg && cd " + ShellQuote::quote(cdPath) + " && yes \"\" | ";
            if (runTimeout > 0) c += "timeout -k 5s " + std::to_string(runTimeout) + "s ";

            const bool isJar = program.size() > 4 && program.substr(program.size() - 4) == ".jar";
            c += isJar ? "java -jar " + ShellQuote::quote(program) : ShellQuote::quote(program);
            for (const auto& arg : passArgs) c += " " + ShellQuote::quote(arg);
            return c;
        };

        // Normaler Run nur für Dateien
        if (canRunProgram && !program.empty() && !parser.isGdbUsed() && !parser.isValgrindUsed()) {
            std::cout << "╔════════════════════════════════════════╗\n";
            std::cout << "║   NORMALES PROGRAMM WIRD AUSGEFÜHRT    ║\n";
            std::cout << "╚════════════════════════════════════════╝\n\n";

            cmd = buildRunCmd();
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
                        << "Program timeout: lief noch nach " << runTimeout
                        << "s und wurde beendet (bei Servern normal, kein Absturz)\n";

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

        // 4. detect errors: checklist categories, a fallback for unknown build failures / crashes,
        // and environment problems (no code fix). Sorted easy -> hard, so the fix loop starts
        // with the easiest error.
        auto runFailedOf = [](const RunResult* r) {
            return r && (r->status == ExecutionStatus::RUNTIME_ERROR
                      || r->status == ExecutionStatus::SIGNAL_TERMINATED
                      || r->status == ExecutionStatus::TEST_FAILURE);
        };
        // error_output = build output + everything the program / debugger / Valgrind printed
        auto detectNow = [&](bool failedBuild, const RunResult* run) {
            const std::string& buildOut = compiler.getLastCompileOutput();
            const bool startsWithBuild = error_output.compare(0, buildOut.size(), buildOut) == 0;
            const std::string runOut = startsWithBuild ? error_output.substr(buildOut.size()) : error_output;
            return ErrorDetection::detect(checklist, language, startsWithBuild ? buildOut : "", failedBuild,
                                          runOut, runFailedOf(run));
        };
        // Environment problems: say so once per new problem, with what to do.
        std::vector<std::string> reportedEnvironment;
        auto reportEnvironment = [&](const ErrorDetection::Result& d) {
            for (const auto& env : d.environment) {
                if (std::find(reportedEnvironment.begin(), reportedEnvironment.end(), env) != reportedEnvironment.end()) continue;
                reportedEnvironment.push_back(env);
                std::cout << "[UMGEBUNGSPROBLEM] " << env << ": " << ErrorDetection::environmentHint(env) << "\n";
            }
            if (!d.environment.empty() && !d.environmentCaused.empty()) {
                std::cout << "  Folgefehler ohne Code-Fix-Versuch:";
                for (const auto& e : d.environmentCaused) std::cout << " " << e;
                std::cout << "\n";
            }
        };

        ErrorDetection::Result detection = detectNow(buildFailed, runPtr);
        auto detectedErrors = detection.errors;

        // 5. print results
        std::cout << "\n========== DETECTED ERRORS ==========\n";

        if (detectedErrors.empty()) {
            std::cout << "No known errors detected.\n";
        } else {
            for (const auto& e : detectedErrors) {
                std::cout << "- " << e << "\n";
            }
        }

        std::cout << "=====================================\n";
        reportEnvironment(detection);
        std::cout << "\n";

        // Ort, Tool und Meldung eines Fehlers für den Report, aus der aktuellen Ausgabe
        fixReport.setDescriber([&](const std::string& name) {
            ErrorInfo info;
            info.language = language;
            const std::vector<std::string> keywords = FixReportBuilder::keywordsFor(name, checklist);
            auto mentions = [&](const std::string& text) {
                std::string lower = text;
                std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
                return std::any_of(keywords.begin(), keywords.end(),
                                   [&](const std::string& k) { return lower.find(k) != std::string::npos; });
            };
            // compiler messages (file:line:col: ...) and Python tracebacks of project files
            for (const auto& e : ToolOutputParser::parse("generic", error_output, reportRoot, reportRoot)) {
                if (!mentions(e.message)) continue;
                if (info.file.empty()) { info.file = e.file; info.severity = e.severity; }
                if (e.file == info.file && std::find(info.lines.begin(), info.lines.end(), e.line) == info.lines.end())
                    info.lines.push_back(e.line);
            }
            if (mentions(compiler.getLastCompileOutput())) {
                info.tool = detectedLanguage == "c" ? "Compiler (gcc)" : detectedLanguage == "cpp" ? "Compiler (g++)"
                          : detectedLanguage == "java" ? "Compiler (Java)" : "Compiler";
            } else if (gdbPtr && mentions(gdbPtr->output)) {
                info.tool = detectedLanguage == "java" ? "Debugger (jdb)" : detectedLanguage == "python" ? "Debugger (pdb)" : "Debugger (gdb)";
            } else if (vgPtr && mentions(vgPtr->run.output)) {
                info.tool = "Valgrind";
            } else {
                info.tool = fixReport.tr("Laufzeit (Programmausgabe)", "runtime (program output)");
            }
            info.message = FixReportBuilder::excerptMessage(error_output, keywords, info.file, 14,
                fs::absolute(reportRoot).lexically_normal().string() + "/");
            return info;
        });
        fixReport.observe(detectedErrors);

        // Detect platform-specific headers(environment issue not code bug)
        {
            static const std::vector<std::string> platformHeaders = {
                "windows.h", "conio.h", "process.h", "jni.h",
                "ldap.h", "dos.h", "graphics.h", "bios.h"
            };
            for (const auto& hdr : platformHeaders) {
                if (error_output.find(hdr + ": No such file") != std::string::npos) {
                    std::cout << "[PLATFORM ISSUE] The project requires '" << hdr
                              << "', which is not available on this OS.\n"
                              << "This is an environment/portability issue, not a code bug.\n"
                              << "Skipping fix attempts; running static analysis only.\n\n";
                    for (const auto& e : detectedErrors) {
                        fixReport.skipError(e, fixReport.tr("Plattform-Header '", "Platform header '") + hdr
                            + fixReport.tr("' ist auf diesem System nicht verfügbar (Umgebungsproblem) - kein Fix-Versuch.",
                                           "' is not available on this system (environment problem) - no fix attempt."));
                    }
                    detectedErrors.clear();
                    break;
                }
            }
        }
        // Fix Loop
        bool anyFixApplied = false;  // then the final report needs fresh code and debugger output
    if (!detectedErrors.empty()) {

        OpenAIClient fixClient;
        std::string checkListRaw = reader.loadRaw();

        const int MAX_ROUNDS   = 20;  // global budget against endless loops
        const int MAX_ITER     = 5;   // attempts per single error
        const int MAX_ATTEMPTS = 15;  // with attempts that made progress (each one earns one more)

        bool lastBuildFailed = buildFailed;  // state of the last build, kept by redetect()

        // Re-build / re-run the project and re-detect errors from the fresh
        // output. The REAL project state is the source of truth after every
        // applied fix, instead of trusting AI-reported new_errors.
        auto redetect = [&]() -> std::vector<std::string> {
            std::string freshOutput;
            std::string prog = program;  // Python has no build step: the program stays

            if (detectedLanguage == "java") {
                JavaProjectDetector jpd;
                JavaProjectType t = jpd.detect(targetPath);
                std::string bp = jpd.getBuildPath();
                switch (t) {
                    case JavaProjectType::Maven:
                    case JavaProjectType::SpringBootMaven:  prog = compiler.compileMaven(bp);  break;
                    case JavaProjectType::Gradle:
                    case JavaProjectType::SpringBootGradle: prog = compiler.compileGradle(bp); break;
                    default:                                prog = compiler.compileJava(files); break;
                }
                freshOutput += compiler.getLastCompileOutput();
                if (!prog.empty()) program = prog;
            } else if (detectedLanguage != "python") {
                // C / C++
                prog = (files.size() > 1)
                    ? compiler.compileMultiple(files, includeDirs)
                    : compiler.compileIfNeeded(files[0]);
                freshOutput += compiler.getLastCompileOutput();
                if (!prog.empty()) program = prog;
            }
            const bool failedBuild = prog.empty() && detectedLanguage != "python";
            lastBuildFailed = failedBuild;

            // Run the program again, in the same mode as the first run: normal run, debugger or
            // Valgrind. (Before, only the normal run was repeated, so with --gdb / --valgrind a
            // crash counted as fixed as soon as the code compiled.) After a failed build there is
            // no program for the current code; its old binary is not run.
            if (failedBuild || program.empty()) {
                runPtr = nullptr;
                gdbPtr = nullptr;
                vgPtr = nullptr;
            } else {
                canRunProgram = true;
                if (parser.isGdbUsed()) {
                    if (detectedLanguage == "java")        gdbRes = run_jdb(program);
                    else if (detectedLanguage == "python") gdbRes = run_pdb(program);
                    else                                   gdbRes = run_gdb(program, passArgs);
                    gdbPtr = &gdbRes;
                    freshOutput += "\n" + gdbRes.output;
                }
                if (parser.isValgrindUsed()) {
                    vgRes = run_valgrind(program, passArgs);
                    vgPtr = &vgRes;
                    freshOutput += "\n" + vgRes.run.output;
                }
                if (!parser.isGdbUsed() && !parser.isValgrindUsed()) {
                    if (cmd.empty()) cmd = buildRunCmd();  // first build failed, now there is a program
                    runRes = run_capture(cmd);
                    runPtr = &runRes;  // the final report shows the state after the fixes
                    freshOutput += "\n" + runRes.output;
                }
            }

            error_output = freshOutput;  // refresh shared output for next request
            detection = detectNow(failedBuild, runPtr);
            reportEnvironment(detection);
            return detection.errors;
        };

        std::vector<std::string> giveUp;  // errors abandoned after MAX_ITER
        auto isGivenUp = [&](const std::string& e) {
            return std::find(giveUp.begin(), giveUp.end(), e) != giveUp.end();
        };

        int round = 0;
        while (round < MAX_ROUNDS) {
            // pick the easiest error we have not given up on
            std::string current;
            for (const auto& e : detectedErrors)
                if (!isGivenUp(e)) { current = e; break; }
            if (current.empty()) break;  // nothing left to do

            // Caused by the environment (database down, port in use, ...): a code change cannot
            // fix it, and the model would only build workarounds. Report it, do not try.
            if (detection.environmentCaused.count(current) && !detection.environment.empty()) {
                std::string envList;
                for (const auto& env : detection.environment) {
                    envList += (envList.empty() ? "" : ", ") + env;
                }
                std::cout << "Skipping " << current << ": caused by the environment (" << envList << ")\n";
                fixReport.skipError(current, fixReport.tr(
                    "Umgebungsproblem (" + envList + "): " + ErrorDetection::environmentHint(detection.environment.front())
                        + " Kein Code-Fix-Versuch.",
                    "Environment problem (" + envList + "): not a code bug, no fix attempt."));
                giveUp.push_back(current);
                continue;
            }

            // venv special case: a syntax error inside an installed package
            // can never be fixed by retrying — reinstall and skip.
            if (current == "syntax_error_python") {
                const std::string pipExe = EnvironmentManager::getPipExecutable(targetPath);
                const std::string pipSuffix = "/bin/pip";
                const std::string venvRoot = (pipExe.size() > pipSuffix.size())
                    ? pipExe.substr(0, pipExe.size() - pipSuffix.size())
                    : "";

                if (!venvRoot.empty() && error_output.find(venvRoot) != std::string::npos) {
                    std::cout << "Syntax error detected inside installed package (venv), skipping retries.\n";
                    std::cout << "Attempting to reinstall correct package...\n";

                    std::string marker = "site-packages/";
                    size_t sitePos = error_output.find(marker);
                    if (sitePos != std::string::npos) {
                        size_t start = sitePos + marker.size();
                        size_t end = error_output.find_first_of("/.\n", start);
                        if (end != std::string::npos) {
                            std::string badPkg = error_output.substr(start, end - start);
                            std::string pip = EnvironmentManager::getPipExecutable(targetPath);
                            std::cout << "Uninstalling bad package: " << badPkg << "\n";
                            std::string uninstall = pip + " uninstall -y " + badPkg;
                            if (system(uninstall.c_str()) != 0) {}  // Ergebnis bewusst ignoriert
                            std::vector<std::string> pkgs = {badPkg};
                            DependencyManager::installPythonPackages(targetPath, pkgs);
                        }
                    }
                    const int reportId = fixReport.beginError(current);
                    detectedErrors = redetect();  // re-detect after reinstall
                    fixReport.observe(detectedErrors, reportId);
                    const bool reinstalled = std::find(detectedErrors.begin(), detectedErrors.end(), current) == detectedErrors.end();
                    fixReport.finishError(reportId, reinstalled,
                        fixReport.tr("Syntaxfehler in einem installierten Paket (venv): Paket neu installiert, kein Code-Fix.",
                                     "Syntax error inside an installed package (venv): package reinstalled, no code fix."));
                    giveUp.push_back(current);    // don't retry this one via AI
                    continue;
                }
            }

            round++;
            bool fixed = false;
            const int reportId = fixReport.beginError(current);
            std::vector<std::string> history;  // failed attempts for this error, sent with the next request

            int budget = MAX_ITER;  // an attempt that makes progress earns one more (up to MAX_ATTEMPTS)
            for (int attempt = 1; attempt <= budget; ++attempt) {
                std::cout << "Attempt " << attempt << "/" << budget
                          << " for error: " << current
                          << " (difficulty: " << ErrorCollector::difficultyOf(current) << ")\n";

                // Always read the current source from disk so the AI sees the
                // latest state, including previously applied fixes.
                std::stringstream freshCollected;
                for (const auto& f : files) {
                    freshCollected << "===== FILE: " << f << " =====\n";
                    freshCollected << collector.readSourceCode(f) << "\n\n";
                }
                std::string sourceCode = freshCollected.str();

                FixRequest fix_req;
                fix_req.error_name   = current;
                fix_req.error_output = error_output;
                fix_req.language     = language;
                fix_req.source_code  = sourceCode;
                fix_req.checklist    = checkListRaw;
                fix_req.explanation_language = explanationLanguage;
                fix_req.previous_attempts = history;

                FixResult fix_res = fixClient.fix_code(fix_req);
                if (!fix_res.success) {
                    fixReport.addAttempt(reportId, attempt, fix_res, "", false, false, {}, {});
                    history.push_back(FixHistory::describeAttempt(attempt, fix_res.file_path, fix_res.explanation.fix,
                        "not applied (" + (fix_res.failure_reason.empty() ? std::string("no usable fix") : fix_res.failure_reason) + ")",
                        ""));
                    continue;
                }

                CodeChanger changer(reportRoot);  // project folder, also when the target is one file
                try {
                    changer.apply_fix(fix_res);
                } catch (const std::exception& e) {
                    // Not written (path, permissions, full disk, ...): record it and go on with the
                    // next attempt instead of ending the whole run without a report.
                    std::cout << "  -> fix could not be written: " << e.what() << "\n";
                    fixReport.addAttempt(reportId, attempt, fix_res, "", false, false, {}, {}, e.what());
                    history.push_back(FixHistory::describeAttempt(attempt, fix_res.file_path, fix_res.explanation.fix,
                        std::string("not applied (could not be written: ") + e.what() + ")", ""));
                    continue;
                }
                anyFixApplied = true;

                // Re-build / re-run and check whether the error is gone.
                const std::vector<std::string> errorsBefore = detectedErrors;
                const std::string outputBefore = error_output;
                const bool buildFailedBefore = lastBuildFailed;
                detectedErrors = redetect();
                const std::string outputAfter = error_output;
                bool gone = std::find(detectedErrors.begin(), detectedErrors.end(), current)
                            == detectedErrors.end();
                auto newCategories = [&] {
                    std::string s;
                    for (const auto& e : detectedErrors) {
                        if (std::find(errorsBefore.begin(), errorsBefore.end(), e) != errorsBefore.end()) continue;
                        s += (s.empty() ? "" : ", ") + e;
                    }
                    return s;
                };

                // Does the change stay? Only when it helped. A change that is taken back:
                //  - broke the build (runtime errors then vanish only because nothing runs any more)
                //  - only changed the error's form: categories vanished, new ones appeared and the
                //    program still crashes at the same place (e.g. Class.forName turning "No suitable
                //    driver" into a ClassNotFoundException)
                //  - left the error in place and added new error categories
                //  - had no effect: the error output is the same as before
                // A change after which the category is still there but the failure looks different
                // (the next failing assert, another test) is progress: it stays and earns one more attempt.
                // Python has no build step: a syntax error the fix introduced stops the program at
                // import, so every runtime error "vanishes". That is a broken build as well.
                auto pythonSyntaxError = [](const std::string& out) {
                    return out.find("SyntaxError:") != std::string::npos
                        || out.find("IndentationError:") != std::string::npos
                        || out.find("TabError:") != std::string::npos;
                };
                const bool brokeBuild = (!buildFailedBefore && lastBuildFailed)
                    || (detectedLanguage == "python" && pythonSyntaxError(outputAfter) && !pythonSyntaxError(outputBefore));

                std::string revertDe, revertEn, historyOutcome, stillPresentReason;
                if (brokeBuild) {
                    const std::string cats = newCategories();
                    revertDe = "Fix hat den Build gebrochen (" + cats + ")";
                    revertEn = "The fix broke the build (" + cats + ")";
                    historyOutcome = "the change broke the build (" + cats + ")";
                } else if (FixHistory::onlyTransformed(errorsBefore, detectedErrors, outputBefore, outputAfter,
                                                       files, fix_res.file_path)) {
                    const std::string site = FixHistory::crashSite(outputAfter, files, fix_res.file_path);
                    const std::string cats = newCategories();
                    revertDe = "Fehler hat nur seine Form geändert (" + cats
                             + "), das Programm stürzt weiter an derselben Stelle ab (" + site + ")";
                    revertEn = "The error only changed its form (" + cats
                             + "), the program still crashes at the same place (" + site + ")";
                    historyOutcome = "the error only changed its form into " + cats
                                   + " and the program still crashed at " + site;
                } else if (!gone) {
                    const std::string cats = newCategories();
                    if (!cats.empty()) {
                        revertDe = "Fehler besteht weiter und der Fix hat neue Fehler eingeführt (" + cats + ")";
                        revertEn = "The error is still present and the fix introduced new errors (" + cats + ")";
                        historyOutcome = "error still present and the change introduced new errors (" + cats + ")";
                    } else if (FixHistory::failureFingerprint(outputBefore) == FixHistory::failureFingerprint(outputAfter)) {
                        revertDe = "Fix hatte keine Wirkung, die Fehlerausgabe ist unverändert";
                        revertEn = "The fix had no effect, the error output is unchanged";
                        historyOutcome = "no effect, the error output did not change";
                    } else {
                        stillPresentReason = fixReport.tr(
                            "Fehlerausgabe hat sich geändert (Fortschritt), die Kategorie besteht aber weiter. Änderung bleibt.",
                            "The error output changed (progress), but the category is still present. The change was kept.");
                        history.push_back(FixHistory::describeAttempt(attempt, fix_res.file_path, fix_res.explanation.fix,
                            "progress: the error output changed but the category is still present; this change was KEPT",
                            outputAfter));
                        if (budget < MAX_ATTEMPTS) ++budget;
                        std::cout << "  -> progress: the error output changed, keeping this change\n";
                    }
                }

                if (!revertEn.empty()) {
                    gone = false;
                    std::cout << "  -> " << revertEn << ". Reverting this change.\n";
                    fs::path changedPath = fix_res.file_path;
                    if (changedPath.is_relative()) changedPath = fs::path(reportRoot) / changedPath;
                    bool reverted = false;
                    {
                        std::ofstream revert(changedPath, std::ios::binary | std::ios::trunc);
                        revert << changer.previousContent();
                        reverted = static_cast<bool>(revert);
                    }
                    if (!reverted) std::cout << "  -> could not write the previous version back to " << changedPath << "\n";
                    detectedErrors = redetect();  // state after the revert
                    stillPresentReason = reverted
                        ? fixReport.tr(revertDe + ". Änderung wurde zurückgenommen.", revertEn + ". The change was reverted.")
                        : fixReport.tr(revertDe + ". Zurücknehmen fehlgeschlagen, Änderung ist noch im Code.",
                                       revertEn + ". Reverting failed, the change is still in the code.");
                    history.push_back(FixHistory::describeAttempt(attempt, fix_res.file_path, fix_res.explanation.fix,
                        historyOutcome + (reverted ? "; this change was REVERTED" : "; reverting it FAILED, it is still in the code"),
                        outputAfter));
                }
                fixReport.addAttempt(reportId, attempt, fix_res, changer.previousContent(), true, gone, detectedErrors, {},
                                     "", stillPresentReason);
                fixReport.observe(detectedErrors, reportId);

                // optional: PDF mit dem Stand nach dieser Fix-Iteration (pdf_export.per_iteration)
                if (pdfConfig.perIteration) {
                    pdfExporter.exportIteration(++pdfIteration, {
                        {"iteration", pdfIteration}, {"error", current}, {"attempt", attempt},
                        {"file_path", fix_res.file_path}, {"error_fixed", gone},
                        {"remaining_errors", detectedErrors}, {"fixed_code", fix_res.fixed_code},
                        {"error_output", error_output}
                    }, parser.getJsonOutFile(), pdfName);
                }
                if (gone) {
                    std::cout << "  -> fixed. Remaining errors: " << detectedErrors.size() << "\n";
                    fixed = true;
                    break;
                }
                std::cout << "  -> error still present, retrying\n";
            }

            fixReport.finishError(reportId, fixed);
            if (!fixed) {
                std::cerr << "Could not fix error: " << current << "\n";
                giveUp.push_back(current);  // prevent infinite retries on same error
            }
        }

        if (detectedErrors.empty())
            std::cout << "\nAll errors fixed - project builds cleanly.\n";
        else
            std::cout << "\nFinished. " << giveUp.size() << " error(s) could not be fixed.\n";
    }

        // The report and the final analysis describe the state AFTER the fixes: the source code
        // as it is on disk now and the last run (redetect() keeps the program, debugger and
        // Valgrind results current). Before, they showed the state of the very first run.
        if (anyFixApplied) {
            std::stringstream finalCollected;
            for (const auto& f : files) {
                finalCollected << "===== FILE: " << f << " =====\n";
                finalCollected << collector.readSourceCode(f) << "\n\n";
            }
            sourceCode = finalCollected.str();
        }


        fixReport.finalize(detectedErrors);
        const nlohmann::json fixReportJson = fixReport.toJson({
            {"project", fs::path(targetPath).lexically_normal().filename().string()},
            {"target", targetPath},
            {"fix_description", parser.getFixDescription()},
            {"languages", language},
            {"run_id", pdfExporter.runId()},
            {"models", OpenAIClient::describeModels()}
        });

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

        // Environment problems of the last run (JSON, PDF and the final analysis): the model
        // should recommend fixing the environment, not the code.
        if (!detection.environment.empty()) {
            nlohmann::ordered_json withEnv = nlohmann::ordered_json::parse(report, nullptr, false);
            if (withEnv.is_object()) {
                nlohmann::ordered_json envList = nlohmann::ordered_json::array();
                for (const auto& env : detection.environment) {
                    envList.push_back({{"name", env}, {"hint", ErrorDetection::environmentHint(env)}});
                }
                withEnv["environment_problems"] = envList;
                withEnv["environment_caused_errors"] = std::vector<std::string>(
                    detection.environmentCaused.begin(), detection.environmentCaused.end());
                withEnv["environment_note"] =
                    "These errors come from the environment the program ran in (not from the code). "
                    "They were not fixed in code; recommend fixing the environment.";
                report = withEnv.dump(2, ' ', false, nlohmann::ordered_json::error_handler_t::replace);
            }
        }

        // Optional: JSON in Datei schreiben
        if (!parser.getJsonOutFile().empty()) {
            try {
                nlohmann::json final_json = nlohmann::json::parse(report);

                // Erweitere mit Error Detection Info
                final_json["detected_errors"] = detectedErrors;
                final_json["language"] = language;
                final_json["compile_output"] = compiler.getLastCompileOutput();
                final_json["fix_report"] = fixReportJson;

            std::ofstream out(parser.getJsonOutFile());
            out << final_json.dump(2);
            out.close();
            std::cout << "JSON Report gespeichert: " << parser.getJsonOutFile() << "\n\n";
            pdfExporter.exportJson(final_json.dump(2), parser.getJsonOutFile(), pdfName);
            } catch (const std::exception& e) {
                // Fallback: Wenn Parse fehlschlägt, speichere original
                std::ofstream out(parser.getJsonOutFile());
                out << report;
                out.close();
                std::cout << "JSON Report gespeichert: " << parser.getJsonOutFile() << "\n\n";
                pdfExporter.exportJson(report, parser.getJsonOutFile(), pdfName);
            }
        } else {
            // kein --json-out: PDF nur, wenn ein Ausgabeordner konfiguriert ist
            nlohmann::ordered_json pdf_json = nlohmann::ordered_json::parse(report, nullptr, false);
            if (pdf_json.is_object()) {
                pdf_json["detected_errors"] = detectedErrors;
                pdf_json["language"] = language;
                pdf_json["fix_report"] = fixReportJson;
                pdfExporter.exportJson(pdf_json.dump(2, ' ', false, nlohmann::ordered_json::error_handler_t::replace), "", pdfName);
            } else {
                pdfExporter.exportJson(report, "", pdfName);
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
            for (const auto& entry : reader.appendFromAiAnalysis(r.text, checklist)) {
                std::cout << "New Checklist Entry: " << entry << "\n";
            }

            if (!parser.getJsonOutFile().empty()) {
                try {
                    nlohmann::json final_json = nlohmann::json::parse(report);
                    final_json["detected_errors"] = detectedErrors;
                    final_json["language"] = language;
                    final_json["compile_output"] = compiler.getLastCompileOutput();
                    final_json["fix_report"] = fixReportJson;
                    final_json["ai_analysis"] = r.text;  // Markdown-Text vom AI

                    std::ofstream out(parser.getJsonOutFile());
                    out << final_json.dump(2);
                    out.close();
                    std::cout << "Final JSON Report aktualisiert: " << parser.getJsonOutFile() << "\n";
                    pdfExporter.exportJson(final_json.dump(2), parser.getJsonOutFile(), pdfName);
                } catch (...) {
                    // Ignore - JSON wurde schon vorher gespeichert
                }
            } else if (!pdfConfig.outputDir.empty()) {
                // kein --json-out, aber PDF-Ordner: Report inklusive AI-Analyse als PDF
                nlohmann::ordered_json final_json = nlohmann::ordered_json::parse(report, nullptr, false);
                if (final_json.is_object()) {
                    final_json["detected_errors"] = detectedErrors;
                    final_json["language"] = language;
                    final_json["fix_report"] = fixReportJson;
                    final_json["ai_analysis"] = r.text;
                    pdfExporter.exportJson(final_json.dump(2, ' ', false, nlohmann::ordered_json::error_handler_t::replace), "", pdfName);
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