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
        bool recursive{false};
        bool verbose{false};
        std::vector<std::string> fileExtensions;
        bool useGdb{false};
        bool useValgrind{false};
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
    std::string getApiUrl() const;
    std::string getApiToken() const;
    std::string getJsonOutFile() const;
    const std::vector<std::string>& getPassthroughArgs() const;

    void parse(int argc, char* argv[]);

    bool isDirectory() const;

    void printHelp(const std::string& programName) const;

    void printConfig() const;

    
};
