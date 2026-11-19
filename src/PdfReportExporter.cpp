#include "PdfReportExporter.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <unistd.h>

namespace fs = std::filesystem;
using ojson = nlohmann::ordered_json;

namespace {

// ---------------------------------------------------------------------------
// Page geometry (A4, PDF points) and text metrics
// ---------------------------------------------------------------------------
// Everything is set in Courier: every character is 0.6 * font size wide, so line
// breaks and right alignment can be computed without font metric tables.
constexpr double PAGE_W = 595.0, PAGE_H = 842.0;
constexpr double MARGIN_X = 40.0;
constexpr double FONT_SIZE = 8.0;
constexpr double CHAR_W = FONT_SIZE * 0.6;
constexpr double LINE_H = 10.0;
constexpr double TOP_Y = 780.0;       // baseline of the first text row
constexpr double BOTTOM_Y = 50.0;     // lowest allowed baseline
constexpr double HEADER_Y = 812.0;
constexpr double FOOTER_Y = 30.0;
constexpr double BIG_FONT_SIZE = 12.0;   // titles of the fix report

const size_t MAX_COLS = static_cast<size_t>((PAGE_W - 2 * MARGIN_X) / CHAR_W);                 // 107
const size_t MAX_BIG_COLS = static_cast<size_t>((PAGE_W - 2 * MARGIN_X) / (BIG_FONT_SIZE * 0.6));   // 71
const size_t ROWS_PER_PAGE = static_cast<size_t>((TOP_Y - BOTTOM_Y) / LINE_H) + 1;             // 74

// Upper bound per document (about 3400 pages). A larger file ends with a notice.
constexpr size_t MAX_ROWS = 250000;
constexpr int MAX_DEPTH = 200;        // deeper JSON is printed compactly instead of recursing

// A "keep together" group (heading + short code block) with at most this many rows is moved
// to the next page instead of being split.
constexpr size_t MAX_KEEP_ROWS = 50;

enum Style : std::uint8_t {
    S_TEXT, S_KEY, S_STRING, S_NUMBER, S_LITERAL, S_PUNCT, S_GUTTER, S_WARN, S_HEADING,
    S_RED, S_GREEN, S_LABEL, S_OK, S_MUTED
};

struct StyleDef {
    double r, g, b;
    bool bold;
};

const StyleDef& styleDef(Style s) {
    static const StyleDef defs[] = {
        {0.00, 0.00, 0.00, false},   // S_TEXT
        {0.10, 0.30, 0.65, false},   // S_KEY      blue
        {0.05, 0.45, 0.15, false},   // S_STRING   green
        {0.75, 0.35, 0.00, false},   // S_NUMBER   orange
        {0.55, 0.15, 0.60, false},   // S_LITERAL  purple (true / false / null)
        {0.35, 0.35, 0.35, false},   // S_PUNCT
        {0.50, 0.50, 0.50, false},   // S_GUTTER   line numbers
        {0.75, 0.10, 0.10, true},    // S_WARN
        {0.10, 0.10, 0.10, true},    // S_HEADING
        {0.65, 0.05, 0.05, false},   // S_RED      removed lines in a diff
        {0.05, 0.42, 0.10, false},   // S_GREEN    added lines in a diff
        {0.25, 0.25, 0.30, true},    // S_LABEL    metadata labels
        {0.05, 0.45, 0.10, true},    // S_OK
        {0.45, 0.45, 0.45, false},   // S_MUTED    "no explanation" notes
    };
    return defs[s];
}

// Background of a row.
enum Fill : std::uint8_t { F_NONE, F_CODE, F_NOTE, F_BAD, F_GOOD, F_ALERT, F_SUCCESS, F_BAND };

const char* fillColor(Fill f) {
    switch (f) {
        case F_CODE:    return "0.94 0.94 0.94";   // code blocks
        case F_NOTE:    return "0.91 0.95 1.00";   // the "Fix recommendations" text field
        case F_BAD:     return "1.00 0.87 0.87";   // faulty / removed line
        case F_GOOD:    return "0.86 0.96 0.86";   // fixed / added line
        case F_ALERT:   return "0.97 0.78 0.78";   // "NOT FIXED" status bar
        case F_SUCCESS: return "0.78 0.92 0.78";   // "FIXED" status bar
        case F_BAND:    return "0.88 0.90 0.96";   // title band of a fix section
        default:        return nullptr;
    }
}

struct Span {
    std::string text;   // Windows-1252, one byte per character
    Style style;
};

struct Row {
    std::vector<Span> spans;
    Fill fill = F_NONE;
    bool big = false;     // title row (12 pt)
    int keep = 0;         // "keep together" group, 0 = none
    int minSpace = 0;     // start a new page when fewer rows than this are left
    int anchor = 0;       // target of internal links (fix section number)
    int link = 0;         // links to the anchor with this number
};

struct Section {
    std::string title;     // page header
    std::vector<Row> rows;
    bool truncated = false;
    bool flow = false;     // fix report: keep groups, no empty row at the top of a page
    int keep = 0;          // current keep group for added rows
    int nextKeep = 1;

