#include "minitest.h"
#include "test_support.h"

#include "PdfReportExporter.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>

using nlohmann::ordered_json;
using support::TempProject;
using Block = PdfReportExporter::TextBlock;

namespace {

namespace fs = std::filesystem;

// Text of a PDF written by PdfReportExporter: the content streams are not compressed, so
// the strings of the "(...) Tj" operators can be read back directly. One line per text
// row (BT ... ET). Characters are Windows-1252 bytes, e.g. "\xFC" for "ü".
std::string pdfText(const std::string& pdf) {
    std::string out;
    size_t pos = 0;
    while ((pos = pdf.find("BT\n", pos)) != std::string::npos) {
        const size_t end = pdf.find("ET\n", pos);
        if (end == std::string::npos) break;
        for (size_t i = pos; i < end; ++i) {
            if (pdf[i] != '(') continue;
            for (++i; i < end && pdf[i] != ')'; ++i) {
                if (pdf[i] != '\\') { out += pdf[i]; continue; }
                ++i;
                if (pdf[i] >= '0' && pdf[i] <= '7') {
                    out += static_cast<char>(std::stoi(pdf.substr(i, 3), nullptr, 8));
                    i += 2;
                } else {
                    out += pdf[i];
                }
            }
        }
        out += '\n';
        pos = end;
    }
    return out;
}

size_t pageCount(const std::string& pdf) {
    size_t count = 0;
    for (size_t pos = 0; (pos = pdf.find("/Type /Page ", pos)) != std::string::npos; ++pos) ++count;
    return count;
}

// Structure of the file: header, trailer, every cross-reference entry points at its
// object and every stream has exactly the declared length.
bool validStructure(const std::string& pdf) {
    if (pdf.rfind("%PDF-1.4\n", 0) != 0) return false;
    if (pdf.size() < 6 || pdf.compare(pdf.size() - 6, 6, "%%EOF\n") != 0) return false;

    const size_t startxref = pdf.rfind("startxref\n");
    if (startxref == std::string::npos) return false;
    const size_t xref = std::stoul(pdf.substr(startxref + 10));
    if (pdf.compare(xref, 7, "xref\n0 ") != 0) return false;

    std::istringstream table(pdf.substr(xref + 7));
    size_t entries = 0;
    table >> entries;
    std::string line;
    std::getline(table, line);
    for (size_t i = 0; i < entries; ++i) {
        std::getline(table, line);
        if (line.size() != 19) return false;                 // 20 bytes including the newline
        if (i == 0) continue;                                // free entry
        const std::string expected = std::to_string(i) + " 0 obj\n";
        if (pdf.compare(std::stoul(line.substr(0, 10)), expected.size(), expected) != 0) return false;
    }

    const std::string marker = " >>\nstream\n";
    for (size_t pos = 0; (pos = pdf.find("<< /Length ", pos)) != std::string::npos; ++pos) {
        const size_t length = std::stoul(pdf.substr(pos + 11));
        const size_t data = pdf.find(marker, pos) + marker.size();
        if (pdf.compare(data + length, 9, "endstream") != 0) return false;
    }
    return entries > 5;
}

std::string renderOne(const std::string& json, const std::string& name = "report.json") {
    return PdfReportExporter::render({{name, json}}, "RUN-42", "2026-01-02 03:04:05");
}

bool has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

// the exporter logs every written file and every failure
struct Quiet {
    std::ostringstream sink;
    std::streambuf* out = std::cout.rdbuf(sink.rdbuf());
    std::streambuf* err = std::cerr.rdbuf(sink.rdbuf());
    ~Quiet() { std::cout.rdbuf(out); std::cerr.rdbuf(err); }
};

bool isPdfFile(const fs::path& path) {
    const std::string content = support::readFile(path.string());
    return content.size() > 200 && validStructure(content);
}

}

// ---------------------------------------------------------------- blocks

