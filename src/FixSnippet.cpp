#include "FixSnippet.h"
#include "UnifiedDiff.h"

#include <algorithm>
#include <set>

using nlohmann::json;

namespace {

// Groups the "interesting" operation indices (sorted) into hunks [begin, end) of the
// operation list, with `context` unchanged operations before and after each group.
// Two groups whose context would touch or overlap become one hunk.
std::vector<std::pair<size_t, size_t>> groupHunks(const std::vector<size_t>& interesting, size_t opCount,
                                                  size_t context) {
    std::vector<std::pair<size_t, size_t>> ranges;
    size_t i = 0;
    while (i < interesting.size()) {
        const size_t first = interesting[i];
        size_t last = first;
        while (i + 1 < interesting.size() && interesting[i + 1] - last <= 2 * context + 1) last = interesting[++i];
        ++i;
        const size_t begin = first > context ? first - context : 0;
        const size_t end = std::min(opCount, last + context + 1);
        ranges.emplace_back(begin, end);
    }
    return ranges;
}

void limitLines(std::vector<SnippetLine>& lines, size_t maxLines, bool& truncated) {
    if (lines.size() > maxLines) {
        lines.resize(maxLines);
        truncated = true;
    }
}

std::set<int> lineSet(const std::vector<int>& lines, size_t lineCount) {
    std::set<int> out;
    for (int l : lines) {
        if (l >= 1 && static_cast<size_t>(l) <= lineCount) out.insert(l);
    }
    return out;
}

} // namespace

Snippet FixSnippet::build(const std::string& oldText, const std::string& newText,
                          const std::vector<int>& errorLines, int context,
                          size_t maxHunks, size_t maxLinesPerHunk) {
    const std::vector<std::string> a = diff_split_lines(oldText);
    const std::vector<std::string> b = diff_split_lines(newText);
    const std::vector<DiffOp> ops = diff_lines(a, b);
    const std::set<int> faulty = lineSet(errorLines, a.size());
    const size_t ctx = static_cast<size_t>(std::max(context, 0));

    auto isFaulty = [&](const DiffOp& op) {
        return op.type != '+' && faulty.count(static_cast<int>(op.a) + 1) > 0;
    };

    std::vector<size_t> interesting;
    for (size_t k = 0; k < ops.size(); ++k) {
        if (ops[k].type != ' ' || isFaulty(ops[k])) interesting.push_back(k);
    }

    Snippet snippet;
    for (const auto& [begin, end] : groupHunks(interesting, ops.size(), ctx)) {
        if (snippet.hunks.size() >= maxHunks) {
            ++snippet.omittedHunks;
            continue;
        }
        SnippetHunk hunk;
        for (size_t k = begin; k < end; ++k) {
            const DiffOp& op = ops[k];
            if (op.type != '+') {
                hunk.oldLines.push_back({static_cast<int>(op.a) + 1, a[op.a], op.type == '-' || isFaulty(op)});
            }
            if (op.type != '-') {
                hunk.newLines.push_back({static_cast<int>(op.b) + 1, b[op.b], op.type == '+'});
            }
        }
        limitLines(hunk.oldLines, maxLinesPerHunk, hunk.truncated);
        limitLines(hunk.newLines, maxLinesPerHunk, hunk.truncated);
        snippet.hunks.push_back(std::move(hunk));
    }
    return snippet;
}

Snippet FixSnippet::excerpt(const std::string& text, const std::vector<int>& errorLines, int context,
                            size_t maxHunks, size_t maxLinesPerHunk) {
    const std::vector<std::string> lines = diff_split_lines(text);
    const std::set<int> faulty = lineSet(errorLines, lines.size());
    const size_t ctx = static_cast<size_t>(std::max(context, 0));

    std::vector<size_t> interesting;
    for (int l : faulty) interesting.push_back(static_cast<size_t>(l - 1));

    Snippet snippet;
    for (const auto& [begin, end] : groupHunks(interesting, lines.size(), ctx)) {
        if (snippet.hunks.size() >= maxHunks) {
            ++snippet.omittedHunks;
            continue;
        }
        SnippetHunk hunk;
        for (size_t k = begin; k < end; ++k) {
            const int number = static_cast<int>(k) + 1;
            hunk.oldLines.push_back({number, lines[k], faulty.count(number) > 0});
        }
        limitLines(hunk.oldLines, maxLinesPerHunk, hunk.truncated);
        snippet.hunks.push_back(std::move(hunk));
    }
    return snippet;
}

std::vector<int> FixSnippet::changedOldLines(const std::string& oldText, const std::string& newText) {
    const std::vector<DiffOp> ops = diff_lines(diff_split_lines(oldText), diff_split_lines(newText));
    std::vector<int> lines;
    for (const auto& op : ops) {
        if (op.type == '-') lines.push_back(static_cast<int>(op.a) + 1);
    }
    return lines;
}

json FixSnippet::toJson(const Snippet& snippet) {
    auto side = [](const std::vector<SnippetLine>& lines) {
        json out = json::array();
        for (const auto& l : lines) out.push_back({{"line", l.line}, {"text", l.text}, {"mark", l.mark}});
        return out;
    };
    json hunks = json::array();
    for (const auto& h : snippet.hunks) {
        hunks.push_back({{"old", side(h.oldLines)}, {"new", side(h.newLines)}, {"truncated", h.truncated}});
    }
    return {{"hunks", hunks}, {"omitted_hunks", snippet.omittedHunks}};
}

Snippet FixSnippet::fromJson(const json& j) {
    auto side = [](const json& lines) {
        std::vector<SnippetLine> out;
        if (!lines.is_array()) return out;
        for (const auto& l : lines) {
            if (!l.is_object() || !l.contains("line") || !l["line"].is_number_integer()) continue;
            SnippetLine line;
            line.line = l["line"].get<int>();
            if (l.contains("text") && l["text"].is_string()) line.text = l["text"].get<std::string>();
            line.mark = l.contains("mark") && l["mark"].is_boolean() && l["mark"].get<bool>();
            out.push_back(std::move(line));
        }
        return out;
    };

    Snippet snippet;
    if (!j.is_object()) return snippet;
    if (j.contains("hunks") && j["hunks"].is_array()) {
        for (const auto& h : j["hunks"]) {
            if (!h.is_object()) continue;
            SnippetHunk hunk;
            if (h.contains("old")) hunk.oldLines = side(h["old"]);
            if (h.contains("new")) hunk.newLines = side(h["new"]);
            hunk.truncated = h.contains("truncated") && h["truncated"].is_boolean() && h["truncated"].get<bool>();
            if (!hunk.oldLines.empty() || !hunk.newLines.empty()) snippet.hunks.push_back(std::move(hunk));
        }
    }
    if (j.contains("omitted_hunks") && j["omitted_hunks"].is_number_integer()) {
        snippet.omittedHunks = std::max(0, j["omitted_hunks"].get<int>());
    }
    return snippet;
}