    bool full() const { return rows.size() >= MAX_ROWS; }
    void add(Row row) {
        if (full()) { truncated = true; return; }
        if (row.keep == 0) row.keep = keep;
        rows.push_back(std::move(row));
    }
    void addText(const std::string& text, Style style, Fill fill = F_NONE) {
        add(Row{{Span{text, style}}, fill});
    }
    void addBlank() { add(Row{}); }
    void beginKeep() { keep = nextKeep++; }
    void endKeep() { keep = 0; }
};

// ---------------------------------------------------------------------------
// UTF-8 -> Windows-1252
// ---------------------------------------------------------------------------
// The standard PDF fonts only have the WinAnsi (cp1252) character set. Everything
// outside it becomes '?', except a few characters that tools print often.
void appendWinAnsi(std::string& out, std::uint32_t cp) {
    if (cp == '\t') { out += "    "; return; }
    if (cp < 0x20 || cp == 0x7F) { out += ' '; return; }
    if (cp < 0x7F || (cp >= 0xA0 && cp <= 0xFF)) { out += static_cast<char>(cp); return; }

    static const std::pair<std::uint32_t, unsigned char> cp1252[] = {
        {0x20AC, 0x80}, {0x201A, 0x82}, {0x0192, 0x83}, {0x201E, 0x84}, {0x2026, 0x85}, {0x2020, 0x86},
        {0x2021, 0x87}, {0x02C6, 0x88}, {0x2030, 0x89}, {0x0160, 0x8A}, {0x2039, 0x8B}, {0x0152, 0x8C},
        {0x017D, 0x8E}, {0x2018, 0x91}, {0x2019, 0x92}, {0x201C, 0x93}, {0x201D, 0x94}, {0x2022, 0x95},
        {0x2013, 0x96}, {0x2014, 0x97}, {0x02DC, 0x98}, {0x2122, 0x99}, {0x0161, 0x9A}, {0x203A, 0x9B},
        {0x0153, 0x9C}, {0x017E, 0x9E}, {0x0178, 0x9F},
    };
    for (const auto& [unicode, byte] : cp1252) {
        if (unicode == cp) { out += static_cast<char>(byte); return; }
    }

    // box drawing (the tool's own banners), arrows and check marks from test runners
    if (cp >= 0x2500 && cp <= 0x257F) {
        if (cp == 0x2500 || cp == 0x2501) out += '-';
        else if (cp == 0x2550) out += '=';
        else if (cp == 0x2502 || cp == 0x2503 || cp == 0x2551) out += '|';
        else out += '+';
        return;
    }
    if (cp == 0x2192 || cp == 0x276F) { out += '>'; return; }
    if (cp == 0x2713 || cp == 0x2714) { out += '+'; return; }
    if (cp == 0x2717 || cp == 0x2716 || cp == 0x00D7) { out += 'x'; return; }
    if (cp == 0x23AF) { out += '-'; return; }
    out += '?';
}

std::string toWinAnsi(const std::string& utf8) {
    std::string out;
    out.reserve(utf8.size());
    const size_t n = utf8.size();
    for (size_t i = 0; i < n;) {
        const unsigned char c = utf8[i];
        if (c < 0x80) { appendWinAnsi(out, c); ++i; continue; }

        // length of the sequence from the lead byte; invalid bytes become '?'
        int extra = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
        std::uint32_t cp = extra == 1 ? (c & 0x1F) : extra == 2 ? (c & 0x0F) : (c & 0x07);
        bool valid = extra > 0 && i + static_cast<size_t>(extra) < n;   // sequence complete?
        for (int k = 1; valid && k <= extra; ++k) {
            const unsigned char next = utf8[i + k];
            if ((next & 0xC0) != 0x80) valid = false;
            else cp = (cp << 6) | (next & 0x3F);
        }
        if (!valid) { out += '?'; ++i; continue; }
        appendWinAnsi(out, cp);
        i += static_cast<size_t>(extra) + 1;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Line wrapping
// ---------------------------------------------------------------------------
// Splits one logical line into rows of at most MAX_COLS characters. Nothing is cut off:
// the rest continues on the next row behind `continuation` (hanging indent or the empty
// line-number gutter of a code block). A break is moved back to the last space when one
// is close to the end of the row.
void wrapInto(Section& section, const std::vector<Span>& line, const std::vector<Span>& continuation, Fill fill) {
    size_t total = 0, contLen = 0;
    for (const auto& s : line) total += s.text.size();
    for (const auto& s : continuation) contLen += s.text.size();

    if (total <= MAX_COLS) {
        section.add(Row{line, fill});
        return;
    }

    // flatten to (character, style) so that a break can fall inside a span
    std::string chars;
    std::vector<Style> styles;
    chars.reserve(total);
    styles.reserve(total);
    for (const auto& s : line) {
        chars += s.text;
        styles.insert(styles.end(), s.text.size(), s.style);
    }

    size_t pos = 0;
    bool first = true;
    while (pos < total && !section.full()) {
        const size_t width = first ? MAX_COLS : MAX_COLS - contLen;
        size_t end = std::min(total, pos + width);
        if (end < total) {
            const size_t space = chars.rfind(' ', end - 1);
            if (space != std::string::npos && space > pos && end - space <= 30) end = space + 1;
        }

        Row row;
        row.fill = fill;
        if (!first) row.spans = continuation;
        for (size_t i = pos; i < end;) {
            size_t j = i;
            while (j < end && styles[j] == styles[i]) ++j;
            row.spans.push_back(Span{chars.substr(i, j - i), styles[i]});
            i = j;
        }
        section.add(std::move(row));
        pos = end;
        first = false;
    }
    if (pos < total) section.truncated = true;
}

// ---------------------------------------------------------------------------
// JSON -> coloured lines
// ---------------------------------------------------------------------------
std::string quoted(const std::string& text) {
    // JSON escaping by the library; invalid UTF-8 is replaced instead of throwing
    return ojson(text).dump(-1, ' ', false, ojson::error_handler_t::replace);
}

void writeValue(Section& section, const ojson& value, int indent, std::vector<Span> prefix,
                const char* suffix, int depth) {
    if (section.full()) { section.truncated = true; return; }

    // continuation rows of a wrapped line are indented 4 columns more than the line
    const std::vector<Span> hanging = {Span{std::string(static_cast<size_t>(std::min(indent + 4, 40)), ' '), S_TEXT}};
    auto emit = [&](const std::string& text, Style style) {
        prefix.push_back(Span{toWinAnsi(text), style});
        if (*suffix) prefix.push_back(Span{suffix, S_PUNCT});
        wrapInto(section, prefix, hanging, F_NONE);
    };

    const bool container = value.is_object() || value.is_array();
    if (!container || value.empty() || depth >= MAX_DEPTH) {
        if (value.is_string()) emit(quoted(value.get<std::string>()), S_STRING);
        else if (value.is_number()) emit(value.dump(), S_NUMBER);
        else if (value.is_boolean() || value.is_null()) emit(value.dump(), S_LITERAL);
        else emit(value.dump(-1, ' ', false, ojson::error_handler_t::replace), S_PUNCT);   // {} / [] / too deep
        return;
    }

    const bool object = value.is_object();
    prefix.push_back(Span{object ? "{" : "[", S_PUNCT});
    wrapInto(section, prefix, hanging, F_NONE);

    const std::string pad(static_cast<size_t>(indent + 2), ' ');
    size_t index = 0;
    const size_t count = value.size();
    for (auto it = value.begin(); it != value.end(); ++it, ++index) {
        std::vector<Span> childPrefix = {Span{pad, S_TEXT}};
        if (object) {
            childPrefix.push_back(Span{toWinAnsi(quoted(it.key())), S_KEY});
            childPrefix.push_back(Span{": ", S_PUNCT});
        }
        writeValue(section, *it, indent + 2, std::move(childPrefix), index + 1 < count ? "," : "", depth + 1);
        if (section.full()) return;
    }

    section.add(Row{{Span{std::string(static_cast<size_t>(indent), ' '), S_TEXT},
                     Span{std::string(object ? "}" : "]") + suffix, S_PUNCT}}, F_NONE});
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
        start = end + 1;
    }
    // a trailing newline does not start another line
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    return lines;
}

// Code block: every line with its number on a grey background. Wrapped rows keep the
// gutter but have no number, so the numbers always match the original text.
void writeNumberedLines(Section& section, const std::vector<std::string>& lines) {
    const std::vector<Span> continuation = {Span{"      | ", S_GUTTER}};
    char gutter[32];
    for (size_t i = 0; i < lines.size() && !section.full(); ++i) {
        std::snprintf(gutter, sizeof(gutter), "%5zu | ", i + 1);
        wrapInto(section, {Span{gutter, S_GUTTER}, Span{toWinAnsi(lines[i]), S_TEXT}}, continuation, F_CODE);
    }
    if (section.full()) section.truncated = true;
}

// Masks "key: value" / "key=value" lines in text that could not be parsed as JSON.
std::string maskRawLine(const std::string& line) {
    std::string lower = line;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });

    for (const char* word : {"api_key", "api-key", "apikey", "authorization", "password", "secret", "token"}) {
        const size_t pos = lower.find(word);
        if (pos == std::string::npos) continue;
        size_t i = pos + std::char_traits<char>::length(word);
        while (i < line.size() && (line[i] == '"' || line[i] == '\'' || line[i] == ' ')) ++i;
        if (i < line.size() && (line[i] == ':' || line[i] == '=')) return line.substr(0, i + 1) + " ***";
    }
    return PdfReportExporter::maskText(line);
}

// The fix recommendations as their own text field: a titled box with a light blue
// background. The text is the Markdown the model returned; it is shown as text (wrapped,
// without line numbers) with two light touches: "#" headings are bold and fenced code
// keeps the grey code background.
void writeRecommendations(Section& section, const std::string& path, const std::string& text,
                          const std::string& title = "Fix recommendations", const std::string& from = "from ",
                          const std::string& linesWord = "lines") {
    const size_t first = section.rows.size();
    const std::vector<std::string> lines = splitLines(text);

    section.addText(toWinAnsi(title), S_HEADING);
    section.addText(toWinAnsi(from + path) + "  (" + std::to_string(lines.size()) + " " + toWinAnsi(linesWord) + ")", S_GUTTER);
    section.addBlank();

    bool code = false;
    for (const auto& line : lines) {
        if (section.full()) break;
        const size_t rowsBefore = section.rows.size();
        const bool fence = line.rfind("```", 0) == 0;
        if (fence) {
            code = !code;                       // the fence lines themselves are not printed
            continue;
        }
        if (code) {
            wrapInto(section, {Span{"  " + toWinAnsi(line), S_TEXT}}, {Span{"  ", S_TEXT}}, F_CODE);
            continue;
        }
        const size_t hashes = line.find_first_not_of('#');
        if (hashes != std::string::npos && hashes > 0 && hashes <= 6 && line[hashes] == ' ') {
            // one empty row above a heading, unless the text already has one there
            if (rowsBefore > first + 3 && !section.rows.back().spans.empty()) section.addBlank();
            wrapInto(section, {Span{toWinAnsi(line.substr(hashes + 1)), S_HEADING}}, {}, F_NONE);
        } else if (line.find_first_not_of(" \t") == std::string::npos) {
            // several empty lines in a row are shown as one
            if (!section.rows.empty() && !section.rows.back().spans.empty()) section.addBlank();
        } else {
            wrapInto(section, {Span{toWinAnsi(line), S_TEXT}}, {}, F_NONE);
        }
    }
    while (section.rows.size() > first && section.rows.back().spans.empty()) section.rows.pop_back();
    section.addBlank();

    for (size_t i = first; i < section.rows.size(); ++i) {
        if (section.rows[i].fill != F_CODE) section.rows[i].fill = F_NOTE;
    }
    section.addBlank();
}

// The JSON (already masked, fix recommendations already replaced by a reference) and its
// multi-line strings as numbered blocks.
void writeJsonDump(Section& section, const ojson& masked) {
    std::vector<PdfReportExporter::TextBlock> blocks;
    const ojson shown = PdfReportExporter::extractBlocks(masked, blocks);
    writeValue(section, shown, 0, {}, "", 0);

    for (const auto& block : blocks) {
        if (section.full()) break;
        section.addBlank();
        wrapInto(section, {Span{toWinAnsi("Block #" + std::to_string(block.number) + "  " + block.path
                                          + "  (" + std::to_string(block.lineCount) + " lines)"), S_HEADING}}, {}, F_NONE);
        writeNumberedLines(section, splitLines(block.text));
    }
}

// Moves the fix recommendations (ai_analysis, ...) out of the JSON: writes each as a text
// field and leaves a reference in the JSON. Returns true when there was one.
bool writeRecommendationFields(Section& section, ojson& masked, const std::string& title, const std::string& from,
                               const std::string& linesWord = "lines", int minSpace = 0) {
    bool recommendations = false;
    if (!masked.is_object()) return false;
    for (auto it = masked.begin(); it != masked.end(); ++it) {
        if (!PdfReportExporter::isRecommendationKey(it.key()) || !it.value().is_string()) continue;
        const std::string text = it.value().get<std::string>();
        const size_t lineCount = splitLines(text).size();
        if (lineCount == 0) continue;
        if (minSpace > 0) {
            section.addBlank();
            Row spacer;
            spacer.minSpace = minSpace;
            section.add(std::move(spacer));
        }
        writeRecommendations(section, "$." + it.key(), text, title, from, linesWord);
        it.value() = "[shown above as " + title + ": " + std::to_string(lineCount) + " lines]";
        recommendations = true;
    }
    return recommendations;
}