TEST_CASE("pdf: multi-line strings become blocks with JSON paths") {
    const ordered_json report = {
        {"source", "int main() {\n    return 0;\n}\n"},
        {"single", "one line"},
        {"single_with_newline", "one line\n"},
        {"agents", {{"debugger", {{"answer", "line 1\r\nline 2"}}}}},
        {"steps", ordered_json::array({{{"output", "ok"}}, {{"output", "a\nb\nc\nd"}}})},
        {"odd key", "x\ny"},
        {"count", 3},
    };

    std::vector<Block> blocks;
    const ordered_json shown = PdfReportExporter::extractBlocks(report, blocks);

    REQUIRE(blocks.size() == 4);
    CHECK(blocks[0].number == 1);
    CHECK(blocks[0].path == "$.source");
    CHECK(blocks[0].lineCount == 3);
    CHECK(blocks[0].text == "int main() {\n    return 0;\n}\n");      // original text is kept
    CHECK(blocks[1].path == "$.agents.debugger.answer");
    CHECK(blocks[1].lineCount == 2);
    CHECK(blocks[2].path == "$.steps[1].output");
    CHECK(blocks[2].lineCount == 4);
    CHECK(blocks[3].path == "$['odd key']");
    CHECK(blocks[3].number == 4);

    // references in the JSON, everything else untouched and in the original order
    CHECK(shown["source"] == "[block #1: 3 lines] int main() {...");
    CHECK(shown["agents"]["debugger"]["answer"] == "[block #2: 2 lines] line 1...");
    CHECK(shown["steps"][1]["output"] == "[block #3: 4 lines] a...");
    CHECK(shown["single"] == "one line");
    CHECK(shown["single_with_newline"] == "one line\n");               // one line: stays inline
    CHECK(shown["steps"][0]["output"] == "ok");
    CHECK(shown["count"] == 3);
    CHECK(shown.begin().key() == "source");
    CHECK(report["source"] == "int main() {\n    return 0;\n}\n");      // input not modified
}

TEST_CASE("pdf: block reference texts") {
    CHECK(PdfReportExporter::blockReference(1, "a\nb") == "[block #1: 2 lines] a...");
    // preview = first line with content, without its indentation
    CHECK(PdfReportExporter::blockReference(7, "\n\n    def f():\n        pass\n") == "[block #7: 4 lines] def f():...");
    // long first lines are shortened to 60 characters
    const std::string longLine(200, 'x');
    CHECK(PdfReportExporter::blockReference(2, longLine + "\nend")
          == "[block #2: 2 lines] " + std::string(60, 'x') + "...");
    // a multi-byte character is not cut in half
    const std::string umlauts = std::string(59, 'a') + "\xC3\xBC" + "bbb\nzweite Zeile";
    CHECK(PdfReportExporter::blockReference(3, umlauts) == "[block #3: 2 lines] " + std::string(59, 'a') + "...");
    CHECK(PdfReportExporter::blockReference(4, "\n\n") == "[block #4: 0 lines] ...");
}

// ---------------------------------------------------------------- masking

TEST_CASE("pdf: sensitive keys") {
    for (const char* key : {"api_key", "API_KEY", "apiKey", "openai-api-key", "token", "access_token", "Authorization",
                            "password", "db_password", "client_secret", "private_key", "aws_credentials"}) {
        CHECK(PdfReportExporter::isSensitiveKey(key));
    }
    for (const char* key : {"prompt_tokens", "max_tokens", "completion_tokens", "tokenizer", "author", "path",
                            "source_code", "keywords", "exit_code"}) {
        CHECK(!PdfReportExporter::isSensitiveKey(key));
    }
}

TEST_CASE("pdf: sensitive fields are masked, everything else is kept") {
    const ordered_json report = {
        {"api_key", "sk-proj-abcdefgh12345678"},
        {"config", {{"token", "t0p-s3cret"}, {"authorization", "Bearer abcdefghijklmnop"}, {"model", "gpt-5.2"}}},
        {"providers", ordered_json::array({{{"name", "claude"}, {"password", 12345}, {"secret", nullptr}}})},
        {"usage", {{"prompt_tokens", 100}, {"max_tokens", 8000}}},
        {"log", "request failed (key sk-ant-REALKEY1234567890) with header Authorization: Bearer eyJhbGciOi.payload.sig"},
    };
    const ordered_json masked = PdfReportExporter::maskSecrets(report);

    CHECK(masked["api_key"] == "***");
    CHECK(masked["config"]["token"] == "***");
    CHECK(masked["config"]["authorization"] == "***");
    CHECK(masked["config"]["model"] == "gpt-5.2");
    CHECK(masked["providers"][0]["password"] == "***");
    CHECK(masked["providers"][0]["secret"].is_null());
    CHECK(masked["providers"][0]["name"] == "claude");
    CHECK(masked["usage"]["prompt_tokens"] == 100);                     // counters are not secrets
    CHECK(masked["usage"]["max_tokens"] == 8000);
    CHECK(masked["log"] == "request failed (key sk-***) with header Authorization: Bearer ***");
    CHECK(report["api_key"] == "sk-proj-abcdefgh12345678");             // input not modified

    const std::string dumped = masked.dump();
    CHECK(!has(dumped, "abcdefgh12345678"));
    CHECK(!has(dumped, "t0p-s3cret"));
    CHECK(!has(dumped, "REALKEY"));
    CHECK(!has(dumped, "eyJhbGciOi"));
}

