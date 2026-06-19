#include "ErrorMatcher.h"
#include <algorithm>

ErrorMatcher::ErrorMatcher(const std::vector<ErrorCategory>& checklist): checklist(checklist) {}

static std::string toLower(const std::string& s) {
    std::string result = s;

    for (char& c : result) {
        c = std::tolower(c);
    }

    return result;
}

static bool contains(const std::string& text, const std::string& sub) {

    if (sub.empty()) return true;
    if (sub.size() > text.size()) return false;

    for (size_t i = 0; i <= text.size() - sub.size(); i++) {

        if (text.substr(i, sub.size()) == sub) {
            return true;
        }
    }

    return false;
}

std::vector<std::string> ErrorMatcher::match(const std::string& error_output, const std::string& language) {

    std::vector<std::string> detected;

    std::string lower = toLower(error_output);



    for (const auto& errorCategory  : checklist) {

        if (!errorCategory.language.empty() && errorCategory.language != "general" && errorCategory.language != language) {
            continue;
        }

        if (errorCategory.detection_type == "detect_from_output") {

            for (const auto& keyword  : errorCategory.keywords) {
                std::string lowerKeyword = toLower(keyword);


                if (contains(lower, lowerKeyword)) {
                    detected.push_back(errorCategory.name);
                    break;
                }
            }
        }
    }

    return detected;
}