// ===========================================================================
// Fix report layout (documents with "fix_report", see docs/PLAN_REPORT_REDESIGN.md)
// ===========================================================================

// Fixed texts of the report in German (default) or English.
struct Tr {
    bool de = true;
    std::string operator()(const char* german, const char* english) const { return de ? german : english; }
};

constexpr size_t LABEL_W = 15;   // width of the label column of the metadata block

std::string str(const ojson& j, const char* key) {
    if (!j.is_object() || !j.contains(key)) return "";
    const ojson& v = j[key];
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    return "";
}

long long num(const ojson& j, const char* key, long long fallback = 0) {
    if (!j.is_object() || !j.contains(key) || !j[key].is_number()) return fallback;
    return j[key].get<long long>();
}

bool flag(const ojson& j, const char* key) {
    return j.is_object() && j.contains(key) && j[key].is_boolean() && j[key].get<bool>();
}

const ojson& sub(const ojson& j, const char* key) {
    static const ojson empty = ojson::object();
    return (j.is_object() && j.contains(key)) ? j[key] : empty;
}

std::vector<std::string> strings(const ojson& j, const char* key) {
    std::vector<std::string> out;
    const ojson& v = sub(j, key);
    if (!v.is_array()) return out;
    for (const auto& e : v) {
        if (e.is_string()) out.push_back(e.get<std::string>());
    }
    return out;
}

std::string joinLines(const ojson& j, size_t max = 6) {
    const ojson& v = sub(j, "lines");
    std::string out;
    size_t n = 0;
    if (!v.is_array()) return out;
    for (const auto& e : v) {
        if (!e.is_number_integer()) continue;
        if (n == max) { out += ", ..."; break; }
        out += (n ? ", " : "") + std::to_string(e.get<long long>());
        ++n;
    }
    return out;
}

std::string clipText(const std::string& text, size_t width) {
    if (text.size() <= width) return text;
    return width > 3 ? text.substr(0, width - 3) + "..." : text.substr(0, width);
}

std::string padRight(const std::string& text, size_t width) {
    std::string t = clipText(text, width);
    return t + std::string(width - t.size(), ' ');
}

// Big title rows (12 pt bold), wrapped at word boundaries.
void addTitle(Section& section, const std::string& utf8, Fill fill, int anchor = 0) {
    const std::string text = toWinAnsi(utf8);
    size_t pos = 0;
    bool first = true;
    while (pos < text.size() || first) {
        size_t end = std::min(text.size(), pos + MAX_BIG_COLS);
        if (end < text.size()) {
            const size_t space = text.rfind(' ', end - 1);
            if (space != std::string::npos && space > pos) end = space + 1;
        }
        Row row{{Span{text.substr(pos, end - pos), S_HEADING}}, fill};
        row.big = true;
        if (first) row.anchor = anchor;
        section.add(std::move(row));
        pos = end;
        first = false;
    }
}

// Free text (explanations), one paragraph per line, wrapped with a small indent.
void addParagraphs(Section& section, const std::string& text, Style style, Fill fill = F_NONE) {
    for (const auto& line : splitLines(text)) {
        if (line.find_first_not_of(" \t") == std::string::npos) {
            if (!section.rows.empty() && !section.rows.back().spans.empty()) section.addBlank();
            continue;
        }
        wrapInto(section, {Span{"  " + toWinAnsi(line), style}}, {Span{"  ", style}}, fill);
    }
}

void addHeading(Section& section, const std::string& utf8, const std::string& note = "") {
    std::vector<Span> spans = {Span{toWinAnsi(utf8), S_HEADING}};
    if (!note.empty()) spans.push_back(Span{"  " + toWinAnsi(note), S_GUTTER});
    wrapInto(section, spans, {Span{"  ", S_TEXT}}, F_NONE);
}

void addField(Section& section, const std::string& label, const std::string& value, Style style = S_TEXT) {
    const std::string shown = value.empty() ? "-" : value;
    wrapInto(section, {Span{toWinAnsi(padRight(label + ":", LABEL_W)), S_LABEL}, Span{toWinAnsi(shown), style}},
             {Span{std::string(LABEL_W, ' '), S_TEXT}}, F_NONE);
}

// One side of the code excerpts: every line with its real line number; marked lines get a
// coloured background and a marker ('>' faulty, '+' fixed) so they stay visible in grey print.
void addCodeLines(Section& section, const ojson& lines, char marker, Fill markFill) {
    const std::vector<Span> continuation = {Span{"        ", S_GUTTER}};
    char gutter[32];
    for (const auto& l : lines) {
        if (section.full()) return;
        if (!l.is_object()) continue;
        const bool mark = flag(l, "mark");
        std::snprintf(gutter, sizeof(gutter), "%5lld %c ", num(l, "line"), mark ? marker : '|');
        wrapInto(section, {Span{gutter, mark ? S_LABEL : S_GUTTER}, Span{toWinAnsi(str(l, "text")), S_TEXT}},
                 continuation, mark ? markFill : F_CODE);
    }
}

// The hunks of a snippet, one side ("old" / "new"). Returns false when there is nothing to show.
bool addSnippet(Section& section, const ojson& snippet, const char* side, char marker, Fill markFill, const Tr& tr) {
    const ojson& hunks = sub(snippet, "hunks");
    if (!hunks.is_array()) return false;
    bool any = false;
    for (const auto& h : hunks) {
        const ojson& lines = sub(h, side);
        if (!lines.is_array() || lines.empty()) continue;
        if (any) section.addText("      " + std::string(20, '.'), S_GUTTER, F_NONE);   // gap between hunks
        addCodeLines(section, lines, marker, markFill);
        if (flag(h, "truncated")) section.addText(toWinAnsi("      ... " + tr("(gekürzt, siehe Diff)", "(shortened, see the diff)")), S_MUTED);
        any = true;
    }
    if (any && num(snippet, "omitted_hunks") > 0) {
        section.addText(toWinAnsi("      ... " + std::to_string(num(snippet, "omitted_hunks"))
                                  + tr(" weitere Stelle(n), siehe Diff", " more place(s), see the diff")), S_MUTED);
    }
    return any;
}

void addDiff(Section& section, const std::string& diff) {
    const std::vector<Span> continuation = {Span{"  ", S_TEXT}};
    for (const auto& line : splitLines(diff)) {
        if (section.full()) return;
        Style style = S_TEXT;
        Fill fill = F_CODE;
        if (line.rfind("+++", 0) == 0 || line.rfind("---", 0) == 0) style = S_GUTTER;
        else if (line.rfind("@@", 0) == 0) style = S_KEY;
        else if (!line.empty() && line[0] == '-') { style = S_RED; fill = F_BAD; }
        else if (!line.empty() && line[0] == '+') { style = S_GREEN; fill = F_GOOD; }
        wrapInto(section, {Span{toWinAnsi(line), style}}, continuation, fill);
    }
}

std::string statusText(const ojson& fix, const Tr& tr) {
    if (str(fix, "status") == "fixed") {
        const long long by = num(fix, "resolved_by");
        return by > 0 && sub(fix, "attempts").empty() ? tr("behoben (mit Nr. ", "fixed (with no. ") + std::to_string(by) + ")"
                                                      : tr("behoben", "fixed");
    }
    return tr("NICHT BEHOBEN", "NOT FIXED");
}

// The attempt that is shown in detail: the one that fixed the error, else the last one
// with a code proposal, else the last one.
const ojson* shownAttempt(const ojson& fix) {
    const ojson& attempts = sub(fix, "attempts");
    if (!attempts.is_array() || attempts.empty()) return nullptr;
    for (const auto& a : attempts) {
        if (flag(a, "applied") && flag(a, "error_gone")) return &a;
    }
    for (auto it = attempts.rbegin(); it != attempts.rend(); ++it) {
        if (!sub(sub(*it, "snippet"), "hunks").empty()) return &*it;
    }
    return &attempts.back();
}

std::string outcomeText(const std::string& outcome, const Tr& tr) {
    if (outcome == "fixed") return tr("behoben", "fixed");
    if (outcome == "still_present") return tr("angewandt, Fehler weiterhin vorhanden", "applied, error still present");
    if (outcome == "rejected") return tr("abgelehnt, nicht angewandt", "rejected, not applied");
    if (outcome == "apply_failed") return tr("konnte nicht geschrieben werden", "could not be written");
    return tr("kein Fix geliefert", "no fix returned");
}

