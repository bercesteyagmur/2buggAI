#include "LanguageDetector.h"

bool LanguageDetector::endsWith(const std::string& str, const std::string& suffix) {
    return str.size() >= suffix.size() &&
           str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string LanguageDetector::detect(const std::vector<std::string>& files) {

    for (const auto& f : files) {

        if (endsWith(f, ".cpp") || endsWith(f, ".cc") || endsWith(f, ".c")) {
            return "cpp";
        }

        if (endsWith(f, ".java")) {
            return "java";
        }

        if (endsWith(f, ".py")) {
            return "python";
        }
    }

    return "general";
}