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
        std::cout << "  --json-out <file>   Speichere JSON-Report in Datei\n";
        std::cout << "  --                  Argumente nach -- werden an das Programm übergeben\n";
        std::cout << "  -h, --help          Zeige diese Hilfe\n\n";
    }

    void ArgumentParser::printConfig() const {
        std::cout << "\n┌─ Buggy Konfiguration ─────────────────────────┐\n";
        std::cout << "│ Pfad:        " << targetPath << "\n";
        std::cout << "│ Typ:         " << (isDirectory() ? "Ordner" : "Datei") << "\n";
        std::cout << "│ Rekursiv:    " << (recursive ? "Ja" : "Nein") << "\n";
        std::cout << "│ Fix:         " << fixDescription << "\n";
        std::cout << "│ Extensions:  ";
        for (size_t i = 0; i < fileExtensions.size(); ++i) {
            std::cout << fileExtensions[i];
            if (i < fileExtensions.size() - 1) std::cout << ", ";
        }
        std::cout << "\n└───────────────────────────────────────────────┘\n\n";
    }