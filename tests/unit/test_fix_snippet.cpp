#include "minitest.h"

#include "FixSnippet.h"
#include "UnifiedDiff.h"

#include <string>
#include <vector>

namespace {

// "line 1\nline 2\n...\nline n\n"
std::string numbered(int n) {
    std::string text;
    for (int i = 1; i <= n; ++i) text += "line " + std::to_string(i) + "\n";
    return text;
}

std::string replaceLine(const std::string& text, int line, const std::string& replacement) {
    std::vector<std::string> lines = diff_split_lines(text);
    lines[static_cast<size_t>(line - 1)] = replacement;
    std::string out;
    for (const auto& l : lines) out += l + "\n";
    return out;
}

std::vector<int> numbersOf(const std::vector<SnippetLine>& lines) {
    std::vector<int> out;
    for (const auto& l : lines) out.push_back(l.line);
    return out;
}

std::vector<int> markedOf(const std::vector<SnippetLine>& lines) {
    std::vector<int> out;
    for (const auto& l : lines) {
        if (l.mark) out.push_back(l.line);
    }
    return out;
}

std::vector<int> range(int first, int last) {
    std::vector<int> out;
    for (int i = first; i <= last; ++i) out.push_back(i);
    return out;
}

} // namespace

TEST_CASE("snippet: changed line with the configured context on both sides") {
    const std::string before = numbered(20);
    const std::string after = replaceLine(before, 10, "line 10 fixed");

    const Snippet s = FixSnippet::build(before, after, {}, 3);
    REQUIRE(s.hunks.size() == 1);
    CHECK(numbersOf(s.hunks[0].oldLines) == range(7, 13));
    CHECK(numbersOf(s.hunks[0].newLines) == range(7, 13));
    CHECK(markedOf(s.hunks[0].oldLines) == std::vector<int>{10});
    CHECK(markedOf(s.hunks[0].newLines) == std::vector<int>{10});
    CHECK(s.hunks[0].oldLines[3].text == "line 10");
    CHECK(s.hunks[0].newLines[3].text == "line 10 fixed");
    CHECK(s.omittedHunks == 0);
    CHECK(!s.hunks[0].truncated);

    // other amounts of context
    CHECK(numbersOf(FixSnippet::build(before, after, {}, 0).hunks[0].oldLines) == std::vector<int>{10});
    CHECK(numbersOf(FixSnippet::build(before, after, {}, 5).hunks[0].oldLines) == range(5, 15));
    CHECK(numbersOf(FixSnippet::build(before, after, {}, -2).hunks[0].oldLines) == std::vector<int>{10});   // treated as 0
}

TEST_CASE("snippet: context is cut off at the start and the end of the file") {
    const std::string before = numbered(20);

    const Snippet first = FixSnippet::build(before, replaceLine(before, 1, "first"), {}, 3);
    REQUIRE(first.hunks.size() == 1);
    CHECK(numbersOf(first.hunks[0].oldLines) == range(1, 4));
    CHECK(markedOf(first.hunks[0].oldLines) == std::vector<int>{1});

    const Snippet last = FixSnippet::build(before, replaceLine(before, 20, "last"), {}, 3);
    REQUIRE(last.hunks.size() == 1);
    CHECK(numbersOf(last.hunks[0].oldLines) == range(17, 20));
    CHECK(numbersOf(last.hunks[0].newLines) == range(17, 20));

    // a file of two lines with a large context
    const Snippet tiny = FixSnippet::build("a\nb\n", "a\nB\n", {}, 10);
    REQUIRE(tiny.hunks.size() == 1);
    CHECK(numbersOf(tiny.hunks[0].oldLines) == range(1, 2));

    // appended at the end (old file without a trailing newline)
    const Snippet appended = FixSnippet::build("a\nb", "a\nb\nc\n", {}, 3);
    REQUIRE(appended.hunks.size() == 1);
    CHECK(markedOf(appended.hunks[0].newLines) == std::vector<int>{3});
    CHECK(markedOf(appended.hunks[0].oldLines).empty());
}

TEST_CASE("snippet: distant changes become several hunks, close ones are merged") {
    const std::string before = numbered(30);
    std::string after = replaceLine(before, 3, "three");
    after = replaceLine(after, 20, "twenty");

    const Snippet two = FixSnippet::build(before, after, {}, 3);
    REQUIRE(two.hunks.size() == 2);
    CHECK(numbersOf(two.hunks[0].oldLines) == range(1, 6));
    CHECK(numbersOf(two.hunks[1].oldLines) == range(17, 23));
    CHECK(markedOf(two.hunks[1].newLines) == std::vector<int>{20});

    // 5 and 12: gap of 7 = 2 * context + 1 -> the contexts touch, one hunk
    std::string close = replaceLine(before, 5, "five");
    close = replaceLine(close, 12, "twelve");
    const Snippet one = FixSnippet::build(before, close, {}, 3);
    REQUIRE(one.hunks.size() == 1);
    CHECK(numbersOf(one.hunks[0].oldLines) == range(2, 15));
    CHECK(markedOf(one.hunks[0].oldLines) == (std::vector<int>{5, 12}));

    // 5 and 13: one line more in between -> two hunks
    std::string apart = replaceLine(before, 5, "five");
    apart = replaceLine(apart, 13, "thirteen");
    CHECK(FixSnippet::build(before, apart, {}, 3).hunks.size() == 2);
}