void writeResult(Section& section, const ojson& fix, const ojson* attempt, const Tr& tr) {
    section.beginKeep();
    addHeading(section, tr("ERGEBNIS", "RESULT"));
    const ojson& a = attempt ? *attempt : ojson::object();
    const ojson& review = sub(a, "review");
    const ojson& verification = sub(a, "verification");
    const bool pipeline = flag(a, "agent_pipeline");

    if (!attempt) {
        addField(section, "Reviewer", tr("nicht ausgeführt (kein Fix-Versuch)", "not run (no fix attempt)"), S_MUTED);
        addField(section, "Verifier", tr("nicht ausgeführt (kein Fix-Versuch)", "not run (no fix attempt)"), S_MUTED);
    } else {
        if (flag(review, "ran")) {
            std::string text = flag(review, "approved") ? tr("angenommen", "approved")
                                                        : tr("abgelehnt (Änderungen gefordert)", "rejected (changes requested)");
            if (flag(review, "revised")) text += tr(" nach einer Überarbeitung", " after one revision");
            if (!str(review, "comment").empty()) text += " - " + str(review, "comment");
            addField(section, "Reviewer", text, flag(review, "approved") ? S_TEXT : S_RED);
            for (const auto& f : strings(review, "findings")) {
                wrapInto(section, {Span{std::string(LABEL_W, ' ') + "- " + toWinAnsi(f), S_TEXT}},
                         {Span{std::string(LABEL_W + 2, ' '), S_TEXT}}, F_NONE);
            }
        } else {
            addField(section, "Reviewer", pipeline ? tr("keine gültige Antwort", "no valid answer")
                                                   : tr("nicht ausgeführt (Einzelmodell-Modus)", "not run (single model mode)"), S_MUTED);
        }
        if (flag(verification, "ran")) {
            std::string text = flag(verification, "confirmed") ? tr("bestätigt", "confirmed") : tr("abgelehnt", "rejected");
            if (!str(verification, "reason").empty()) text += " - " + str(verification, "reason");
            addField(section, "Verifier", text, flag(verification, "confirmed") ? S_TEXT : S_RED);
            for (const auto& issue : strings(verification, "remaining_issues")) {
                wrapInto(section, {Span{std::string(LABEL_W, ' ') + "- " + toWinAnsi(issue), S_TEXT}},
                         {Span{std::string(LABEL_W + 2, ' '), S_TEXT}}, F_NONE);
            }
        } else {
            addField(section, "Verifier", pipeline ? tr("nicht erreicht", "not reached")
                                                   : tr("nicht ausgeführt (Einzelmodell-Modus)", "not run (single model mode)"), S_MUTED);
        }

        // the real re-run of the tool (build, checks, tests)
        std::string rerun;
        Style rerunStyle = S_TEXT;
        if (!flag(a, "applied")) {
            rerun = tr("nicht ausgeführt (Fix nicht angewandt)", "not run (fix not applied)");
            rerunStyle = S_MUTED;
        } else if (flag(a, "error_gone")) {
            const auto failed = strings(a, "failed_checks");
            const auto remaining = strings(a, "remaining_errors");
            rerun = tr("Fehler behoben", "error fixed");
            if (failed.empty() && remaining.empty()) rerun += tr(", alle Prüfungen/Tests bestanden", ", all checks/tests passed");
            else rerun += tr(", andere Fehler offen: ", ", other errors open: ") + std::to_string(remaining.size());
            rerunStyle = S_OK;
        } else {
            rerun = tr("Fehler weiterhin vorhanden", "error still present");
            rerunStyle = S_RED;
        }
        addField(section, tr("Re-Run", "Re-run"), rerun, rerunStyle);
        const auto failed = strings(a, "failed_checks");
        if (flag(a, "applied") && !failed.empty()) {
            std::string list;
            for (size_t i = 0; i < failed.size() && i < 8; ++i) list += (i ? ", " : "") + failed[i];
            if (failed.size() > 8) list += ", ...";
            addField(section, tr("Fehlgeschl.", "Failed checks"), list, S_RED);
        }
    }
    addField(section, tr("Iterationen", "Iterations"), std::to_string(num(fix, "iterations")));
    section.endKeep();
}

void writeAttemptList(Section& section, const ojson& fix, const Tr& tr) {
    const ojson& attempts = sub(fix, "attempts");
    if (!attempts.is_array() || attempts.size() < 2) return;
    section.addBlank();
    section.beginKeep();
    addHeading(section, tr("VERSUCHE", "ATTEMPTS"));
    for (const auto& a : attempts) {
        std::string text = "#" + std::to_string(num(a, "number")) + "  " + outcomeText(str(a, "outcome"), tr);
        if (!str(a, "reason").empty() && str(a, "outcome") != "fixed") text += " - " + str(a, "reason");
        wrapInto(section, {Span{"  " + toWinAnsi(text), str(a, "outcome") == "fixed" ? S_OK : S_TEXT}},
                 {Span{"      ", S_TEXT}}, F_NONE);
    }
    section.endKeep();
}

void writeFixSection(Section& section, const ojson& fix, bool showDiff, const Tr& tr) {
    const int id = static_cast<int>(num(fix, "id"));
    const bool fixed = str(fix, "status") == "fixed";
    const ojson* attempt = shownAttempt(fix);
    const ojson& explanation = attempt ? sub(*attempt, "explanation") : ojson::object();
    const std::string noExplanation = tr("Keine Erklärung vom Modell geliefert.", "No explanation provided by the model.");

    // title band and status, kept together with the metadata
    section.addBlank();
    section.beginKeep();
    Row spacer;
    spacer.minSpace = static_cast<int>(ROWS_PER_PAGE / 4);   // a section does not start at the very bottom
    section.add(std::move(spacer));
    addTitle(section, std::to_string(id) + ". " + (str(fix, "title").empty() ? str(fix, "error_name") : str(fix, "title")),
             F_BAND, id);

    std::string status = fixed ? tr("BEHOBEN", "FIXED") : tr("NICHT BEHOBEN", "NOT FIXED");
    const long long iterations = num(fix, "iterations");
    if (iterations > 0) status += tr(" - Iterationen: ", " - iterations: ") + std::to_string(iterations);
    section.add(Row{{Span{toWinAnsi("  " + status), fixed ? S_OK : S_WARN}}, fixed ? F_SUCCESS : F_ALERT});
    if (!str(fix, "status_reason").empty()) {
        wrapInto(section, {Span{toWinAnsi("  " + tr("Grund: ", "Reason: ") + str(fix, "status_reason")), fixed ? S_TEXT : S_WARN}},
                 {Span{"  ", S_TEXT}}, fixed ? F_SUCCESS : F_ALERT);
    }
    section.addBlank();

    std::string severity = str(fix, "severity");
    if (!str(fix, "difficulty").empty()) severity += " (" + tr("Schwierigkeit: ", "difficulty: ") + str(fix, "difficulty") + ")";
    std::string category = str(fix, "category") + tr(" (Checkliste)", " (checklist)");
    if (!str(explanation, "error_category").empty() && str(explanation, "error_category") != str(fix, "category")) {
        category += tr("; laut Modell: ", "; model: ") + str(explanation, "error_category");
    }
    addField(section, tr("Datei", "File"), str(fix, "file"));
    addField(section, tr("Zeile(n)", "Line(s)"), joinLines(fix));
    addField(section, tr("Sprache", "Language"), str(fix, "language"));
    addField(section, tr("Tool/Quelle", "Tool/source"), str(fix, "tool"));
    addField(section, tr("Schweregrad", "Severity"), severity);
    addField(section, tr("Kategorie", "Category"), category);
    section.endKeep();

    // original error message
    if (!str(fix, "message").empty()) {
        section.addBlank();
        section.beginKeep();
        addHeading(section, tr("FEHLERMELDUNG", "ERROR MESSAGE"), tr("(gekürzt auf das Relevante)", "(relevant lines)"));
        for (const auto& line : splitLines(str(fix, "message"))) {
            wrapInto(section, {Span{"  " + toWinAnsi(line), S_TEXT}}, {Span{"    ", S_TEXT}}, F_CODE);
        }
        section.endKeep();
    }

    // fixed together with another fix: no code of its own
    if (!attempt) {
        if (!sub(sub(fix, "excerpt"), "hunks").empty()) {
            section.addBlank();
            section.beginKeep();
            addHeading(section, tr("FEHLERHAFTER CODE", "FAULTY CODE"), "(" + str(fix, "file") + ")");
            addSnippet(section, sub(fix, "excerpt"), "old", '>', F_BAD, tr);
            section.endKeep();
        }
        section.addBlank();
        writeResult(section, fix, nullptr, tr);
        return;
    }

    const std::string file = str(*attempt, "file").empty() ? str(fix, "file") : str(*attempt, "file");
    const ojson& snippet = sub(*attempt, "snippet");
    const bool applied = flag(*attempt, "applied") && flag(*attempt, "error_gone");

    // faulty code (from the fix, or the excerpt of the original file)
    section.addBlank();
    section.beginKeep();
    addHeading(section, tr("FEHLERHAFTER CODE", "FAULTY CODE"), "(" + file + tr(", vor dem Fix)", ", before the fix)"));
    if (!addSnippet(section, snippet, "old", '>', F_BAD, tr) && !addSnippet(section, sub(fix, "excerpt"), "old", '>', F_BAD, tr)) {
        section.addText(toWinAnsi("  " + tr("Kein Codeausschnitt verfügbar (Datei oder Zeile unbekannt).",
                                            "No code excerpt available (file or line unknown).")), S_MUTED);
    }
    section.endKeep();

    section.addBlank();
    section.beginKeep();
    addHeading(section, tr("WARUM DAS EIN FEHLER IST", "WHY THIS IS AN ERROR"));
    if (str(explanation, "root_cause_explanation").empty()) section.addText(toWinAnsi("  " + noExplanation), S_MUTED);
    else addParagraphs(section, str(explanation, "root_cause_explanation"), S_TEXT);
    section.endKeep();

    // fixed code / last proposal
    section.addBlank();
    section.beginKeep();
    addHeading(section, applied ? tr("GEFIXTER CODE", "FIXED CODE")
                                : tr("LETZTER VORSCHLAG (nicht übernommen)", "LAST PROPOSAL (not applied)"),
               "(" + file + tr(", nach dem Fix)", ", after the fix)"));
    if (!addSnippet(section, snippet, "new", '+', F_GOOD, tr)) {
        section.addText(toWinAnsi("  " + tr("Kein Code-Vorschlag vorhanden.", "No code proposal available.")), S_MUTED);
    }
    section.endKeep();

    if (showDiff && !str(*attempt, "diff").empty()) {
        section.addBlank();
        section.beginKeep();
        addHeading(section, "DIFF");
        addDiff(section, str(*attempt, "diff"));
        section.endKeep();
    }

    section.addBlank();
    section.beginKeep();
    addHeading(section, tr("ERKLÄRUNG DER BEHEBUNG", "HOW THE FIX WORKS"));
    if (str(explanation, "fix_explanation").empty()) section.addText(toWinAnsi("  " + noExplanation), S_MUTED);
    else addParagraphs(section, str(explanation, "fix_explanation"), S_TEXT);
    const std::string sideEffects = str(explanation, "side_effects");
    wrapInto(section, {Span{toWinAnsi("  " + tr("Mögliche Nebenwirkungen: ", "Possible side effects: ")), S_LABEL},
                       Span{toWinAnsi(sideEffects.empty() ? tr("keine angegeben", "none stated") : sideEffects),
                            sideEffects.empty() ? S_MUTED : S_TEXT}},
             {Span{"    ", S_TEXT}}, F_NONE);
    section.endKeep();

    section.addBlank();
    writeResult(section, fix, attempt, tr);
    writeAttemptList(section, fix, tr);
}