TEST_CASE("pdf: key-like text is masked only where it looks like a key") {
    CHECK(PdfReportExporter::maskText("OPENAI_API_KEY=sk-proj-AAAABBBBCCCC") == "OPENAI_API_KEY=sk-***");
    CHECK(PdfReportExporter::maskText("Bearer abc") == "Bearer abc");                           // too short for a token
    CHECK(PdfReportExporter::maskText("task-force-meeting-notes") == "task-force-meeting-notes");
    CHECK(PdfReportExporter::maskText("risk-assessment-2026-final") == "risk-assessment-2026-final");
    CHECK(PdfReportExporter::maskText("sk-short") == "sk-short");
    CHECK(PdfReportExporter::maskText("") == "");
}

TEST_CASE("pdf: secrets never reach the PDF bytes") {
    const std::string pdf = renderOne(R"({"api_key": "sk-proj-abcdefgh12345678", "nested": {"auth_token": "tok-99887766"},
        "output": "line one\nAuthorization: Bearer eyJhbGciOiJIUzI1NiJ9.payload\nline three"})");
    CHECK(!has(pdf, "abcdefgh12345678"));
    CHECK(!has(pdf, "tok-99887766"));
    CHECK(!has(pdf, "eyJhbGciOiJIUzI1NiJ9"));
    const std::string text = pdfText(pdf);
    CHECK(has(text, "\"api_key\": \"***\""));
    CHECK(has(text, "Authorization: Bearer ***"));
}

// ---------------------------------------------------------------- rendering

TEST_CASE("pdf: a report becomes a valid PDF with header, colours, blocks and page numbers") {
    const std::string pdf = renderOne(R"({
        "target": "/work/proj", "exit_code": 1, "fixed": true, "gdb": null, "ratio": 0.5,
        "source_code": {"content": "#include <stdio.h>\nint main(void) {\n\treturn 0\n}\n"},
        "empty_list": [], "empty_object": {}
    })");

    CHECK(pdf.rfind("%PDF", 0) == 0);
    CHECK(pdf.size() > 1000);
    CHECK(validStructure(pdf));
    CHECK(pageCount(pdf) == 1);

    const std::string text = pdfText(pdf);
    CHECK(has(text, "report.json"));                                // header: file name
    CHECK(has(text, "Run RUN-42   2026-01-02 03:04:05"));           // header: run id, timestamp
    CHECK(has(text, "Page 1 of 1"));                                // footer
    CHECK(has(text, "  \"target\": \"/work/proj\","));              // indented, keys in file order
    CHECK(has(text, "  \"exit_code\": 1,"));
    CHECK(has(text, "  \"fixed\": true,"));
    CHECK(has(text, "  \"gdb\": null,"));
    CHECK(has(text, "  \"empty_list\": [],"));
    CHECK(has(text, "  \"empty_object\": {}"));
    CHECK(text.find("\"target\"") < text.find("\"exit_code\""));

    // the multi-line string is a reference in the JSON and a numbered block at the end
    CHECK(has(text, "\"content\": \"[block #1: 4 lines] #include <stdio.h>...\""));
    CHECK(!has(text, "\\n"));                                       // no escaped newlines anywhere
    CHECK(has(text, "Block #1  $.source_code.content  (4 lines)"));
    CHECK(has(text, "    1 | #include <stdio.h>"));
    CHECK(has(text, "    3 |     return 0"));                       // tab -> 4 spaces
    CHECK(has(text, "    4 | }"));
    CHECK(text.find("Block #1  ") > text.find("\"empty_object\""));

    // syntax colours (fill colour operators) and the grey block background
    CHECK(has(pdf, "0.10 0.30 0.65 rg"));                           // keys
    CHECK(has(pdf, "0.05 0.45 0.15 rg"));                           // strings
    CHECK(has(pdf, "0.75 0.35 0.00 rg"));                           // numbers
    CHECK(has(pdf, "0.55 0.15 0.60 rg"));                           // true / false / null
    CHECK(has(pdf, "0.94 0.94 0.94 rg"));                           // block background
    CHECK(has(pdf, "/BaseFont /Courier "));                         // monospace
}