TEST_CASE("snippet: inserted lines shift the numbers of the new file") {
    const std::string before = numbered(20);
    // two lines inserted after line 4, and line 15 changed further down
    std::vector<std::string> lines = diff_split_lines(before);
    lines.insert(lines.begin() + 4, {"new A", "new B"});
    lines[16] = "line 15 fixed";   // old line 15 is now at index 16 (line 17)
    std::string after;
    for (const auto& l : lines) after += l + "\n";

    const Snippet s = FixSnippet::build(before, after, {}, 2);
    REQUIRE(s.hunks.size() == 2);

    // first hunk: the insertion
    CHECK(numbersOf(s.hunks[0].oldLines) == range(3, 6));                 // context only
    CHECK(markedOf(s.hunks[0].oldLines).empty());
    CHECK(numbersOf(s.hunks[0].newLines) == range(3, 8));
    CHECK(markedOf(s.hunks[0].newLines) == (std::vector<int>{5, 6}));
    CHECK(s.hunks[0].newLines[2].text == "new A");

    // second hunk: old line 15 is new line 17, and so is its context
    CHECK(numbersOf(s.hunks[1].oldLines) == range(13, 17));
    CHECK(markedOf(s.hunks[1].oldLines) == std::vector<int>{15});
    CHECK(numbersOf(s.hunks[1].newLines) == range(15, 19));
    CHECK(markedOf(s.hunks[1].newLines) == std::vector<int>{17});
    CHECK(s.hunks[1].newLines[2].text == "line 15 fixed");
    CHECK(s.hunks[1].oldLines[2].text == "line 15");
}

TEST_CASE("snippet: deleted lines shift the numbers the other way") {
    const std::string before = numbered(20);
    std::vector<std::string> lines = diff_split_lines(before);
    lines.erase(lines.begin() + 4, lines.begin() + 6);   // delete lines 5 and 6
    lines[9] = "line 12 fixed";                           // old line 12 is new line 10
    std::string after;
    for (const auto& l : lines) after += l + "\n";

    const Snippet s = FixSnippet::build(before, after, {}, 1);
    REQUIRE(s.hunks.size() == 2);
    CHECK(markedOf(s.hunks[0].oldLines) == (std::vector<int>{5, 6}));
    CHECK(markedOf(s.hunks[0].newLines).empty());
    CHECK(numbersOf(s.hunks[0].newLines) == (std::vector<int>{4, 5}));
    CHECK(markedOf(s.hunks[1].oldLines) == std::vector<int>{12});
    CHECK(markedOf(s.hunks[1].newLines) == std::vector<int>{10});
    CHECK(numbersOf(s.hunks[1].oldLines) == range(11, 13));
    CHECK(numbersOf(s.hunks[1].newLines) == range(9, 11));
}

TEST_CASE("snippet: reported error lines are marked in the old file even when unchanged") {
    const std::string before = numbered(30);
    const std::string after = replaceLine(before, 3, "three");

    // line 25 is reported by the tool but the fix changed line 3: own hunk, marked only in old
    const Snippet s = FixSnippet::build(before, after, {25, 4, 0, -1, 99}, 2);
    REQUIRE(s.hunks.size() == 2);
    CHECK(markedOf(s.hunks[0].oldLines) == (std::vector<int>{3, 4}));     // changed + reported
    CHECK(markedOf(s.hunks[0].newLines) == std::vector<int>{3});          // only the change
    CHECK(markedOf(s.hunks[1].oldLines) == std::vector<int>{25});
    CHECK(numbersOf(s.hunks[1].oldLines) == range(23, 27));
    CHECK(markedOf(s.hunks[1].newLines).empty());

    // a pure insertion: the reported line is the one that is marked red
    const std::string inserted = "line 1\nline 2\nint y = 1;\nline 3\n";
    const Snippet ins = FixSnippet::build(numbered(3), inserted, {3}, 3);
    REQUIRE(ins.hunks.size() == 1);
    CHECK(markedOf(ins.hunks[0].oldLines) == std::vector<int>{3});
    CHECK(markedOf(ins.hunks[0].newLines) == std::vector<int>{3});
    CHECK(ins.hunks[0].newLines[2].text == "int y = 1;");
    CHECK(ins.hunks[0].newLines[3].line == 4);
}

TEST_CASE("snippet: no change, CRLF and empty input") {
    CHECK(FixSnippet::build(numbered(5), numbered(5), {}, 3).empty());
    CHECK(FixSnippet::build("a\r\nb\r\n", "a\nb\n", {}, 3).empty());   // line endings are ignored
    CHECK(FixSnippet::build("", "", {}, 3).empty());

    const Snippet created = FixSnippet::build("", "a\nb\n", {}, 3);
    REQUIRE(created.hunks.size() == 1);
    CHECK(created.hunks[0].oldLines.empty());
    CHECK(markedOf(created.hunks[0].newLines) == (std::vector<int>{1, 2}));

    // unchanged file, but an error line: shown with context
    const Snippet only = FixSnippet::build(numbered(10), numbered(10), {5}, 1);
    REQUIRE(only.hunks.size() == 1);
    CHECK(numbersOf(only.hunks[0].oldLines) == range(4, 6));
}