// Header block, summary table and one section per error. `masked` is the whole report.
void writeFixReport(Section& section, ojson& masked, const ojson& report, const std::string& runId,
                    const std::string& timestamp, const PdfExportConfig& layout) {
    const std::string lang = str(report, "language").empty() ? layout.language : str(report, "language");
    const Tr tr{lang != "en"};
    section.flow = true;

    // ---- header ----
    addTitle(section, tr("2buggAI Fehlerbericht", "2buggAI error report"), F_NONE);
    section.addBlank();
    const ojson& models = sub(report, "models");
    addField(section, tr("Projekt", "Project"), str(report, "project"));
    addField(section, tr("Ziel", "Target"), str(report, "target"));
    addField(section, "Run-ID", runId);
    addField(section, tr("Datum", "Date"), timestamp);
    if (!str(report, "fix_description").empty()) addField(section, tr("Auftrag", "Task"), str(report, "fix_description"));
    if (!str(report, "languages").empty()) addField(section, tr("Sprachen", "Languages"), str(report, "languages"));
    const std::string mode = str(models, "mode");
    addField(section, tr("Modus", "Mode"), mode == "pipeline" ? tr("Agent-Pipeline (Debugger -> Reviewer -> Verifier)",
                                                                  "agent pipeline (Debugger -> Reviewer -> Verifier)")
                                         : mode == "single" ? tr("Einzelmodell", "single model") : "");
    for (const auto& [key, label] : std::vector<std::pair<const char*, std::string>>{
             {"debugger", "Debugger"}, {"reviewer", "Reviewer"}, {"verifier", "Verifier"},
             {"fix", tr("Fix-Modell", "Fix model")}, {"report", tr("Analyse", "Analysis")}}) {
        if (!str(models, key).empty()) addField(section, label, str(models, key));
    }

    // ---- summary ----
    const ojson& fixes = sub(report, "fixes");
    const ojson& summary = sub(report, "summary");
    section.addBlank();
    section.beginKeep();
    addTitle(section, tr("Zusammenfassung", "Summary"), F_NONE);
    section.addBlank();
    section.add(Row{{Span{toWinAnsi(tr("  Gefunden: ", "  Found: ")) + std::to_string(num(summary, "found")) + "   ", S_LABEL},
                     Span{toWinAnsi(tr("Behoben: ", "Fixed: ")) + std::to_string(num(summary, "fixed")) + "   ", S_OK},
                     Span{toWinAnsi(tr("Nicht behoben: ", "Not fixed: ")) + std::to_string(num(summary, "not_fixed")),
                          num(summary, "not_fixed") > 0 ? S_WARN : S_LABEL}}});
    section.addBlank();

    if (!fixes.is_array() || fixes.empty()) {
        section.addText(toWinAnsi("  " + tr("Keine Fehler gefunden.", "No errors found.")), S_TEXT);
        section.endKeep();
    } else {
        const std::string header = " " + padRight(tr("Nr.", "No."), 4) + " | " + padRight(tr("Datei:Zeile", "File:line"), 32) + " | "
            + padRight(tr("Kategorie", "Category"), 26) + " | " + padRight("Status", 20) + " | " + tr("Abschnitt", "Section");
        section.addText(toWinAnsi(header), S_LABEL, F_CODE);
        for (const auto& fix : fixes) {
            const int id = static_cast<int>(num(fix, "id"));
            std::string location = str(fix, "file").empty() ? "-" : str(fix, "file");
            if (!joinLines(fix, 3).empty()) location += ":" + joinLines(fix, 3);
            const bool fixed = str(fix, "status") == "fixed";
            Row row;
            row.link = id;
            row.spans = {
                Span{toWinAnsi(" " + padRight(std::to_string(id), 4) + " | " + padRight(location, 32) + " | "
                               + padRight(str(fix, "category"), 26) + " | "), S_TEXT},
                Span{toWinAnsi(padRight(statusText(fix, tr), 20)), fixed ? S_OK : S_WARN},
                // "\x01\x01\x01" is replaced by the page number of the section after pagination
                Span{" | " + toWinAnsi(tr("S.", "p.")) + "\x01\x01\x01", S_KEY},
            };
            section.add(std::move(row));
        }
        section.endKeep();
        section.addBlank();
        section.addText(toWinAnsi(tr("  Die Zeilen der Tabelle sind Links auf den jeweiligen Abschnitt.",
                                     "  The rows of the table link to their section.")), S_MUTED);

        for (const auto& fix : fixes) {
            if (section.full()) break;
            if (fix.is_object()) writeFixSection(section, fix, layout.showDiff, tr);
        }
    }

    // ---- final analysis of the model (free text, after the run) ----
    writeRecommendationFields(section, masked, tr("Gesamtanalyse des Modells", "Final analysis of the model"),
                              tr("Freitext-Analyse nach dem Run, aus ", "free-text analysis after the run, from "),
                              tr("Zeilen", "lines"), static_cast<int>(ROWS_PER_PAGE / 4));

    // ---- appendix ----
    if (layout.includeRawJsonAppendix) {
        Row pageBreak;
        pageBreak.minSpace = static_cast<int>(ROWS_PER_PAGE) + 1;   // always on a new page
        section.add(std::move(pageBreak));
        addTitle(section, tr("Anhang: roher JSON-Report", "Appendix: raw JSON report"), F_NONE);
        section.addBlank();
        writeJsonDump(section, masked);
    }
}

// One document = the fix report (when the JSON has "fix_report"), otherwise the fix
// recommendations (if any), the JSON, and then its multi-line strings as code blocks.
Section buildSection(const PdfReportExporter::Document& doc, const std::string& runId, const std::string& timestamp,
                     const PdfExportConfig& layout) {
    Section section;
    section.title = doc.name;

    ojson parsed;
    std::string parseError;
    try {
        parsed = ojson::parse(doc.jsonText);
    } catch (const std::exception& e) {
        parseError = e.what();
    }

    if (!parseError.empty()) {
        // Invalid JSON: say so and show the file as it is.
        const bool empty = doc.jsonText.find_first_not_of(" \t\r\n") == std::string::npos;
        section.addText(empty ? "Invalid JSON: the file is empty." : "Invalid JSON: this file could not be parsed.", S_WARN);
        if (!empty) {
            wrapInto(section, {Span{toWinAnsi(parseError), S_WARN}}, {}, F_NONE);
            section.addBlank();
            section.addText("Raw content", S_HEADING);
            std::vector<std::string> lines = splitLines(doc.jsonText);
            for (auto& line : lines) line = maskRawLine(line);
            writeNumberedLines(section, lines);
        }
    } else {
        ojson masked = PdfReportExporter::maskSecrets(parsed);

        bool done = false;
        if (masked.is_object() && masked.contains("fix_report") && masked["fix_report"].is_object()) {
            try {
                const ojson report = masked["fix_report"];
                writeFixReport(section, masked, report, runId, timestamp, layout);
                done = true;
            } catch (const std::exception& e) {
                std::cerr << "[PDF] Fehlerbericht-Layout fehlgeschlagen (" << e.what() << ") - JSON wird gezeigt\n";
                section = Section{};
                section.title = doc.name;
                masked = PdfReportExporter::maskSecrets(parsed);
            }
        }
        if (!done) {
            // Fix recommendations first, in their own text field; the JSON keeps a reference.
            if (writeRecommendationFields(section, masked, "Fix recommendations", "from ")) {
                section.addText("JSON report", S_HEADING);
            }
            writeJsonDump(section, masked);
        }
    }

    if (section.truncated) {
        section.rows.resize(std::min(section.rows.size(), MAX_ROWS - 2));
        section.rows.push_back(Row{});
        section.rows.push_back(Row{{Span{"Report truncated: the file has more than " + std::to_string(MAX_ROWS)
                                         + " lines. The JSON file is complete.", S_WARN}}, F_NONE});
    }
    return section;
}

