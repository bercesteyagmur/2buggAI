#include "LanguageDetector.h"

#include <filesystem>

bool LanguageDetector::endsWith(const std::string& str, const std::string& suffix) {
    return str.size() >= suffix.size() &&
           str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string LanguageDetector::languageOf(const std::string& path) {
    static const std::map<std::string, std::string> languages = {
        {".c", "c"}, {".h", "c"},
        {".cpp", "cpp"}, {".cc", "cpp"}, {".cxx", "cpp"}, {".hpp", "cpp"}, {".hh", "cpp"},
        {".java", "java"}, {".kt", "kotlin"},
        {".py", "python"},
        {".js", "javascript"}, {".mjs", "javascript"}, {".cjs", "javascript"}, {".jsx", "javascript"},
        {".ts", "typescript"}, {".tsx", "typescript"},
        {".vue", "vue"},
        {".php", "php"},
        {".twig", "twig"},
        {".sql", "sql"},
        {".go", "go"},
        {".rb", "ruby"},
        {".cs", "csharp"},
        {".rs", "rust"},
    };

    std::string filename = std::filesystem::path(path).filename().string();

    if (filename.size() > 10 && filename.compare(filename.size() - 10, 10, ".blade.php") == 0) {
        return "blade";
    }

    auto it = languages.find(std::filesystem::path(path).extension().string());
    return it == languages.end() ? "" : it->second;
}

std::map<std::string, int> LanguageDetector::detectAll(const std::vector<std::string>& files) {
    std::map<std::string, int> counts;

    for (const auto& f : files) {
        std::string lang = languageOf(f);
        if (!lang.empty()) {
            counts[lang]++;
        }
    }

    return counts;
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
