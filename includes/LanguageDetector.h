//
// Created by bercesteyagmur on 4/26/26.
//

#ifndef INC_2_BUGGY_AI_LANGUAGEDETECTOR_H
#define INC_2_BUGGY_AI_LANGUAGEDETECTOR_H

#include <map>
#include <string>
#include <vector>

class LanguageDetector {
private:
    bool endsWith(const std::string& str, const std::string& suffix);

public:
    std::string detect(const std::vector<std::string>& files);

    std::map<std::string, int> detectAll(const std::vector<std::string>& files);

    static std::string languageOf(const std::string& path);
};

#endif //INC_2_BUGGY_AI_LANGUAGEDETECTOR_H
