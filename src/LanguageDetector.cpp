#include "LanguageDetector.h"

bool LanguageDetector::endsWith(const std::string& str, const std::string& suffix) {
    return str.size() >= suffix.size() &&
           str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string LanguageDetector::detect(const std::vector<std::string>& files) {
    bool hasC = false;
    bool hasCpp = false;
    bool hasJava = false;
    bool hasPython = false;

    for (const auto& f : files) {

        if (endsWith(f, ".c")) {
            hasC = true;
        }

        if (endsWith(f, ".cpp") || endsWith(f, ".cc")) {
            hasCpp = true;
        }

        if (endsWith(f, ".java")) {
             hasJava = true;
        }

        if (endsWith(f, ".py")) {
            hasPython = true;
        }
    }

    if (hasJava && !hasCpp) return "java";
    if (hasC) return "c";
    if (hasCpp) return "cpp";
    if (hasPython) return "python";

    return "unknown";
}