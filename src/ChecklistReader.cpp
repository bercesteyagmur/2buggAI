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
        if (line.empty()) {
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

        // parse
        std::stringstream ss(line);

        std::string name, type, keywords_str;

        std::getline(ss, name, '|');
        std::getline(ss, type, '|');
        std::getline(ss, keywords_str);

        ErrorCategory error;
        error.name = trim(name);
        error.detection_type = trim(type);
        error.language = current_language;

        // split keywords
        std::stringstream ks(keywords_str);
        std::string keyword;

        while (std::getline(ks, keyword, ',')) {
            keyword = trim(keyword);

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