TEST_CASE("pdf: invalid JSON gives a notice and the raw text, no exception") {
    const std::string pdf = renderOne("{\"errors\": [1, 2,\n  \"password\": \"hunter2\"\nnot json at all");
    CHECK(validStructure(pdf));
    const std::string text = pdfText(pdf);
    CHECK(has(text, "Invalid JSON: this file could not be parsed."));
    CHECK(has(text, "parse error at line 2"));                      // parser message
    CHECK(has(text, "Raw content"));
    CHECK(has(text, "    1 | {\"errors\": [1, 2,"));
    CHECK(has(text, "    3 | not json at all"));
    CHECK(has(text, "\"password\": ***"));                           // masked in raw text as well
    CHECK(!has(pdf, "hunter2"));
}

TEST_CASE("pdf: empty and minimal JSON") {
    const std::string empty = renderOne("");
    CHECK(validStructure(empty));
    CHECK(has(pdfText(empty), "Invalid JSON: the file is empty."));
    CHECK(has(pdfText(renderOne("  \n\t ")), "Invalid JSON: the file is empty."));

    for (const char* json : {"{}", "[]", "null", "42", "\"just a string\""}) {
        const std::string pdf = renderOne(json);
        CHECK(validStructure(pdf));
        CHECK(pageCount(pdf) == 1);
        CHECK(has(pdfText(pdf), json));
        CHECK(!has(pdfText(pdf), "Invalid JSON"));
    }

    // no documents at all still gives a readable one-page file
    CHECK(validStructure(PdfReportExporter::render({}, "", "now")));
}

TEST_CASE("pdf: umlauts and other non-ASCII text") {
    const std::string pdf = renderOne(
        "{\"gr\xC3\xB6\xC3\x9F" "e\": \"\xC3\x84rger mit \xC3\x9C" "berl\xC3\xA4ufen \xE2\x80\x93 5 \xE2\x82\xAC\","
        " \"emoji\": \"ok \xF0\x9F\x98\x80 \xE4\xB8\xAD\", \"box\": \"\xE2\x95\x94\xE2\x95\x90\xE2\x95\x97\"}");
    CHECK(validStructure(pdf));
    const std::string text = pdfText(pdf);
    CHECK(has(text, "\"gr\xF6\xDF" "e\": \"\xC4rger mit \xDC" "berl\xE4ufen \x96 5 \x80\""));   // Windows-1252
    CHECK(has(text, "\"emoji\": \"ok ? ?\""));                      // outside the font: placeholder
    CHECK(has(text, "\"box\": \"+=+\""));
    CHECK(has(pdf, "/Encoding /WinAnsiEncoding"));
    CHECK(has(pdf, "\\366"));                                       // ö is written as an octal escape

    // broken UTF-8 (cut sequence, stray bytes) must not crash or produce invalid output
    const std::string broken = renderOne(std::string("{\"x\": \"abc\xC3\"}") + "\xFF\xFE");
    CHECK(validStructure(broken));

    // invalid UTF-8 inside iteration data (e.g. raw tool output) is replaced, not thrown
    TempProject p;
    Quiet quiet;
    PdfExportConfig config;
    config.perIteration = true;
    config.combined = false;
    config.outputDir = p.path();
    PdfReportExporter exporter(config);
    CHECK(exporter.exportIteration(1, {{"error_output", std::string("caf\xC3 \xFF\nzweite Zeile")}}, "", "run"));
    CHECK(isPdfFile(p.path() + "/run.iter01.pdf"));
}

TEST_CASE("pdf: long lines wrap, nothing is cut off") {
    std::string longValue;
    for (int i = 0; i < 120; ++i) longValue += "word" + std::to_string(i) + " ";
    longValue += "END_OF_VALUE";
    const std::string unbroken(400, 'q');

    ordered_json report = {{"long", longValue}, {"unbroken", unbroken + "TAIL"},
                           {"code", "short\n" + std::string(300, 'z') + "LINE_END\nlast"}};
    const std::string text = pdfText(renderOne(report.dump()));

    CHECK(has(text, "END_OF_VALUE"));
    CHECK(has(text, "TAIL"));
    CHECK(has(text, "LINE_END"));
    CHECK(has(text, "word0 word1"));

    // no row is wider than the page (107 columns at 8pt Courier on A4)
    size_t widest = 0, rows = 0;
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line); ++rows) widest = std::max(widest, line.size());
    CHECK(widest <= 107);
    CHECK(widest > 90);

    // all characters of the unbroken value are still there
    size_t q = 0;
    for (char c : text) q += c == 'q';
    CHECK(q == 400);
    // wrapped code lines keep the gutter but get no new line number
    CHECK(has(text, "    2 | zzzz"));
    CHECK(has(text, "      | zzzz"));
    CHECK(has(text, "    3 | last"));
}

