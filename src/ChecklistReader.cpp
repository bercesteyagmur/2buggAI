#include "ChecklistReader.h"
#include <fstream>
#include <sstream>

std::vector<ErrorCategory> ChecklistReader::load() {

    std::ifstream file("errorchecklist.txt");

    if (!file.is_open()) {
        throw std::runtime_error("Failed to open checklist file: errorchecklist.txt");
    }

    std::vector<ErrorCategory> error_list;

    std::string line;

    // When reading from a file, the content is treated as a stream of characters.
    // std::getline reads characters one by one and stores them into a string
    // until it reaches a newline character '\n'.
    // That means each line in the file becomes one full string.

    std::string current_language = "general";

    while (std::getline(file, line)) {

        // text file is basically a sequence of characters.
        // lines are separated by '\n' (newline).
        // so getline reads everything up to '\n' and puts it into a string, so line.
        // file = where we read data from
        // line = where the read data is stored

        // Skip empty lines and comment lines
        /* if (line.empty()) {
            continue;
        }

        if (line == "# ---------------- JAVA ----------------") {
            current_language = "java";
            continue;
        }

        if (line == "# ---------------- GENERAL ----------------") {
            current_language = "general";
            continue;
        }

        if (line == "# ---------------- C / C++ ----------------") {
            current_language = "cpp";
            continue;
        }

        if (line == "# ---------------- PYTHON ----------------") {
            current_language = "python";
            continue;
        }

        if (line.rfind("#", 0) == 0) {
            continue;
        }
        */

        if (line.empty()) {
            continue;
        }

        if (line.rfind("#", 0) == 0) {
            continue;
        }

        // parse
        std::stringstream ss(line);

        std::string language, name, type, keywords_str;

        std::getline(ss, language, '|');
        std::getline(ss, name, '|');
        std::getline(ss, type, '|');
        std::getline(ss, keywords_str);

        ErrorCategory error;
        error.name = trim(name);
        error.detection_type = trim(type);
        error.language = trim(language);

        // split keywords
        std::stringstream ks(keywords_str);
        std::string keyword;

        while (std::getline(ks, keyword, ',')) {
            keyword = trim(keyword);

            if (keyword == "-" || keyword.empty()) continue;

            error.keywords.push_back(keyword);
        }

        error_list.push_back(error);
    }

    return error_list;
}

std::string ChecklistReader::trim(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
        start++;
    }

    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        end--;
    }

    return s.substr(start, end - start);
}

// Reads the entire checklist file as a raw string (including comments and formatting).
std::string ChecklistReader::loadRaw() {
    std::ifstream file("errorchecklist.txt");

    if (!file.is_open()) {
        throw std::runtime_error("Failed to open checklist file: errorchecklist.txt");
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

bool ChecklistReader::appendIfNew(const std::string& errorName, const std::string& language, const std::vector<ErrorCategory>& existing) {         
    //Check if name already exists
    for (const auto& cat : existing) {
        if (cat.name == errorName) {
            return false;  // already there
        }
    }

    // Validate language (fallback to "general" if invalid)
    std::string lang = language;
    if (lang != "general" && lang != "c" && lang != "cpp"
        && lang != "java" && lang != "python"
        && lang != "php" && lang != "javascript" && lang != "typescript"
        && lang != "vue" && lang != "sql") {
        lang = "general";
    }

    // Append in 4 column format
    std::ofstream file("errorchecklist.txt", std::ios::app);
    if (!file.is_open()) {
        return false;
    }
    
    file << "\n" << lang << " | " << errorName << " | detect_from_output | " << errorName << "\n";
    file.close();
    return true;
}

std::vector<std::string> ChecklistReader::appendFromAiAnalysis(const std::string& analysis, const std::vector<ErrorCategory>& existing) {
    std::vector<std::string> added;
    std::istringstream iss(analysis);
    std::string textLine;
    const std::string categoryMarker = "**Category:**";
    const std::string languageMarker = "**Language:**";

    std::string pendingCategory;

    while (std::getline(iss, textLine)) {
        size_t catPos = textLine.find(categoryMarker);
        if (catPos != std::string::npos) {
            std::string category = textLine.substr(catPos + categoryMarker.size());
            size_t slash = category.find('/');
            if (slash != std::string::npos) category = category.substr(0, slash);
            pendingCategory = trim(category);
            continue;
        }

        size_t langPos = textLine.find(languageMarker);
        if (langPos != std::string::npos) {
            std::string lang = textLine.substr(langPos + languageMarker.size());
            size_t slash = lang.find('/');
            if (slash != std::string::npos) lang = lang.substr(0, slash);
            lang = trim(lang);

            if (!pendingCategory.empty() && pendingCategory != "other"
                && appendIfNew(pendingCategory, lang, existing)) {
                added.push_back(pendingCategory + " (" + lang + ")");
            }
            pendingCategory.clear();
        }
    }
    return added;
}
