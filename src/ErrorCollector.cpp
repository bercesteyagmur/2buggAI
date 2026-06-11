#include "ErrorCollector.h"
#include <algorithm>
#include <sstream>
#include <cctype>

namespace {

std::string toLower(const std::string& s) {
    std::string result = s;

    for (char& c : result) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    return result;
}

bool contains(const std::string& text, const std::string& sub) {
    return text.find(sub) != std::string::npos;
}

// Schwer: Speicher- und Nebenläufigkeitsfehler
const std::vector<std::string>& hardKeywords() {
    static const std::vector<std::string> keywords = {
        "segmentation fault", "segfault", "double free", "buffer overflow",
        "invalid memory access", "deadlock", "race condition",
        "stack overflow", "heap corruption", "use after free", "recursionerror"
    };
    return keywords;
}

// Mittel: Typ-, Logik- und Zugriffsfehler
const std::vector<std::string>& mediumKeywords() {
    static const std::vector<std::string> keywords = {
        "type mismatch", "cannot convert", "null", "nonetype",
        "out of bounds", "out of range", "indexerror", "keyerror",
        "attributeerror", "nullpointerexception", "classcastexception",
        "logic error"
    };
    return keywords;
}

// Allgemeine Begriffe, die unabhängig von der Schwierigkeit auf einen Fehler hindeuten
const std::vector<std::string>& generalErrorKeywords() {
    static const std::vector<std::string> keywords = {
        "error", "exception", "fatal"
    };
    return keywords;
}

bool matchesAny(const std::string& lower, const std::vector<std::string>& keywords) {
    for (const auto& keyword : keywords) {
        if (contains(lower, keyword)) return true;
    }
    return false;
}

// "segmentation_fault" -> "segmentation fault" (für Vergleich mit Keywords, die Leerzeichen enthalten)
std::string underscoresToSpaces(const std::string& s) {
    std::string result = s;
    std::replace(result.begin(), result.end(), '_', ' ');
    return result;
}

// "recursion_error" -> "recursionerror" (für Vergleich mit zusammengeschriebenen Keywords)
std::string withoutUnderscores(const std::string& s) {
    std::string result;
    result.reserve(s.size());

    for (char c : s) {
        if (c != '_') result.push_back(c);
    }

    return result;
}

// Schwierigkeitsstufen: 0 = leicht, 1 = mittel, 2 = schwer
// Funktioniert sowohl mit rohen Compiler-/Programmzeilen als auch mit
// Checklist-Kategorienamen wie "segmentation_fault" oder "type_mismatch".
int difficultyScore(const std::string& error) {
    const std::string lower = toLower(error);
    const std::string spaced = underscoresToSpaces(lower);
    const std::string compact = withoutUnderscores(lower);

    if (matchesAny(spaced, hardKeywords()) || matchesAny(compact, hardKeywords())) return 2;
    if (matchesAny(spaced, mediumKeywords()) || matchesAny(compact, mediumKeywords())) return 1;

    // Leicht: z.B. Syntax-/Tippfehler, fehlendes Semikolon, undeklarierte Variablen
    return 0;
}

std::string difficultyLabel(int score) {
    switch (score) {
        case 0:  return "leicht";
        case 1:  return "mittel";
        default: return "schwer";
    }
}

} // namespace

std::vector<std::string> ErrorCollector::collectErrors(const std::string& compileOutput) {
    std::vector<std::string> errors;
    std::istringstream stream(compileOutput);
    std::string line;

    while (std::getline(stream, line)) {
        if (line.empty()) continue;

        const std::string lower = toLower(line);

        if (matchesAny(lower, generalErrorKeywords()) ||
            matchesAny(lower, hardKeywords()) ||
            matchesAny(lower, mediumKeywords())) {
            errors.push_back(line);
        }
    }

    return errors;
}

std::vector<std::string> ErrorCollector::sortedErrors(const std::vector<std::string>& errors) {
    std::vector<std::string> sorted = errors;

    std::stable_sort(sorted.begin(), sorted.end(), [](const std::string& a, const std::string& b) {
        return difficultyScore(a) < difficultyScore(b);
    });

    return sorted;
}

std::string ErrorCollector::difficultyOf(const std::string& error) {
    return difficultyLabel(difficultyScore(error));
}