TEST_CASE("pdf: page breaks and page numbers") {
    ordered_json list = ordered_json::array();
    for (int i = 0; i < 200; ++i) list.push_back("entry " + std::to_string(i));
    const std::string pdf = renderOne(list.dump());

    CHECK(validStructure(pdf));
    CHECK(pageCount(pdf) == 3);                                     // 202 rows, 74 per page
    const std::string text = pdfText(pdf);
    CHECK(has(text, "Page 1 of 3"));
    CHECK(has(text, "Page 2 of 3"));
    CHECK(has(text, "Page 3 of 3"));
    CHECK(has(text, "\"entry 199\""));
}

TEST_CASE("pdf: combined report has a cover page and one section per document") {
    const std::string pdf = PdfReportExporter::render(
        {{"report.json", R"({"a": 1})"}, {"report.iter01", R"({"iteration": 1, "fixed_code": "x\ny"})"}, {"broken.json", "{"}},
        "RUN-7", "2026-01-02 03:04:05", "2buggAI run report");

    CHECK(validStructure(pdf));
    CHECK(pageCount(pdf) == 4);                                     // cover + 3 documents
    const std::string text = pdfText(pdf);
    CHECK(has(text, "2buggAI run report"));
    CHECK(has(text, "Run ID:   RUN-7"));
    CHECK(has(text, "Documents (3), each starting on a new page:"));
    CHECK(has(text, "  1. report.json"));
    CHECK(has(text, "  2. report.iter01"));
    CHECK(has(text, "  3. broken.json"));
    CHECK(has(text, "Page 4 of 4"));
    CHECK(has(text, "\"iteration\": 1,"));
    CHECK(has(text, "Block #1  $.fixed_code  (2 lines)"));          // block numbers restart per document
    CHECK(has(text, "Invalid JSON"));
}

TEST_CASE("pdf: a JSON file larger than 5 MB is exported completely") {
    // ~6 MB: 3000 steps, each with a 25 line output
    std::string output;
    for (int i = 0; i < 25; ++i) output += "tests/test_module.py::test_case_number_" + std::to_string(i) + " PASSED with some more text\n";
    ordered_json steps = ordered_json::array();
    for (int i = 0; i < 3000; ++i) steps.push_back({{"name", "step " + std::to_string(i)}, {"exit_code", i % 3}, {"output", output}});
    const std::string json = ordered_json{{"steps", steps}, {"last", "LAST_FIELD"}}.dump(2);
    REQUIRE(json.size() > 5 * 1024 * 1024);

    const auto start = std::chrono::steady_clock::now();
    const std::string pdf = renderOne(json, "large.json");
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    CHECK(pdf.rfind("%PDF", 0) == 0);
    CHECK(validStructure(pdf));
    CHECK(pageCount(pdf) > 1000);
    CHECK(seconds < 60.0);
    const std::string text = pdfText(pdf);
    CHECK(has(text, "\"last\": \"LAST_FIELD\""));
    CHECK(has(text, "Block #3000  $.steps[2999].output  (25 lines)"));
    CHECK(!has(text, "Report truncated"));
}

TEST_CASE("pdf: extremely long reports end with a notice instead of exhausting memory") {
    std::string lines;
    for (int i = 0; i < 300000; ++i) lines += "x\n";
    const std::string pdf = renderOne(ordered_json{{"output", lines}}.dump(), "huge.json");

    CHECK(validStructure(pdf));
    CHECK(pageCount(pdf) < 3500);
    CHECK(has(pdfText(pdf), "Report truncated: the file has more than 250000 lines. The JSON file is complete."));
}

TEST_CASE("pdf: deeply nested JSON does not overflow the stack") {
    std::string json;
    for (int i = 0; i < 1000; ++i) json += "{\"a\":";
    json += "\"deep\\nvalue\"";      // "\n" escaped: valid JSON
    for (int i = 0; i < 1000; ++i) json += "}";
    const std::string pdf = renderOne(json);
    CHECK(validStructure(pdf));
    CHECK(!has(pdfText(pdf), "Invalid JSON"));
}

