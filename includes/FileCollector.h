//
// Created by bercesteyagmur on 4/26/26.
//

#ifndef INC_2_BUGGY_AI_FILECOLLECTOR_H
#define INC_2_BUGGY_AI_FILECOLLECTOR_H

#include <string>
#include <vector>

class FileCollector {

private:
    bool isSourceFile(const std::string& path);
    bool isBuildDirectory(const std::string& path);

public:
    std::vector<std::string> collectSourceFiles(const std::string& path, bool recursive);
    std::vector<std::string> collectIncludeDirs(const std::string& path, bool recursive);
    std::string collectSourceCode(const std::vector<std::string>& files);
    std::string readSourceCode(const std::string& path);
    int countLines(const std::string& filePath);

};

#endif //INC_2_BUGGY_AI_FILECOLLECTOR_H
