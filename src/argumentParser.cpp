#include "argumentParser.h"
#include <string>
#include <vector>
#include <iostream>
#include <stdexcept>
#include <filesystem>


void ArgumentParser::parse(int argc, char* argv[]){
        if (argc < 2) {
            throw std::invalid_argument("Insufficient arguments provided.");
        }

        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];

            if (arg == "--help" || arg == "-h") {
                printHelp(argv[0]);
                exit(0);
            } 
            else if (arg == "--recursive" || arg == "-r") {
                recursive = true;
            } 
            else if (arg == "--verbose" || arg == "-v") {
                verbose = true;
            } 
            else if (arg == "--path" || arg == "-p") {
                if (i + 1 >= argc) {
                    throw std::invalid_argument("Expected path after " + arg);
                }
                targetPath = argv[++i];
            }
            else if (arg == "--fix" || arg == "-f") {
                if (i + 1 >= argc) {
                    throw std::invalid_argument("Expected fix description after " + arg);
                }
                fixDescription = argv[++i];
            }
            else if (arg == "--extensions" || arg == "-e") {
                if (i + 1 >= argc) {
                    throw std::invalid_argument("Expected extensions after " + arg);
                }
                std::string extList = argv[++i];
                size_t pos = 0;
                while ((pos = extList.find(',')) != std::string::npos) {
                    fileExtensions.push_back(extList.substr(0, pos));
                    extList.erase(0, pos + 1);
                }
                fileExtensions.push_back(extList);
            } else if (arg == "--gdb") {
                useGdb = true;
            }
            else if (arg == "--valgrind") {
                useValgrind = true;
            }
            else if (arg == "--runtime") {
                runtimeChecks = true;
            }
            else if (arg == "--stack") {
                if (i + 1 >= argc) throw std::invalid_argument("Expected auto, fullstack or legacy after " + arg);
                stackMode = argv[++i];
                if (stackMode != "auto" && stackMode != "fullstack" && stackMode != "legacy") {
                    throw std::invalid_argument("Invalid value for --stack: " + stackMode + " (allowed: auto, fullstack, legacy)");
                }
            }
            else if (arg == "--no-pdf") {
                noPdf = true;
            }
            else if (arg == "--no-pdf-combined") {
                noPdfCombined = true;
            }
            else if (arg == "--pdf-per-iteration") {
                pdfPerIteration = true;
            }
            else if (arg == "--pdf-dir") {
                if (i + 1 >= argc) throw std::invalid_argument("Expected directory after " + arg);
                pdfDir = argv[++i];
            }
            else if (arg == "--run-timeout") {
                if (i + 1 >= argc) throw std::invalid_argument("Expected seconds after " + arg);
                std::string value = argv[++i];
                if (value.empty() || value.size() > 6 ||
                    value.find_first_not_of("0123456789") != std::string::npos) {
                    throw std::invalid_argument("Invalid value for --run-timeout: " + value + " (seconds, 0 = no limit)");
                }
                runTimeoutSec = std::stoi(value);
            }
            else if (arg == "--api-url") {
                if (i + 1 >= argc) throw std::invalid_argument("Expected url after " + arg);
                apiUrl = argv[++i];
            }
            else if (arg == "--api-token") {
                if (i + 1 >= argc) throw std::invalid_argument("Expected token after " + arg);
                apiToken = argv[++i];
            }
            else if (arg == "--json-out") {
                if (i + 1 >= argc) throw std::invalid_argument("Expected file after " + arg);
                jsonOutFile = argv[++i];
            }
            else if (arg == "--") {
                for (int j = i + 1; j < argc; ++j) passthroughArgs.push_back(argv[j]);
                break;
                
            } else if(targetPath.empty() && arg[0] != '-') {
                targetPath = arg;
            }
            else if (fixDescription.empty() && arg[0] != '-') {
                fixDescription = arg;
            }

        }

        if (targetPath.empty()) {
            throw std::invalid_argument("Target path is required.");
        }

        if (!std::filesystem::exists(targetPath)){
            throw std::invalid_argument("The specified path does not exist: " + targetPath);
        }

        if (fixDescription.empty()) {
            throw std::invalid_argument("Fix description is required.");
        }
    }

    std::string ArgumentParser::getTargetPath() const {
        return targetPath;
    } 
    std::string ArgumentParser::getFixDescription() const {
        return fixDescription;
    }
    bool ArgumentParser::isRecursive() const {
        return recursive;
    }
    bool ArgumentParser::isVerbose() const {
        return verbose;
    }
    std::vector<std::string>& ArgumentParser::getFileExtensions() {
        return fileExtensions;
    }

    bool ArgumentParser::isDirectory() const {
        return std::filesystem::is_directory(targetPath);
    }

    bool ArgumentParser::isGdbUsed() const { return useGdb; }
    bool ArgumentParser::isValgrindUsed() const { return useValgrind; }
    bool ArgumentParser::isRuntimeUsed() const { return runtimeChecks; }
    std::string ArgumentParser::getStackMode() const { return stackMode; }
    bool ArgumentParser::isPdfDisabled() const { return noPdf; }
    bool ArgumentParser::isPdfCombinedDisabled() const { return noPdfCombined; }
    bool ArgumentParser::isPdfPerIteration() const { return pdfPerIteration; }
    std::string ArgumentParser::getPdfDir() const { return pdfDir; }
    int ArgumentParser::getRunTimeoutSec() const { return runTimeoutSec; }
    std::string ArgumentParser::getApiUrl() const { return apiUrl; }
    std::string ArgumentParser::getApiToken() const { return apiToken; }
    std::string ArgumentParser::getJsonOutFile() const { return jsonOutFile; }
    const std::vector<std::string>& ArgumentParser::getPassthroughArgs() const { return passthroughArgs; }


    void ArgumentParser::printHelp(const std::string& programName) const {
        std::cout << "╔══════════════════════════════════════════════════════════╗\n";
        std::cout << "║              BUGGY - Code Fix Tool                       ║\n";
        std::cout << "╚══════════════════════════════════════════════════════════╝\n\n";

        std::cout << "VERWENDUNG:\n";
        std::cout << "  " << programName << " <pfad> <fix-beschreibung> [optionen]\n";
        std::cout << "  " << programName << " --path <pfad> --fix <beschreibung> [optionen]\n\n";

        std::cout << "ARGUMENTE:\n";
        std::cout << "  <pfad>              Datei oder Ordner zum Analysieren\n";
        std::cout << "  <fix-beschreibung>  Was soll gefixt werden\n\n";

        std::cout << "OPTIONEN:\n";
        std::cout << "  -p, --path          Pfad zur Datei oder zum Ordner\n";
        std::cout << "  -f, --fix           Beschreibung was gefixt werden soll\n";
        std::cout << "  -r, --recursive     Durchsuche Unterordner rekursiv\n";
        std::cout << "  -v, --verbose       Zeige detaillierte Ausgaben\n";
        std::cout << "  -e, --extensions    Dateiendungen\n";
        std::cout << "                      Beispiel: -e .cpp,.h,.txt\n";
        std::cout << "  --gdb               Nutze GDB für Crash-Analyse\n";
        std::cout << "  --valgrind          Nutze Valgrind für Memory-Check\n";
        std::cout << "  --runtime           PHP/JS: Tests mit Xdebug / Node Stacktraces ausführen\n";
        std::cout << "  --stack <mode>      Fullstack-Erkennung (Python + JS/TS + SQL + Services):\n";
        std::cout << "                      auto (Standard), fullstack (erzwingen), legacy (aus)\n";
        std::cout << "  --json-out <file>   Speichere JSON-Report in Datei\n";
        std::cout << "                      (dazu automatisch <file>.pdf und <file>.combined.pdf)\n";
        std::cout << "  --no-pdf            Keine PDF-Reports erzeugen\n";
        std::cout << "  --no-pdf-combined   Keinen Gesamt-Report (alle JSONs in einer PDF)\n";
        std::cout << "  --pdf-per-iteration Zusätzlich eine PDF nach jeder Fix-Iteration\n";
        std::cout << "  --pdf-dir <dir>     PDFs in diesen Ordner statt neben die JSON-Datei\n";
        std::cout << "  --run-timeout <s>   Zeitlimit für den Programmlauf in Sekunden (Standard 120,\n";
        std::cout << "                      0 = kein Limit). Server laufen bis zum Limit weiter\n";
        std::cout << "  --                  Argumente nach -- werden an das Programm übergeben\n";
        std::cout << "  -h, --help          Zeige diese Hilfe\n\n";
    }

    void ArgumentParser::printConfig() const {
        std::cout << "\n┌─ Buggy Konfiguration ─────────────────────────┐\n";
        std::cout << "│ Pfad:        " << targetPath << "\n";
        std::cout << "│ Typ:         " << (isDirectory() ? "Ordner" : "Datei") << "\n";
        std::cout << "│ Rekursiv:    " << (recursive ? "Ja" : "Nein") << "\n";
        std::cout << "│ Fix:         " << fixDescription << "\n";
        std::cout << "│ GDB:         " << (useGdb ? "Ja" : "Nein") << "\n";
        std::cout << "│ Valgrind:    " << (useValgrind ? "Ja" : "Nein") << "\n";
        std::cout << "│ Extensions:  ";
        for (size_t i = 0; i < fileExtensions.size(); ++i) {
            std::cout << fileExtensions[i];
            if (i < fileExtensions.size() - 1) std::cout << ", ";
        }
        std::cout << "\n";
        
        if (!passthroughArgs.empty()) {                                            
            std::cout << "│ Prog-Args:   ";
            for (size_t i = 0; i < passthroughArgs.size(); ++i) {
                std::cout << passthroughArgs[i];
                if (i < passthroughArgs.size() - 1) std::cout << " ";
            }
            std::cout << "\n";
        }
    
    if (!jsonOutFile.empty()) {                                                  
        std::cout << "│ JSON Output: " << jsonOutFile << "\n";
    }
        std::cout << "\n└───────────────────────────────────────────────┘\n\n";
    }