TEST_CASE("pdf: fix recommendations get their own text field before the JSON") {
    CHECK(PdfReportExporter::isRecommendationKey("ai_analysis"));
    CHECK(PdfReportExporter::isRecommendationKey("fix_recommendations"));
    CHECK(PdfReportExporter::isRecommendationKey("Recommendations"));
    CHECK(!PdfReportExporter::isRecommendationKey("source_code"));
    CHECK(!PdfReportExporter::isRecommendationKey("compile_output"));

    const ordered_json report = {
        {"target", "/work/proj"},
        {"compile_output", "main.c:6: error: expected ';'\n    6 |     return 0\n"},
        {"ai_analysis", "## Summary\nA semicolon is missing after `return 0`.\n\n\n### Bug 1: Missing semicolon\n"
                        "- **File:** main.c:6\n\n**Fixed code:**\n```\n    return 0;\n```\n\n## Recommendations\n"
                        "Compile with -Wall -Werror in CI (key sk-proj-abcdefgh12345678).\n"},
    };
    const std::string pdf = renderOne(report.dump());
    CHECK(validStructure(pdf));
    const std::string text = pdfText(pdf);

    // own titled field, placed before the JSON, with the source of the text
    REQUIRE(has(text, "Fix recommendations\n"));
    CHECK(has(text, "from $.ai_analysis  (14 lines)"));
    CHECK(has(text, "JSON report\n"));
    CHECK(text.find("Fix recommendations\n") < text.find("JSON report\n"));
    CHECK(text.find("JSON report\n") < text.find("\"target\""));

    // readable text: no line numbers, headings without '#', fences not printed
    CHECK(has(text, "\nSummary\n"));
    CHECK(has(text, "\nA semicolon is missing after `return 0`.\n"));
    CHECK(has(text, "\nBug 1: Missing semicolon\n"));
    CHECK(has(text, "\nRecommendations\n"));
    CHECK(has(text, "\n      return 0;\n"));                         // fenced code, indented
    CHECK(!has(text, "```"));
    CHECK(!has(text, "## Summary"));
    CHECK(!has(text, "| ## "));                                      // not a numbered code block
    CHECK(text.find("\nRecommendations\n") < text.find("JSON report\n"));

    // the JSON only refers to it; other multi-line strings are still ordinary blocks
    CHECK(has(text, "\"ai_analysis\": \"[shown above as Fix recommendations: 14 lines]\""));
    CHECK(has(text, "Block #1  $.compile_output  (2 lines)"));
    CHECK(!has(text, "$.ai_analysis  (14 lines)\n    1 |"));
    CHECK(!has(text, "Block #2"));

    // its own background colour (light blue), code inside it grey; secrets masked there too
    CHECK(has(pdf, "0.91 0.95 1.00 rg"));
    CHECK(has(pdf, "0.94 0.94 0.94 rg"));
    CHECK(has(text, "(key sk-***)"));
    CHECK(!has(pdf, "abcdefgh12345678"));

    // reports without recommendations look as before; nested or non-string fields are ignored
    const std::string plain = pdfText(renderOne(R"({"a": 1, "steps": [{"ai_analysis": "x\ny"}], "recommendations": ["a"]})"));
    CHECK(!has(plain, "Fix recommendations"));
    CHECK(!has(plain, "JSON report"));
    CHECK(has(plain, "Block #1  $.steps[0].ai_analysis  (2 lines)"));

    // a one-line recommendation is shown in the field as well; an empty one is not
    const std::string oneLine = pdfText(renderOne(R"({"fix_recommendations": "Add the missing semicolon in main.c:6."})"));
    CHECK(has(oneLine, "from $.fix_recommendations  (1 lines)"));
    CHECK(has(oneLine, "\nAdd the missing semicolon in main.c:6.\n"));
    CHECK(!has(pdfText(renderOne(R"({"ai_analysis": ""})")), "Fix recommendations"));
}

// ---------------------------------------------------------------- configuration

