//
// Created by bercesteyagmur on 4/26/26.
//

#ifndef INC_2_BUGGY_AI_CHECKLISTREADER_H
#define INC_2_BUGGY_AI_CHECKLISTREADER_H

#include <string>
#include <vector>
#include <cctype>

struct ErrorCategory {
    std::string language; // "general", "cpp", "java", "python"
    std::string name;
    std::string detection_type;
    std::vector<std::string> keywords;
};

class ChecklistReader {
public:
    std::vector<ErrorCategory> load();
    std::string trim(const std::string& s);
    std::string loadRaw();
};

#endif //INC_2_BUGGY_AI_CHECKLISTREADER_H