TEST_CASE("snippet: limits for hunks and lines") {
    const std::string before = numbered(100);
    std::string after = before;
    for (int line : {5, 30, 60, 90}) after = replaceLine(after, line, "x");

    const Snippet s = FixSnippet::build(before, after, {}, 1, 2);
    CHECK(s.hunks.size() == 2);
    CHECK(s.omittedHunks == 2);

    // a whole-file rewrite: every line changes, the hunk is cut
    std::string rewritten;
    for (int i = 1; i <= 100; ++i) rewritten += "other " + std::to_string(i) + "\n";
    const Snippet big = FixSnippet::build(before, rewritten, {}, 3, 8, 10);
    REQUIRE(big.hunks.size() == 1);
    CHECK(big.hunks[0].oldLines.size() == 10);
    CHECK(big.hunks[0].newLines.size() == 10);
    CHECK(big.hunks[0].truncated);
}

TEST_CASE("snippet: excerpt of a file without a fix") {
    const std::string text = numbered(40);
    const Snippet s = FixSnippet::excerpt(text, {1, 20, 22, 40}, 2);
    REQUIRE(s.hunks.size() == 3);
    CHECK(numbersOf(s.hunks[0].oldLines) == range(1, 3));
    CHECK(numbersOf(s.hunks[1].oldLines) == range(18, 24));
    CHECK(markedOf(s.hunks[1].oldLines) == (std::vector<int>{20, 22}));
    CHECK(numbersOf(s.hunks[2].oldLines) == range(38, 40));
    CHECK(s.hunks[0].newLines.empty());

    CHECK(FixSnippet::excerpt(text, {}, 3).empty());
    CHECK(FixSnippet::excerpt(text, {0, 41, -5}, 3).empty());   // outside the file
    CHECK(FixSnippet::excerpt("", {1}, 3).empty());
}

TEST_CASE("snippet: changed lines of the old file") {
    const std::string before = numbered(10);
    std::string after = replaceLine(before, 2, "two");
    after = replaceLine(after, 7, "seven");
    CHECK(FixSnippet::changedOldLines(before, after) == (std::vector<int>{2, 7}));
    CHECK(FixSnippet::changedOldLines(before, before).empty());
    CHECK(FixSnippet::changedOldLines("a\nb\n", "a\nX\nb\n").empty());   // pure insertion
}

TEST_CASE("snippet: JSON round trip and tolerant reading") {
    const std::string before = numbered(20);
    const Snippet s = FixSnippet::build(before, replaceLine(before, 10, "ten"), {12}, 2, 8, 4);
    const Snippet back = FixSnippet::fromJson(FixSnippet::toJson(s));
    REQUIRE(back.hunks.size() == s.hunks.size());
    CHECK(numbersOf(back.hunks[0].oldLines) == numbersOf(s.hunks[0].oldLines));
    CHECK(markedOf(back.hunks[0].oldLines) == markedOf(s.hunks[0].oldLines));
    CHECK(back.hunks[0].newLines[2].text == "ten");
    CHECK(back.hunks[0].truncated == s.hunks[0].truncated);

    CHECK(FixSnippet::fromJson(nullptr).empty());
    CHECK(FixSnippet::fromJson("text").empty());
    CHECK(FixSnippet::fromJson({{"hunks", "nope"}}).empty());
    const Snippet broken = FixSnippet::fromJson(nlohmann::json::parse(R"({
        "hunks": [ 5, {"old": [{"line": "x"}, {"line": 3, "text": 7, "mark": "yes"}]}, {"old": [], "new": []} ],
        "omitted_hunks": -4 })"));
    REQUIRE(broken.hunks.size() == 1);
    REQUIRE(broken.hunks[0].oldLines.size() == 1);
    CHECK(broken.hunks[0].oldLines[0].line == 3);
    CHECK(broken.hunks[0].oldLines[0].text.empty());
    CHECK(!broken.hunks[0].oldLines[0].mark);
    CHECK(broken.omittedHunks == 0);
}

TEST_CASE("unified diff: unchanged output after exposing the line diff") {
    const std::string diff = unified_diff("a\nb\nc\n", "a\nB\nc\n", "f.txt");
    CHECK(diff == "--- a/f.txt\n+++ b/f.txt\n@@ -1,3 +1,3 @@\n a\n-b\n+B\n c\n");
    CHECK(unified_diff("a\n", "a\n", "f.txt").empty());

    const std::vector<DiffOp> ops = diff_lines({"a", "b"}, {"a", "x", "b"});
    REQUIRE(ops.size() == 3);
    CHECK(ops[1].type == '+');
    CHECK(ops[1].b == 1);
    CHECK(ops[2].type == ' ');
    CHECK(ops[2].a == 1);
    CHECK(ops[2].b == 2);
}