TEST_CASE("pdf: configuration defaults and pdf_export section") {
    const PdfExportConfig defaults;
    CHECK(defaults.enabled);
    CHECK(defaults.combined);
    CHECK(!defaults.perIteration);
    CHECK(defaults.outputDir.empty());

    const PdfExportConfig none = PdfReportExporter::parseConfig(R"({"stack": "fullstack"})");
    CHECK(none.enabled);
    CHECK(none.combined);

    const PdfExportConfig set = PdfReportExporter::parseConfig(
        R"({"pdf_export": {"enabled": false, "combined": false, "per_iteration": true, "output_dir": "reports"}})");
    CHECK(!set.enabled);
    CHECK(!set.combined);
    CHECK(set.perIteration);
    CHECK(set.outputDir == "reports");

    // wrong types are ignored, broken files fall back to the defaults with an error text
    const PdfExportConfig odd = PdfReportExporter::parseConfig(R"({"pdf_export": {"enabled": "no", "output_dir": 5}})");
    CHECK(odd.enabled);
    CHECK(odd.outputDir.empty());
    std::string error;
    CHECK(PdfReportExporter::parseConfig("not json", &error).enabled);
    CHECK(!error.empty());
    error.clear();
    PdfReportExporter::parseConfig(R"({"pdf_export": true})", &error);
    CHECK(!error.empty());
}

TEST_CASE("pdf: project configuration is read from .buggai.json") {
    TempProject p;
    p.write("main.c", "int main(void){return 0;}\n");
    CHECK(PdfReportExporter::loadConfig(p.path()).enabled);            // no file: defaults

    p.write(".buggai.json", R"({"pdf_export": {"per_iteration": true, "output_dir": "reports"}})");
    const PdfExportConfig fromDir = PdfReportExporter::loadConfig(p.path());
    CHECK(fromDir.perIteration);
    CHECK(fromDir.outputDir == p.path() + "/reports");                  // relative to the project
    // a single file as target uses the config next to it
    CHECK(PdfReportExporter::loadConfig(p.path() + "/main.c").outputDir == p.path() + "/reports");

    p.write(".buggai.json", R"({"pdf_export": {"output_dir": "/abs/reports"}})");
    CHECK(PdfReportExporter::loadConfig(p.path()).outputDir == "/abs/reports");
}

// ---------------------------------------------------------------- exporter (files)

