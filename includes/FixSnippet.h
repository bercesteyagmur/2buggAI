#ifndef INC_2BUGGAI_FIXSNIPPET_H
#define INC_2BUGGAI_FIXSNIPPET_H

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

// One line of a code excerpt in the report, with its real line number in its file.
struct SnippetLine {
    int line = 0;          // 1-based line number in the old (or new) file
    std::string text;
    bool mark = false;     // old side: faulty / removed line, new side: added / changed line
};

// One contiguous region of a fix: the same lines before and after the fix.
struct SnippetHunk {
    std::vector<SnippetLine> oldLines;   // original file (context, removed and faulty lines)
    std::vector<SnippetLine> newLines;   // fixed file (context and added lines); empty for an excerpt
    bool truncated = false;              // lines after the last one shown were left out
};

struct Snippet {
    std::vector<SnippetHunk> hunks;
    int omittedHunks = 0;                // hunks left out because of the limit

    bool empty() const { return hunks.empty(); }
};

// Builds the "faulty code" / "fixed code" excerpts of a fix from the file content before and
// after it (see docs/PLAN_REPORT_REDESIGN.md, section 3).
class FixSnippet {
public:
    static constexpr size_t MAX_HUNKS = 8;
    static constexpr size_t MAX_LINES_PER_HUNK = 120;   // per side

    // Excerpts of a fix. errorLines are 1-based lines of the OLD text that a tool or the model
    // reported as faulty; they are marked even when the fix did not change them, and they get
    // their own hunk when they are far from the change. context = unchanged lines shown
    // before and after (cut off at the start and end of the file). Changes whose context
    // overlaps are merged into one hunk, like in a unified diff.
    static Snippet build(const std::string& oldText, const std::string& newText,
                         const std::vector<int>& errorLines, int context,
                         size_t maxHunks = MAX_HUNKS, size_t maxLinesPerHunk = MAX_LINES_PER_HUNK);

    // Excerpt of a file without a fix: the error lines with context (old side only).
    static Snippet excerpt(const std::string& text, const std::vector<int>& errorLines, int context,
                           size_t maxHunks = MAX_HUNKS, size_t maxLinesPerHunk = MAX_LINES_PER_HUNK);

    // 1-based lines of the old text that the fix removes or replaces.
    static std::vector<int> changedOldLines(const std::string& oldText, const std::string& newText);

    static nlohmann::json toJson(const Snippet& snippet);
    static Snippet fromJson(const nlohmann::json& j);   // tolerant: unknown / broken entries are skipped
};

#endif //INC_2BUGGAI_FIXSNIPPET_H