// ---------------------------------------------------------------------------
// PDF writer
// ---------------------------------------------------------------------------
// Minimal PDF 1.4: one uncompressed content stream per page, the standard fonts
// Courier (/F1) and Courier-Bold (/F2) with WinAnsi encoding, text and filled rectangles.

std::string num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    return buf;
}

// PDF literal string: escape the delimiters, write non-ASCII bytes as octal.
std::string pdfString(const std::string& text) {
    std::string out = "(";
    char buf[8];
    for (unsigned char c : text) {
        if (c == '(' || c == ')' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 0x20 || c > 0x7E) { std::snprintf(buf, sizeof(buf), "\\%03o", c); out += buf; }
        else out += static_cast<char>(c);
    }
    return out + ")";
}

void drawSpans(std::string& out, const std::vector<Span>& spans, double x, double y, double size = FONT_SIZE,
               bool forceBold = false) {
    if (spans.empty()) return;
    out += "BT\n";
    size_t col = 0;
    for (const auto& span : spans) {
        const StyleDef& def = styleDef(span.style);
        out += (def.bold || forceBold) ? "/F2 " : "/F1 ";
        out += num(size) + " Tf " + num(def.r) + " " + num(def.g) + " " + num(def.b) + " rg 1 0 0 1 "
             + num(x + static_cast<double>(col) * size * 0.6) + " " + num(y) + " Tm " + pdfString(span.text) + " Tj\n";
        col += span.text.size();
    }
    out += "ET\n";
}

std::string fit(const std::string& text, size_t maxChars) {
    return text.size() <= maxChars ? text : text.substr(0, maxChars - 3) + "...";
}

std::string pageContent(const Section& section, size_t begin, size_t end, size_t pageNumber, size_t pageCount,
                        const std::string& runId, const std::string& timestamp) {
    std::string out;

    // header: file name on the left, run id and time on the right, thin rule below
    const std::string right = toWinAnsi((runId.empty() ? "" : "Run " + runId + "   ") + timestamp);
    const std::string left = fit(toWinAnsi(section.title), MAX_COLS - right.size() - 2);
    drawSpans(out, {Span{left, S_HEADING}}, MARGIN_X, HEADER_Y);
    drawSpans(out, {Span{right, S_PUNCT}}, PAGE_W - MARGIN_X - static_cast<double>(right.size()) * CHAR_W, HEADER_Y);
    out += "0.60 G 0.50 w " + num(MARGIN_X) + " " + num(HEADER_Y - 6) + " m " + num(PAGE_W - MARGIN_X) + " "
         + num(HEADER_Y - 6) + " l S\n";

    double y = TOP_Y;
    for (size_t i = begin; i < end; ++i, y -= LINE_H) {
        const Row& row = section.rows[i];
        if (const char* color = fillColor(row.fill)) {
            // a title row is taller: its band also covers the row above
            const double height = row.big ? LINE_H * 1.6 : LINE_H;
            out += std::string(color) + " rg " + num(MARGIN_X - 3) + " " + num(y - 2.5) + " "
                 + num(PAGE_W - 2 * MARGIN_X + 6) + " " + num(height) + " re f\n";
        }
        if (row.big) drawSpans(out, row.spans, MARGIN_X, y, BIG_FONT_SIZE, true);
        else drawSpans(out, row.spans, MARGIN_X, y);
    }

    const std::string pageLabel = "Page " + std::to_string(pageNumber) + " of " + std::to_string(pageCount);
    drawSpans(out, {Span{"2buggAI report", S_PUNCT}}, MARGIN_X, FOOTER_Y);
    drawSpans(out, {Span{pageLabel, S_PUNCT}}, PAGE_W - MARGIN_X - static_cast<double>(pageLabel.size()) * CHAR_W, FOOTER_Y);
    return out;
}

std::string currentTimestamp(const char* format) {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    char buf[64];
    std::strftime(buf, sizeof(buf), format, &tm);
    return buf;
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

bool isKeyChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
}

void collectBlocks(ojson& value, const std::string& path, std::vector<PdfReportExporter::TextBlock>& blocks, int depth) {
    if (value.is_string()) {
        const std::string& text = value.get_ref<const std::string&>();
        const std::vector<std::string> lines = splitLines(text);
        if (lines.size() < 2) return;   // single line (possibly with a trailing newline): stays inline

        PdfReportExporter::TextBlock block;
        block.number = static_cast<int>(blocks.size()) + 1;
        block.path = path;
        block.text = text;
        block.lineCount = static_cast<int>(lines.size());
        value = PdfReportExporter::blockReference(block.number, text);
        blocks.push_back(std::move(block));
        return;
    }
    if (depth >= MAX_DEPTH) return;

    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            // $.a.b for simple keys, $['odd key'] otherwise
            const std::string& key = it.key();
            const bool simple = !key.empty() && !std::isdigit(static_cast<unsigned char>(key[0]))
                && std::all_of(key.begin(), key.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
            collectBlocks(it.value(), simple ? path + "." + key : path + "['" + key + "']", blocks, depth + 1);
        }
    } else if (value.is_array()) {
        for (size_t i = 0; i < value.size(); ++i) {
            collectBlocks(value[i], path + "[" + std::to_string(i) + "]", blocks, depth + 1);
        }
    }
}

void maskInPlace(ojson& value, int depth) {
    if (value.is_string()) {
        value = PdfReportExporter::maskCodeText(PdfReportExporter::maskText(value.get_ref<const std::string&>()));
    } else if (depth >= MAX_DEPTH) {
        return;
    } else if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (PdfReportExporter::isSensitiveKey(it.key())) {
                if (!it.value().is_null()) it.value() = "***";
            } else {
                maskInPlace(it.value(), depth + 1);
            }
        }
    } else if (value.is_array()) {
        for (auto& element : value) maskInPlace(element, depth + 1);
    }
}

// Names whose string value is a secret in source code (lower case).
const std::vector<std::string>& secretNames() {
    static const std::vector<std::string> names = {
        "api_key", "apikey", "api-key", "secret", "password", "passwd", "pwd", "token",
        "authorization", "private_key", "credential", "access_key",
    };
    return names;
}