TEST_CASE("pdf: exporter writes a PDF next to each JSON and a combined report") {
    TempProject p;
    Quiet quiet;
    const std::string json = p.path() + "/out/report.json";
    p.write("out/report.json", R"({"detected_errors": ["syntax_error"]})");
    {
        PdfReportExporter exporter;
        CHECK(exporter.exportJson(R"({"detected_errors": ["syntax_error"]})", json));
        CHECK(isPdfFile(p.path() + "/out/report.pdf"));
        CHECK(!fs::exists(p.path() + "/out/report.combined.pdf"));      // written at the end of the run

        // the same JSON written again (final analysis added) replaces the PDF and its entry
        CHECK(exporter.exportJson(R"({"detected_errors": [], "ai_analysis": "## Summary\nfixed"})", json));
        CHECK(!exporter.exportIteration(1, {{"iteration", 1}}, json));  // per_iteration is off by default
        CHECK(exporter.runId().size() > 10);
        CHECK(exporter.writtenFiles().size() == 1);
    }   // destructor = end of the run

    CHECK(isPdfFile(p.path() + "/out/report.combined.pdf"));
    const std::string combined = pdfText(support::readFile(p.path() + "/out/report.combined.pdf"));
    CHECK(has(combined, "Documents (1), each starting on a new page:"));
    CHECK(has(combined, "\"ai_analysis\": \"[shown above as Fix recommendations: 2 lines]\""));
    CHECK(has(combined, "Fix recommendations"));
    CHECK(!has(combined, "syntax_error"));                              // only the final version
    CHECK(!fs::exists(p.path() + "/out/report.pdf.tmp"));
    CHECK(!fs::exists(p.path() + "/out/report.iter01.pdf"));
    CHECK(support::readFile(json) == R"({"detected_errors": ["syntax_error"]})");   // JSON untouched
}

TEST_CASE("pdf: output folder, per-iteration PDFs and their order in the combined report") {
    TempProject p;
    Quiet quiet;
    PdfExportConfig config;
    config.perIteration = true;
    config.outputDir = p.path() + "/reports/pdf";                       // does not exist yet
    PdfReportExporter exporter(config);

    const std::string json = p.path() + "/report.json";
    CHECK(exporter.exportIteration(1, {{"iteration", 1}, {"error", "syntax_error"}, {"fixed_code", "a\nb"}}, json));
    CHECK(exporter.exportIteration(2, {{"iteration", 2}, {"error", "linker_error"}}, json));
    CHECK(exporter.exportJson(R"({"final": true})", json));
    CHECK(exporter.finish());
    CHECK(!exporter.finish());                                          // once per run

    for (const char* name : {"report.pdf", "report.iter01.pdf", "report.iter02.pdf", "report.combined.pdf"}) {
        CHECK(isPdfFile(config.outputDir + "/" + name));
    }
    CHECK(!fs::exists(p.path() + "/report.pdf"));                       // not next to the JSON
    CHECK(exporter.writtenFiles().size() == 4);

    const std::string combined = pdfText(support::readFile(config.outputDir + "/report.combined.pdf"));
    CHECK(has(combined, "  1. report.json"));                           // final report first
    CHECK(has(combined, "  2. report.iter01"));
    CHECK(has(combined, "  3. report.iter02"));
    CHECK(has(combined, "\"error\": \"linker_error\""));
}

TEST_CASE("pdf: without a JSON file a PDF is only written into a configured folder") {
    TempProject p;
    Quiet quiet;
    {
        PdfReportExporter exporter;                                     // no output_dir
        CHECK(!exporter.exportJson("{}", "", "proj_report"));
        CHECK(!exporter.finish());
        CHECK(exporter.writtenFiles().empty());
    }
    {
        PdfExportConfig config;
        config.outputDir = p.path() + "/reports";
        PdfReportExporter exporter(config);
        CHECK(exporter.exportJson(R"({"a": 1})", "", "proj_report"));
    }
    CHECK(isPdfFile(p.path() + "/reports/proj_report.pdf"));
    CHECK(isPdfFile(p.path() + "/reports/proj_report.combined.pdf"));
}

TEST_CASE("pdf: disabled export and disabled combined report") {
    TempProject p;
    Quiet quiet;
    const std::string json = p.path() + "/report.json";
    {
        PdfExportConfig config;
        config.enabled = false;
        config.perIteration = true;
        PdfReportExporter exporter(config);
        CHECK(!exporter.exportJson("{}", json));
        CHECK(!exporter.exportIteration(1, {{"x", 1}}, json));
    }
    CHECK(fs::is_empty(p.path()));

    {
        PdfExportConfig config;
        config.combined = false;
        PdfReportExporter exporter(config);
        CHECK(exporter.exportJson("{}", json));
    }
    CHECK(fs::exists(p.path() + "/report.pdf"));
    CHECK(!fs::exists(p.path() + "/report.combined.pdf"));
}

TEST_CASE("pdf: a failing export is reported and never throws") {
    TempProject p;
    p.write("blocker", "a file where a folder is needed");

    std::ostringstream log;
    std::streambuf* oldErr = std::cerr.rdbuf(log.rdbuf());
    std::streambuf* oldOut = std::cout.rdbuf(log.rdbuf());
    bool threw = false;
    try {
        PdfExportConfig config;
        config.perIteration = true;
        config.outputDir = p.path() + "/blocker/reports";               // cannot be created
        PdfReportExporter exporter(config);
        CHECK(!exporter.exportJson(R"({"a": 1})", p.path() + "/report.json"));
        CHECK(!exporter.exportIteration(1, {{"x", 1}}, p.path() + "/report.json"));
        CHECK(!exporter.finish());
        CHECK(exporter.writtenFiles().empty());

        // unwritable target next to the JSON
        PdfReportExporter next;
        CHECK(!next.exportJson("{}", "/proc/this/does/not/exist/report.json"));
    } catch (...) {
        threw = true;
    }
    std::cerr.rdbuf(oldErr);
    std::cout.rdbuf(oldOut);

    CHECK(!threw);
    CHECK(has(log.str(), "[PDF] Export fehlgeschlagen"));
    CHECK(has(log.str(), "der Run l\xC3\xA4uft weiter"));
    CHECK(support::readFile(p.path() + "/blocker") == "a file where a folder is needed");
}

TEST_CASE("pdf: text can be extracted with pdftotext (skipped when it is not installed)") {
    if (std::system("command -v pdftotext > /dev/null 2>&1") != 0) {
        std::cout << "    (pdftotext is not installed - skipped; the text is checked from the PDF streams instead)\n";
        return;
    }
    TempProject p;
    p.write("report.pdf", renderOne(R"({"name": "Größe", "source": "int main() {\n  return 0;\n}"})"));
    const std::string out = p.path() + "/report.txt";
    REQUIRE(std::system(("pdftotext -layout '" + p.path() + "/report.pdf' '" + out + "' 2>/dev/null").c_str()) == 0);
    const std::string text = support::readFile(out);
    CHECK(has(text, "\"name\": \"Gr\xC3\xB6\xC3\x9F" "e\""));
    CHECK(has(text, "$.source"));
    CHECK(has(text, "Page 1 of 1"));
}
