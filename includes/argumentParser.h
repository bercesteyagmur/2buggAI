#pragma once
#include <string>
#include <vector>
#include <iostream>
#include <stdexcept>
#include <filesystem>

class ArgumentParser {
    private: 
        std::string targetPath;
        std::string fixDescription;
        bool recursive{true};
        bool verbose{false};
        std::vector<std::string> fileExtensions;
        bool useGdb{false};
        bool useValgrind{false};
        bool runtimeChecks{false};
        std::string stackMode{"auto"};   // auto | fullstack | legacy
        bool noPdf{false};               // --no-pdf
        bool noPdfCombined{false};       // --no-pdf-combined
        bool pdfPerIteration{false};     // --pdf-per-iteration
        std::string pdfDir;              // --pdf-dir <dir>
        int runTimeoutSec{120};          // --run-timeout <sec>, 0 = no limit
        std::string apiUrl;
        std::string apiToken;
        std::string jsonOutFile;
        std::vector<std::string> passthroughArgs;


    public:

    

    std::string getTargetPath() const;
    std::string getFixDescription() const;
    bool isRecursive() const;
    bool isVerbose() const;
    std::vector<std::string>& getFileExtensions();
    bool isGdbUsed() const;
    bool isValgrindUsed() const;
    bool isRuntimeUsed() const;
    std::string getStackMode() const;
    bool isPdfDisabled() const;
    bool isPdfCombinedDisabled() const;
    bool isPdfPerIteration() const;
    std::string getPdfDir() const;
    int getRunTimeoutSec() const;
    std::string getApiUrl() const;
    std::string getApiToken() const;
    std::string getJsonOutFile() const;
    const std::vector<std::string>& getPassthroughArgs() const;

    void parse(int argc, char* argv[]);

    bool isDirectory() const;

    void printHelp(const std::string& programName) const;

    void printConfig() const;

    
};