// One line of code: string literals assigned to a secret-like name become "***".
std::string maskCodeLine(const std::string& line) {
    const std::string lower = toLower(line);
    std::string out = line;
    // replacements from the back, so earlier positions stay valid
    std::vector<std::pair<size_t, size_t>> literals;   // [start, end) of the literal content

    for (const auto& name : secretNames()) {
        for (size_t pos = lower.find(name); pos != std::string::npos; pos = lower.find(name, pos + 1)) {
            size_t i = pos + name.size();
            // the rest of the identifier (api_key_prod, password2) and closing quotes / brackets of a key
            while (i < line.size() && (std::isalnum(static_cast<unsigned char>(line[i])) || line[i] == '_' || line[i] == '-')) ++i;
            while (i < line.size() && (line[i] == '"' || line[i] == '\'' || line[i] == ']' || line[i] == ' ' || line[i] == '\t')) ++i;
            if (i >= line.size()) continue;
            if (line[i] == ':' && i + 1 < line.size() && line[i + 1] == '=') ++i;           // Go / Pascal :=
            else if (line[i] == '=' && i + 1 < line.size() && line[i + 1] == '=') continue;  // comparison
            else if (line[i] != ':' && line[i] != '=') continue;
            ++i;
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
            // optional prefixes of string literals: f"...", b'...', r"...", L"..."
            if (i + 1 < line.size() && std::isalpha(static_cast<unsigned char>(line[i]))
                && (line[i + 1] == '"' || line[i + 1] == '\'')) ++i;
            if (i >= line.size() || (line[i] != '"' && line[i] != '\'' && line[i] != '`')) continue;
            const char quote = line[i];
            size_t end = i + 1;
            while (end < line.size() && line[end] != quote) end += (line[end] == '\\') ? 2 : 1;
            end = std::min(end, line.size());
            if (end > i + 1) literals.emplace_back(i + 1, end);
        }
    }
    if (literals.empty()) return line;
    std::sort(literals.begin(), literals.end());
    literals.erase(std::unique(literals.begin(), literals.end()), literals.end());
    for (auto it = literals.rbegin(); it != literals.rend(); ++it) {
        const auto next = std::next(it);
        if (next != literals.rend() && next->second > it->first) continue;   // overlapping: masked by the outer one
        out.replace(it->first, it->second - it->first, "***");
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// building blocks
// ---------------------------------------------------------------------------

bool PdfReportExporter::isSensitiveKey(const std::string& key) {
    std::string k = toLower(key);
    std::replace(k.begin(), k.end(), '-', '_');

    for (const char* part : {"api_key", "apikey", "authorization", "password", "passwd", "secret",
                             "private_key", "credential"}) {
        if (k.find(part) != std::string::npos) return true;
    }
    // "token", "access_token", "api_token" - but not counters such as "prompt_tokens" / "max_tokens"
    return k == "token" || k == "auth" || k == "bearer"
        || (k.size() > 6 && k.compare(k.size() - 6, 6, "_token") == 0);
}

bool PdfReportExporter::isRecommendationKey(const std::string& key) {
    const std::string k = toLower(key);
    return k == "ai_analysis" || k == "fix_recommendations" || k == "recommendations";
}

// Hand-written scan instead of std::regex: reports can be several megabytes, and
// std::regex recurses per character on long inputs.
std::string PdfReportExporter::maskText(const std::string& text) {
    if (text.find("sk-") == std::string::npos && text.find("Bearer ") == std::string::npos) return text;

    std::string out;
    out.reserve(text.size());
    const size_t n = text.size();
    for (size_t i = 0; i < n;) {
        // OpenAI / Anthropic style keys: "sk-" + at least 8 key characters, not inside a word ("task-...")
        if (text.compare(i, 3, "sk-") == 0 && (i == 0 || !isKeyChar(text[i - 1]))) {
            size_t j = i + 3;
            while (j < n && isKeyChar(text[j])) ++j;
            if (j - (i + 3) >= 8) { out += "sk-***"; i = j; continue; }
        }
        // Authorization header values: "Bearer <token>"
        if (text.compare(i, 7, "Bearer ") == 0) {
            size_t j = i + 7;
            while (j < n && (isKeyChar(text[j]) || text[j] == '.' || text[j] == '~' || text[j] == '+'
                             || text[j] == '/' || text[j] == '=')) ++j;
            if (j - (i + 7) >= 8) { out += "Bearer ***"; i = j; continue; }
        }
        out += text[i++];
    }
    return out;
}

std::string PdfReportExporter::maskCodeText(const std::string& text) {
    // cheap pre-check: most strings contain none of the names
    const std::string lower = toLower(text);
    const bool candidate = std::any_of(secretNames().begin(), secretNames().end(),
                                       [&](const std::string& n) { return lower.find(n) != std::string::npos; });
    if (!candidate) return text;

    std::string out;
    out.reserve(text.size());
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        out += maskCodeLine(text.substr(start, end - start));
        if (end < text.size()) out += '\n';
        start = end + 1;
    }
    return out;
}

ojson PdfReportExporter::maskSecrets(const ojson& value) {
    ojson copy = value;
    maskInPlace(copy, 0);
    return copy;
}

std::string PdfReportExporter::blockReference(int number, const std::string& text) {
    const std::vector<std::string> lines = splitLines(text);

    // preview: start of the first line that has content
    std::string preview;
    for (const auto& line : lines) {
        const size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        preview = line.substr(start);
        break;
    }
    constexpr size_t MAX_PREVIEW = 60;
    if (preview.size() > MAX_PREVIEW) {
        // do not cut a UTF-8 character in half
        size_t cut = MAX_PREVIEW;
        while (cut > 0 && (static_cast<unsigned char>(preview[cut]) & 0xC0) == 0x80) --cut;
        preview.resize(cut);
    }

    return "[block #" + std::to_string(number) + ": " + std::to_string(lines.size()) + " lines] " + preview + "...";
}

ojson PdfReportExporter::extractBlocks(const ojson& value, std::vector<TextBlock>& blocks) {
    ojson copy = value;
    collectBlocks(copy, "$", blocks, 0);
    return copy;
}

std::string PdfReportExporter::render(const std::vector<Document>& documents, const std::string& runId,
                                      const std::string& timestamp, const std::string& coverTitle,
                                      const PdfExportConfig& layout) {
    std::vector<Section> sections;

    if (!coverTitle.empty()) {
        Section cover;
        cover.title = coverTitle;
        cover.addText(toWinAnsi(coverTitle), S_HEADING);
        cover.addBlank();
        if (!runId.empty()) cover.addText("Run ID:   " + toWinAnsi(runId), S_TEXT);
        cover.addText("Created:  " + toWinAnsi(timestamp), S_TEXT);
        cover.addBlank();
        cover.addText("Documents (" + std::to_string(documents.size()) + "), each starting on a new page:", S_TEXT);
        for (size_t i = 0; i < documents.size(); ++i) {
            wrapInto(cover, {Span{"  " + std::to_string(i + 1) + ". ", S_PUNCT}, Span{toWinAnsi(documents[i].name), S_KEY}},
                     {Span{"     ", S_TEXT}}, F_NONE);
        }
        sections.push_back(std::move(cover));
    }
    for (const auto& doc : documents) sections.push_back(buildSection(doc, runId, timestamp, layout));

    // Pagination: every section starts on a new page; an empty section still gets one page.
    // Sections of the fix report ("flow") also keep short groups together, honour minSpace
    // and do not start a continued page with an empty row.
    struct Page { size_t section, begin, end; };
    std::vector<Page> pages;
    std::map<std::pair<size_t, int>, size_t> anchorPage;   // (section, anchor) -> page index
    for (size_t s = 0; s < sections.size(); ++s) {
        const std::vector<Row>& rows = sections[s].rows;
        const size_t firstPage = pages.size();
        if (!sections[s].flow) {
            size_t begin = 0;
            do {
                const size_t end = std::min(rows.size(), begin + ROWS_PER_PAGE);
                pages.push_back({s, begin, end});
                begin = end;
            } while (begin < rows.size());
            continue;
        }

        size_t begin = 0, used = 0;
        for (size_t i = 0; i < rows.size(); ++i) {
            const Row& row = rows[i];
            size_t need = static_cast<size_t>(std::max(row.minSpace, 0));
            if (row.keep != 0 && (i == 0 || rows[i - 1].keep != row.keep)) {
                size_t length = 1;
                while (i + length < rows.size() && rows[i + length].keep == row.keep) ++length;
                if (length <= MAX_KEEP_ROWS) need = std::max(need, length);
            }
            if (used > 0 && (used >= ROWS_PER_PAGE || used + need > ROWS_PER_PAGE)) {
                pages.push_back({s, begin, i});
                begin = i;
                used = 0;
            }
            const bool blank = row.spans.empty() && row.fill == F_NONE;
            if (used == 0 && pages.size() > firstPage && blank) {   // no empty row at the top of a continued page
                begin = i + 1;
                continue;
            }
            if (row.anchor != 0) anchorPage[{s, row.anchor}] = pages.size();
            ++used;
        }
        if (used > 0 || pages.size() == firstPage) pages.push_back({s, begin, std::max(begin, rows.size())});
    }
    if (pages.empty()) {
        sections.emplace_back();
        sections.back().title = "empty report";
        pages.push_back({0, 0, 0});
    }

    // internal links: fill in the page numbers and collect one annotation per linked row
    struct Link { size_t page; double y; size_t target; };
    std::vector<Link> links;
    for (size_t p = 0; p < pages.size(); ++p) {
        Section& section = sections[pages[p].section];
        double y = TOP_Y;
        for (size_t i = pages[p].begin; i < pages[p].end; ++i, y -= LINE_H) {
            Row& row = section.rows[i];
            if (row.link == 0) continue;
            auto target = anchorPage.find({pages[p].section, row.link});
            char number[16];
            std::snprintf(number, sizeof(number), "%3zu", target == anchorPage.end() ? size_t{0} : target->second + 1);
            for (auto& span : row.spans) {
                const size_t at = span.text.find("\x01\x01\x01");
                if (at != std::string::npos) span.text.replace(at, 3, target == anchorPage.end() ? "  -" : number);
            }
            if (target != anchorPage.end()) links.push_back({p, y, target->second});
        }
    }

    // objects: 1 catalog, 2 page tree, 3/4 fonts, 5 info, then (page, content) pairs, then link annotations
    std::string pdf = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
    std::vector<size_t> offsets;
    auto addObject = [&](const std::string& body) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(offsets.size()) + " 0 obj\n" + body + "\nendobj\n";
    };
    auto pageObject = [](size_t page) { return std::to_string(6 + 2 * page) + " 0 R"; };

    std::string kids;
    for (size_t i = 0; i < pages.size(); ++i) kids += pageObject(i) + " ";

    addObject("<< /Type /Catalog /Pages 2 0 R >>");
    addObject("<< /Type /Pages /Count " + std::to_string(pages.size()) + " /Kids [ " + kids + "] >>");
    addObject("<< /Type /Font /Subtype /Type1 /BaseFont /Courier /Encoding /WinAnsiEncoding >>");
    addObject("<< /Type /Font /Subtype /Type1 /BaseFont /Courier-Bold /Encoding /WinAnsiEncoding >>");
    addObject("<< /Title " + pdfString(toWinAnsi(coverTitle.empty() && !documents.empty() ? documents.front().name : coverTitle))
              + " /Producer (2buggAI PdfReportExporter) >>");

    const size_t firstAnnot = 6 + 2 * pages.size();
    for (size_t i = 0; i < pages.size(); ++i) {
        std::string annots;
        for (size_t k = 0; k < links.size(); ++k) {
            if (links[k].page == i) annots += std::to_string(firstAnnot + k) + " 0 R ";
        }
        const std::string content = pageContent(sections[pages[i].section], pages[i].begin, pages[i].end,
                                                i + 1, pages.size(), runId, timestamp);
        addObject("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " + num(PAGE_W) + " " + num(PAGE_H) + "] "
                  "/Resources << /Font << /F1 3 0 R /F2 4 0 R >> >> /Contents " + std::to_string(7 + 2 * i) + " 0 R"
                  + (annots.empty() ? "" : " /Annots [ " + annots + "]") + " >>");
        addObject("<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "endstream");
    }
    for (const auto& link : links) {
        addObject("<< /Type /Annot /Subtype /Link /Border [0 0 0] /Rect [" + num(MARGIN_X - 3) + " " + num(link.y - 2.5)
                  + " " + num(PAGE_W - MARGIN_X + 3) + " " + num(link.y + 7.5) + "] /Dest [" + pageObject(link.target)
                  + " /Fit] >>");
    }

    // cross-reference table: one 20 byte entry per object, offsets from the file start
    const size_t xref = pdf.size();
    pdf += "xref\n0 " + std::to_string(offsets.size() + 1) + "\n0000000000 65535 f \n";
    char entry[32];
    for (size_t offset : offsets) {
        std::snprintf(entry, sizeof(entry), "%010zu 00000 n \n", offset);
        pdf += entry;
    }
    pdf += "trailer\n<< /Size " + std::to_string(offsets.size() + 1) + " /Root 1 0 R /Info 5 0 R >>\nstartxref\n"
         + std::to_string(xref) + "\n%%EOF\n";
    return pdf;
}

// ---------------------------------------------------------------------------
// configuration
// ---------------------------------------------------------------------------

PdfExportConfig PdfReportExporter::parseConfig(const std::string& buggaiJsonText, std::string* error) {
    PdfExportConfig config;
    const ojson j = ojson::parse(buggaiJsonText, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        if (error) *error = "not a JSON object";
        return config;
    }

    if (j.contains("report")) {
        const ojson& r = j["report"];
        if (!r.is_object()) {
            if (error) *error = "report must be an object";
        } else {
            if (r.contains("context_lines") && r["context_lines"].is_number_integer()) {
                config.contextLines = static_cast<int>(std::clamp<long long>(r["context_lines"].get<long long>(), 0, 20));
            }
            if (r.contains("show_diff") && r["show_diff"].is_boolean()) config.showDiff = r["show_diff"].get<bool>();
            if (r.contains("include_raw_json_appendix") && r["include_raw_json_appendix"].is_boolean()) {
                config.includeRawJsonAppendix = r["include_raw_json_appendix"].get<bool>();
            }
            if (r.contains("language") && r["language"].is_string()) {
                const std::string lang = toLower(r["language"].get<std::string>());
                if (lang == "de" || lang == "en") config.language = lang;
                else if (error) *error = "report.language must be \"de\" or \"en\"";
            }
        }
    }

    if (!j.contains("pdf_export")) return config;

    const ojson& p = j["pdf_export"];
    if (!p.is_object()) {
        if (error) *error = "pdf_export must be an object";
        return config;
    }
    auto flag = [&](const char* key, bool& target) {
        if (p.contains(key) && p[key].is_boolean()) target = p[key].get<bool>();
    };
    flag("enabled", config.enabled);
    flag("combined", config.combined);
    flag("per_iteration", config.perIteration);
    if (p.contains("output_dir") && p["output_dir"].is_string()) config.outputDir = p["output_dir"].get<std::string>();
    return config;
}

PdfExportConfig PdfReportExporter::loadConfig(const std::string& targetPath) {
    std::error_code ec;
    const fs::path dir = fs::is_directory(targetPath, ec) ? fs::path(targetPath) : fs::path(targetPath).parent_path();
    const fs::path file = dir / ".buggai.json";
    if (!fs::exists(file, ec)) return {};

    std::ifstream in(file);
    std::stringstream buffer;
    buffer << in.rdbuf();

    std::string error;
    PdfExportConfig config = parseConfig(buffer.str(), &error);
    if (!error.empty()) std::cerr << "[PDF] " << file.string() << ": " << error << " - using the defaults\n";
    if (!config.outputDir.empty() && fs::path(config.outputDir).is_relative()) {
        config.outputDir = (dir / config.outputDir).string();
    }
    return config;
}

// ---------------------------------------------------------------------------
// hooks
// ---------------------------------------------------------------------------

PdfReportExporter::PdfReportExporter() : PdfReportExporter(PdfExportConfig{}) {}

PdfReportExporter::PdfReportExporter(PdfExportConfig config)
    : config_(std::move(config)),
      // the tool has no run id of its own; start time + process id identifies the run
      runId_(currentTimestamp("%Y%m%d-%H%M%S") + "-" + std::to_string(getpid())),
      timestamp_(currentTimestamp("%Y-%m-%d %H:%M:%S")) {}

PdfReportExporter::~PdfReportExporter() {
    finish();   // covers every return path of a run
}

// Path of a PDF: folder = output_dir or the folder of the JSON, name = JSON file name
// without ".json" + suffix. Empty when there is neither a JSON file nor an output folder.
std::string PdfReportExporter::targetFor(const std::string& jsonPath, const std::string& fallbackName,
                                         const std::string& suffix, std::string& displayName) const {
    fs::path json(jsonPath);
    std::string base = jsonPath.empty() ? fallbackName : json.filename().string();
    displayName = base;
    if (fs::path(base).extension() == ".json") base = fs::path(base).stem().string();
    if (base.empty()) base = "report";

    fs::path dir;
    if (!config_.outputDir.empty()) dir = config_.outputDir;
    else if (!jsonPath.empty()) dir = json.parent_path();
    else return "";

    return (dir / (base + suffix)).string();
}

bool PdfReportExporter::write(const std::string& path, const std::string& pdf) {
    const fs::path target(path);
    if (!target.parent_path().empty()) fs::create_directories(target.parent_path());

    // write next to the target and rename: a half written PDF never replaces a good one
    const fs::path temp = target.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot write " + path);
        out.write(pdf.data(), static_cast<std::streamsize>(pdf.size()));
        out.flush();
        if (!out) {
            std::error_code ignored;
            fs::remove(temp, ignored);
            throw std::runtime_error("write failed for " + path);
        }
    }
    fs::rename(temp, target);

    if (std::find(written_.begin(), written_.end(), path) == written_.end()) written_.push_back(path);
    std::cout << "PDF Report gespeichert: " << path << "\n";
    return true;
}

