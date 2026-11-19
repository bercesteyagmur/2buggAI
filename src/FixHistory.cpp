#include "FixHistory.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <regex>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace {

std::string stripAnsi(const std::string& s) {
    static const std::regex ansi("\x1B\\[[0-9;]*[A-Za-z]");
    return std::regex_replace(s, ansi, "");
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string clip(const std::string& s, size_t max) {
    return s.size() <= max ? s : s.substr(0, max) + " ...";
}

std::string baseName(const std::string& path) {
    return fs::path(path).filename().string();
}

bool isNoise(const std::string& line) {
    const std::string l = lower(line);
    return l.rfind("[info]", 0) == 0 || l.rfind("download", 0) == 0 || l.rfind("progress", 0) == 0
        || l.find("errors: 0") != std::string::npos   // "Tests run: 3, Failures: 0, Errors: 0"
        || l.rfind("warning:", 0) == 0;
}

bool carriesError(const std::string& line) {
    const std::string l = lower(line);
    static const char* const keys[] = {
        "exception", "error", "caused by", "traceback", "fatal", "failed", "no main manifest",
        "no suitable driver", "segmentation fault", "undefined reference"
    };
    return std::any_of(std::begin(keys), std::end(keys),
                       [&](const char* k) { return l.find(k) != std::string::npos; });
}

} // namespace

namespace FixHistory {

std::string errorExcerpt(const std::string& output, size_t maxLines, size_t maxLineLength) {
    std::vector<std::string> lines;
    std::vector<std::string> lastLines;
    std::set<std::string> seen;
    std::istringstream in(stripAnsi(output));
    std::string line;
    while (std::getline(in, line)) {
        const std::string t = trim(line);
        if (t.empty() || isNoise(t)) continue;
        lastLines.push_back(t);
        if (lastLines.size() > maxLines) lastLines.erase(lastLines.begin());
        if (lines.size() < maxLines && carriesError(t) && seen.insert(t).second) lines.push_back(t);
    }
    if (lines.empty()) lines = lastLines;

    std::string out;
    for (const auto& l : lines) out += clip(l, maxLineLength) + "\n";
    return out;
}

std::string describeAttempt(int number, const std::string& file, const std::string& fixExplanation,
                            const std::string& outcome, const std::string& outputAfter) {
    std::string s = "- Attempt " + std::to_string(number) + ": ";
    s += file.empty() ? "no file changed" : "changed " + file;
    if (!trim(fixExplanation).empty()) s += ". Change: " + clip(trim(fixExplanation), 500);
    s += ". Result: " + outcome + ".\n";
    const std::string excerpt = outputAfter.empty() ? "" : errorExcerpt(outputAfter, 8, 200);
    if (!excerpt.empty()) {
        s += "  Output after this attempt:\n";
        std::istringstream in(excerpt);
        std::string l;
        while (std::getline(in, l)) s += "    " + l + "\n";
    }
    return s;
}

std::vector<StackFrame> projectFrames(const std::string& output, const std::vector<std::string>& projectFiles) {
    std::set<std::string> names;
    for (const auto& f : projectFiles) names.insert(baseName(f));

    // Java: "\tat pkg.Class.method(File.java:12)" (innermost first)
    static const std::regex javaFrame(R"(^\s*at\s+[\w$.<>/]+\(([\w$.-]+\.(?:java|kt)):(\d+)\))");
    // Python: '  File "/p/app.py", line 12, in f' (outermost first)
    static const std::regex pyFrame(R"re(^\s*File "([^"]+)", line (\d+))re");

    std::vector<StackFrame> java, python;
    std::istringstream in(stripAnsi(output));
    std::string line;
    std::smatch m;
    while (std::getline(in, line)) {
        if (std::regex_search(line, m, javaFrame)) {
            if (names.count(baseName(m[1].str()))) java.push_back({m[1].str(), std::stoi(m[2].str())});
        } else if (std::regex_search(line, m, pyFrame)) {
            if (names.count(baseName(m[1].str()))) python.push_back({baseName(m[1].str()), std::stoi(m[2].str())});
        }
    }
    std::reverse(python.begin(), python.end());
    java.insert(java.end(), python.begin(), python.end());
    return java;
}

std::string crashSite(const std::string& output, const std::vector<std::string>& projectFiles,
                      const std::string& changedFile) {
    const std::string changed = baseName(changedFile);
    for (const auto& f : projectFrames(output, projectFiles)) {
        if (baseName(f.file) != changed) return baseName(f.file) + ":" + std::to_string(f.line);
    }
    return "";
}

std::string failureFingerprint(const std::string& output) {
    static const std::regex lineWord(R"(\bline \d+)");                          // Python: line 12
    static const std::regex fileLine(R"((\.[A-Za-z]{1,5}):\d+(:\d+)?)");        // Main.java:12, a.c:3:5
    static const std::regex address(R"(0x[0-9a-fA-F]+)");                       // gdb / Valgrind
    static const std::regex pid(R"(==\d+==)");                                  // Valgrind
    std::string s = errorExcerpt(output, 40, 400);
    s = std::regex_replace(s, lineWord, "line N");
    s = std::regex_replace(s, fileLine, "$1:N");
    s = std::regex_replace(s, address, "0x?");
    s = std::regex_replace(s, pid, "==?==");
    return s;
}

bool onlyTransformed(const std::vector<std::string>& errorsBefore, const std::vector<std::string>& errorsAfter,
                     const std::string& outputBefore, const std::string& outputAfter,
                     const std::vector<std::string>& projectFiles, const std::string& changedFile) {
    auto missingIn = [](const std::vector<std::string>& in) {
        return [&in](const std::string& e) { return std::find(in.begin(), in.end(), e) == in.end(); };
    };
    const bool newCategory = std::any_of(errorsAfter.begin(), errorsAfter.end(), missingIn(errorsBefore));
    const bool vanished    = std::any_of(errorsBefore.begin(), errorsBefore.end(), missingIn(errorsAfter));
    if (!newCategory || !vanished) return false;
    const std::string before = crashSite(outputBefore, projectFiles, changedFile);
    return !before.empty() && before == crashSite(outputAfter, projectFiles, changedFile);
}

} // namespace FixHistory