bool PdfReportExporter::exportJson(const std::string& jsonText, const std::string& jsonPath,
                                   const std::string& fallbackName) noexcept {
    if (!config_.enabled) return false;
    try {
        std::string name;
        const std::string path = targetFor(jsonPath, fallbackName, ".pdf", name);
        if (path.empty()) return false;   // no JSON file and no output folder: nothing to do

        // remember the document for the combined report; a rewritten JSON replaces its entry
        auto existing = std::find_if(reports_.begin(), reports_.end(), [&](const Document& d) { return d.name == name; });
        if (existing != reports_.end()) existing->jsonText = jsonText;
        else reports_.push_back({name, jsonText});
        if (combinedPath_.empty()) combinedPath_ = targetFor(jsonPath, fallbackName, ".combined.pdf", name);

        return write(path, render({{name, jsonText}}, runId_, timestamp_, "", config_));
    } catch (const std::exception& e) {
        std::cerr << "[PDF] Export fehlgeschlagen (" << e.what() << ") - der Run läuft weiter\n";
    } catch (...) {
        std::cerr << "[PDF] Export fehlgeschlagen - der Run läuft weiter\n";
    }
    return false;
}

bool PdfReportExporter::exportIteration(int iteration, const ojson& data, const std::string& jsonPath,
                                        const std::string& fallbackName) noexcept {
    if (!config_.enabled || !config_.perIteration) return false;
    try {
        char suffix[32];
        std::snprintf(suffix, sizeof(suffix), ".iter%02d", iteration);

        std::string name;
        const std::string path = targetFor(jsonPath, fallbackName, std::string(suffix) + ".pdf", name);
        if (path.empty()) return false;
        if (combinedPath_.empty()) combinedPath_ = targetFor(jsonPath, fallbackName, ".combined.pdf", name);

        const Document doc{fs::path(path).stem().string(),
                           data.dump(2, ' ', false, ojson::error_handler_t::replace)};
        iterations_.push_back(doc);
        return write(path, render({doc}, runId_, timestamp_, "", config_));
    } catch (const std::exception& e) {
        std::cerr << "[PDF] Export der Iteration fehlgeschlagen (" << e.what() << ") - der Run läuft weiter\n";
    } catch (...) {
        std::cerr << "[PDF] Export der Iteration fehlgeschlagen - der Run läuft weiter\n";
    }
    return false;
}

bool PdfReportExporter::finish() noexcept {
    if (finished_ || !config_.enabled || !config_.combined) return false;
    if (combinedPath_.empty() || (reports_.empty() && iterations_.empty())) return false;
    finished_ = true;
    try {
        // final reports first, then the iterations in the order they happened
        std::vector<Document> documents = reports_;
        documents.insert(documents.end(), iterations_.begin(), iterations_.end());
        return write(combinedPath_, render(documents, runId_, timestamp_, "2buggAI run report", config_));
    } catch (const std::exception& e) {
        std::cerr << "[PDF] Gesamt-Report fehlgeschlagen (" << e.what() << ") - der Run läuft weiter\n";
    } catch (...) {
        std::cerr << "[PDF] Gesamt-Report fehlgeschlagen - der Run läuft weiter\n";
    }
    return false;
}
