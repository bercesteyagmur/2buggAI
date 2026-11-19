#include "minitest.h"
#include "test_support.h"

#include "FixRecord.h"
#include "OpenAiClient.h"
#include "PdfReportExporter.h"

#include <stdexcept>
#include <string>
#include <vector>

using nlohmann::json;
using nlohmann::ordered_json;
using support::TempProject;

namespace {

// Text of a PDF written by PdfReportExporter (uncompressed streams), one line per row;
// pages are separated by "\f".
std::string pdfText(const std::string& pdf) {
    std::string out;
    size_t stream = 0;
    while ((stream = pdf.find("stream\n", stream)) != std::string::npos) {
        const size_t end = pdf.find("endstream", stream);
        if (end == std::string::npos) break;
        const std::string content = pdf.substr(stream + 7, end - stream - 7);
        size_t pos = 0;
        while ((pos = content.find("BT\n", pos)) != std::string::npos) {
            const size_t et = content.find("ET\n", pos);
            if (et == std::string::npos) break;
            for (size_t i = pos; i < et; ++i) {
                if (content[i] != '(') continue;
                for (++i; i < et && content[i] != ')'; ++i) {
                    if (content[i] != '\\') { out += content[i]; continue; }
                    ++i;
                    if (content[i] >= '0' && content[i] <= '7') {
                        out += static_cast<char>(std::stoi(content.substr(i, 3), nullptr, 8));
                        i += 2;
                    } else {
                        out += content[i];
                    }
                }
            }
            out += '\n';
            pos = et;
        }
        out += '\f';
        stream = end;
    }
    return out;
}

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// Page (1-based) on which a text first appears, 0 if not found.
size_t pageOf(const std::string& text, const std::string& part) {
    const size_t at = text.find(part);
    if (at == std::string::npos) return 0;
    size_t page = 1;
    for (size_t i = 0; i < at; ++i) page += text[i] == '\f';
    return page;
}

FixResult approvedFix(const std::string& file, const std::string& code) {
    FixResult r;
    r.file_path = file;
    r.fixed_code = code;
    r.success = true;
    r.explanation.root_cause = "Die Ursache.";
    r.explanation.fix = "Der Fix.";
    r.review.ran = true;
    r.review.approved = true;
    r.review.verdict = "APPROVE";
    r.review.comment = "ok";
    r.verification.ran = true;
    r.verification.confirmed = true;
    return r;
}

// A small fix report as FixReportBuilder writes it, built through the builder itself.
json sampleFixReport(const TempProject& project, int contextLines = 3) {
    project.write("main.c", "#include <stdio.h>\n\nint main(void) {\n    int x = 5;\n    printf(\"%d\\n\", x);\n    return 0\n}\n");
    FixReportBuilder builder(project.path(), contextLines);
    builder.setPipeline(true);
    builder.setDescriber([](const std::string& name) {
        ErrorInfo info;
        if (name == "syntax_error") {
            info.file = "main.c";
            info.lines = {6};
            info.tool = "Compiler (gcc)";
            info.message = "main.c:7:1: error: expected ';' before '}' token\n";
        }
        return info;
    });
    builder.observe({"syntax_error", "missing_semicolon", "logic_error"});
    const int id = builder.beginError("syntax_error");
    const std::string original = support::readFile(project.path() + "/main.c");
    std::string fixed = original;
    fixed.replace(fixed.find("return 0\n"), 9, "return 0;\n");
    builder.addAttempt(id, 1, approvedFix("main.c", fixed), original, true, true, {"logic_error"}, {});
    builder.observe({"logic_error"}, id);
    builder.finishError(id, true);

    const int logic = builder.beginError("logic_error");
    FixResult rejected;
    rejected.failure_reason = "Verifier abgelehnt: falsch";
    builder.addAttempt(logic, 1, rejected, "", false, false, {}, {});
    builder.finishError(logic, false);
    builder.finalize({"logic_error"});
    return builder.toJson({{"project", "proj"}, {"models", {{"mode", "pipeline"}, {"debugger", "gemini/x"}}}});
}

std::string renderReport(const json& report, const PdfExportConfig& layout = PdfExportConfig{}) {
    const PdfReportExporter::Document doc{"report.json", report.dump(2)};
    return PdfReportExporter::render({doc}, "RUN-1", "2026-11-17 10:00:00", "", layout);
}

} // namespace

// ---------------------------------------------------------------------------
// Agent answers
// ---------------------------------------------------------------------------

TEST_CASE("agent: Debugger explanation fields are parsed") {
    const FixExplanation e = parse_fix_explanation(R"(Here is the fix:
```json
{"file_path": "a.py", "fixed_code": "x", "success": true,
 "root_cause_explanation": "Die Variable ist nicht definiert.",
 "fix_explanation": "Definition ergänzt.",
 "affected_lines": [3, 5],
 "error_category": "undeclared_variable",
 "side_effects": "keine"}
```)");
    CHECK(e.root_cause == "Die Variable ist nicht definiert.");
    CHECK(e.fix == "Definition ergänzt.");
    CHECK(e.affected_lines == (std::vector<int>{3, 5}));
    CHECK(e.error_category == "undeclared_variable");
    CHECK(e.side_effects == "keine");
}

TEST_CASE("agent: missing or broken explanation fields stay empty") {
    // the old answer format without any of the new fields
    const FixExplanation old = parse_fix_explanation(R"({"file_path": "a.py", "fixed_code": "x", "success": true})");
    CHECK(old.root_cause.empty());
    CHECK(old.fix.empty());
    CHECK(old.affected_lines.empty());
    CHECK(old.side_effects.empty());

    // wrong types
    const FixExplanation wrong = parse_fix_explanation(
        R"({"root_cause_explanation": 42, "fix_explanation": {"a": 1}, "affected_lines": {"x": 1},
            "error_category": null, "side_effects": true})");
    CHECK(wrong.root_cause.empty());
    CHECK(wrong.fix.empty());
    CHECK(wrong.affected_lines.empty());
    CHECK(wrong.error_category.empty());
    CHECK(wrong.side_effects.empty());

    // no JSON at all, truncated JSON, empty text
    CHECK(parse_fix_explanation("Sorry, I cannot help.").root_cause.empty());
    CHECK(parse_fix_explanation(R"({"root_cause_explanation": "abc)").root_cause.empty());
    CHECK(parse_fix_explanation("").affected_lines.empty());
}

TEST_CASE("agent: affected_lines accepts numbers, strings and ranges") {
    auto lines = [](const std::string& value) {
        return parse_fix_explanation(R"({"affected_lines": )" + value + "}").affected_lines;
    };
    CHECK(lines("[3, \"5\", \"7-9\", \"abc\", -1, 0, 2.0, 3]") == (std::vector<int>{3, 5, 7, 8, 9, 2}));
    CHECK(lines("\"12, 14 - 15\"") == (std::vector<int>{12, 14, 15}));
    CHECK(lines("17") == std::vector<int>{17});
    CHECK(lines("\"line 4\"") == std::vector<int>{4});
    CHECK(lines("null").empty());
    CHECK(lines("[]").empty());
    CHECK(lines("\"1-100000\"").size() == 200);     // bounded
    CHECK(lines("[99999999999]").empty());          // absurd values are dropped
}

TEST_CASE("agent: explanation given as a list of sentences is joined") {
    const FixExplanation e = parse_fix_explanation(
        R"({"root_cause_explanation": ["Satz eins.", "Satz zwei."], "side_effects": ["a", "", 5, "b"]})");
    CHECK(e.root_cause == "Satz eins. Satz zwei.");
    CHECK(e.side_effects == "a; b");
}

TEST_CASE("agent: Reviewer answer with approved and review_comment") {
    const ReviewOutcome ok = parse_review_outcome(
        R"({"verdict": "approve", "findings": [], "summary": "s", "approved": true, "review_comment": "Gut."})");
    CHECK(ok.ran);
    CHECK(ok.approved);
    CHECK(ok.verdict == "APPROVE");
    CHECK(ok.comment == "Gut.");

    // review_comment missing: the summary is the comment; findings become readable lines
    const ReviewOutcome changes = parse_review_outcome(
        R"({"verdict": "CHANGES_REQUESTED", "summary": "Zu breit.",
            "findings": [{"file": "a.c", "problem": "unrelated rename", "severity": "medium"}, "plain text", 7]})");
    CHECK(changes.ran);
    CHECK(!changes.approved);
    CHECK(changes.comment == "Zu breit.");
    CHECK(changes.findings == (std::vector<std::string>{"[medium] a.c: unrelated rename", "plain text"}));

    // verdict decides over "approved"
    CHECK(!parse_review_outcome(R"({"verdict": "CHANGES_REQUESTED", "approved": true})").approved);

    // verdict missing: "approved" is used
    const ReviewOutcome fallback = parse_review_outcome(R"({"approved": false, "review_comment": "nein"})");
    CHECK(fallback.ran);
    CHECK(fallback.verdict == "CHANGES_REQUESTED");
    CHECK(fallback.comment == "nein");

    // neither: no valid review
    CHECK(!parse_review_outcome(R"({"verdict": "MAYBE"})").ran);
    CHECK(!parse_review_outcome("no json").ran);
    CHECK(!parse_review_outcome(R"({"approved": "yes"})").ran);
}

// ---------------------------------------------------------------------------
// Masking
// ---------------------------------------------------------------------------

TEST_CASE("masking: secrets in code lines") {
    using P = PdfReportExporter;
    CHECK(P::maskCodeText(R"(const char* api_key = "abc123";)") == R"(const char* api_key = "***";)");
    CHECK(P::maskCodeText(R"(API_KEY='abc')") == R"(API_KEY='***')");
    CHECK(P::maskCodeText(R"({"password": "hunter2", "user": "bob"})") == R"({"password": "***", "user": "bob"})");
    CHECK(P::maskCodeText(R"(db_password: "x y z")") == R"(db_password: "***")");
    CHECK(P::maskCodeText(R"(token := "t0k3n")") == R"(token := "***")");
    CHECK(P::maskCodeText(R"(SECRET_KEY = f"{base}-x")") == R"(SECRET_KEY = f"***")");
    CHECK(P::maskCodeText(R"(headers["Authorization"] = "Bearer abc")") == R"(headers["Authorization"] = "***")");
    CHECK(P::maskCodeText(R"(a_token = "x"; b_secret = 'y')") == R"(a_token = "***"; b_secret = '***')");
    CHECK(P::maskCodeText("line one\napi_key = \"k\"\nline three") == "line one\napi_key = \"***\"\nline three");

    // left alone: comparisons, values that are not literals, unrelated names, empty literals
    CHECK(P::maskCodeText(R"(if (token == "EOF") return;)") == R"(if (token == "EOF") return;)");
    CHECK(P::maskCodeText("token = lexer.next();") == "token = lexer.next();");
    CHECK(P::maskCodeText(R"(name = "bob")") == R"(name = "bob")");
    CHECK(P::maskCodeText(R"(password = "")") == R"(password = "")");
    CHECK(P::maskCodeText("") == "");
}

TEST_CASE("masking: the whole fix report") {
    const ordered_json report = ordered_json::parse(R"({
        "api_key": "sk-abcdefghijklmnop",
        "fix_report": {"fixes": [{
            "message": "Authorization: Bearer abcdefghijklmnop",
            "attempts": [{
                "snippet": {"hunks": [{"old": [{"line": 1, "text": "api_key = \"sk-live-1234567890\"", "mark": true}]}]},
                "diff": "+password = 'pw'\n"
            }]
        }]}
    })");
    const std::string masked = PdfReportExporter::maskSecrets(report).dump();
    CHECK(!has(masked, "sk-live-1234567890"));
    CHECK(!has(masked, "abcdefghijklmnop"));
    CHECK(!has(masked, "'pw'"));
    CHECK(has(masked, "api_key = \\\"***\\\""));
}

// ---------------------------------------------------------------------------
// FixReportBuilder
// ---------------------------------------------------------------------------

TEST_CASE("fix report: counts, status and the attempt that fixed the error") {
    TempProject project;
    const json report = sampleFixReport(project);
    CHECK(report["schema"] == 1);
    CHECK(report["project"] == "proj");
    CHECK(report["language"] == "de");
    CHECK(report["summary"]["found"] == 3);
    CHECK(report["summary"]["fixed"] == 2);
    CHECK(report["summary"]["not_fixed"] == 1);
    REQUIRE(report["fixes"].size() == 3);

    const json& syntax = report["fixes"][0];
    CHECK(syntax["title"] == "syntax_error in main.c");
    CHECK(syntax["status"] == "fixed");
    CHECK(syntax["iterations"] == 1);
    CHECK(syntax["lines"] == json::array({6}));
    CHECK(syntax["difficulty"] == "leicht");
    const json& attempt = syntax["attempts"][0];
    CHECK(attempt["outcome"] == "fixed");
    CHECK(attempt["review"]["approved"] == true);
    CHECK(attempt["remaining_errors"] == json::array({"logic_error"}));
    CHECK(has(attempt["diff"].get<std::string>(), "+    return 0;"));
    const Snippet snippet = FixSnippet::fromJson(attempt["snippet"]);
    REQUIRE(snippet.hunks.size() == 1);
    CHECK(snippet.hunks[0].oldLines.front().line == 3);   // 3 lines of context before line 6
    // the excerpt of the original file was taken when the error was started
    CHECK(!FixSnippet::fromJson(syntax["excerpt"]).empty());

    const json& logic = report["fixes"][1];
    CHECK(logic["status"] == "not_fixed");
    CHECK(logic["attempts"][0]["outcome"] == "no_fix");
    CHECK(has(logic["status_reason"].get<std::string>(), "Nach 1 Versuch(en) nicht behoben. Letzter Grund: Verifier abgelehnt: falsch"));

    // seen at the start, never worked on, gone after the fix of section 1
    const json& together = report["fixes"][2];
    CHECK(together["error_name"] == "missing_semicolon");
    CHECK(together["status"] == "fixed");
    CHECK(together["resolved_by"] == 1);
    CHECK(together["iterations"] == 0);
    CHECK(has(together["status_reason"].get<std::string>(), "Abschnitt 1"));
}

TEST_CASE("fix report: rejected proposal, unhandled errors and late resolution") {
    TempProject project;
    project.write("a.py", "import os\nprint(x)\n");
    FixReportBuilder builder(project.path(), 1, "en");
    builder.observe({"name_error", "import_error", "never_handled"});

    // rejected proposal: the code is kept for the report, the file stays unchanged
    const int a = builder.beginError("name_error");
    FixResult proposal = approvedFix("a.py", "import os\nx = 1\nprint(x)\n");
    proposal.success = false;
    proposal.failure_reason = "Verifier rejected: no";
    builder.addAttempt(a, 1, proposal, "", false, false, {}, {});
    builder.finishError(a, false);

    // given up, then another fix removes it
    const int b = builder.beginError("import_error");
    builder.addAttempt(b, 1, FixResult{}, "", false, false, {}, {}, "disk full");
    builder.finishError(b, false);
    builder.observe({"name_error", "never_handled"}, a);   // pretend a later fix of `a` removed it

    builder.finalize({"name_error", "never_handled"});
    const json report = builder.toJson();
    REQUIRE(report["fixes"].size() == 3);

    const json& rejected = report["fixes"][0];
    CHECK(rejected["status"] == "not_fixed");
    CHECK(rejected["attempts"][0]["outcome"] == "rejected");
    CHECK(rejected["attempts"][0]["reason"] == "Verifier rejected: no");
    // file and lines are taken from the proposal when the tool output had no location
    CHECK(rejected["file"] == "a.py");
    CHECK(rejected["lines"] == json::array());   // pure insertion: no line of the old file changed
    CHECK(!FixSnippet::fromJson(rejected["attempts"][0]["snippet"]).empty());
    CHECK(support::readFile(project.path() + "/a.py") == "import os\nprint(x)\n");

    const json& late = report["fixes"][1];
    CHECK(late["attempts"][0]["outcome"] == "apply_failed");
    CHECK(late["attempts"][0]["reason"] == "Fix could not be written: disk full");
    CHECK(late["status"] == "fixed");
    CHECK(late["resolved_by"] == a);

    const json& unhandled = report["fixes"][2];
    CHECK(unhandled["status"] == "not_fixed");
    CHECK(unhandled["status_reason"] == "Not handled (round limit of the fix loop reached).");
}

TEST_CASE("fix report: a failing describer or unknown ids never throw") {
    TempProject project;
    FixReportBuilder builder(project.path(), 3);
    builder.setDescriber([](const std::string&) -> ErrorInfo { throw std::runtime_error("boom"); });
    builder.observe({"x"});
    const int id = builder.beginError("x");
    CHECK(id == 1);
    builder.addAttempt(42, 1, FixResult{}, "", false, false, {}, {});   // unknown id: ignored
    builder.finishError(-1, true);
    builder.addAttempt(id, 1, approvedFix("missing/file.c", "code"), "", true, true, {}, {});
    builder.finishError(id, true);
    builder.finalize({});
    const json report = builder.toJson();
    CHECK(report["summary"]["found"] == 1);
    CHECK(report["summary"]["fixed"] == 1);
    CHECK(report["fixes"][0]["severity"] == "error");
    CHECK(FixSnippet::fromJson(report["fixes"][0]["attempts"][0]["snippet"]).empty());   // file unreadable
}

TEST_CASE("fix report: relevant lines of the error output") {
    const std::string gcc =
        "/p/main.cpp: In function 'int f()':\n"
        "/p/main.cpp:8:13: error: 'y' was not declared in this scope\n"
        "    8 |     int z = y + a;\n"
        "      |             ^\n"
        "/p/main.cpp: In function 'int main()':\n"
        "/p/main.cpp:13:22: error: cannot convert 'std::string' to 'int'\n"
        "   13 |     int count = std::string(\"42\");\n"
        "      |                      ^~~~~~~~~~~~\n"
        "/p/main.cpp:15:13: error: expected ';' before '}' token\n"
        "   15 |     return 0\n"
        "      |             ^\n";
    const std::string semi = FixReportBuilder::excerptMessage(gcc, {"expected ';'"}, "main.cpp", 14, "/p/");
    CHECK(semi == "main.cpp:15:13: error: expected ';' before '}' token\n   15 |     return 0\n      |             ^\n");

    const std::string undeclared = FixReportBuilder::excerptMessage(gcc, {"not declared"}, "main.cpp", 14, "/p/");
    CHECK(undeclared == "main.cpp: In function 'int f()':\nmain.cpp:8:13: error: 'y' was not declared in this scope\n"
                        "    8 |     int z = y + a;\n      |             ^\n");

    const std::string traceback =
        "===== STEP: backend: pytest | exit code 1 =====\n"
        "Traceback (most recent call last):\n"
        "  File \"/venv/lib/site.py\", line 3, in run\n"
        "    load()\n"
        "  File \"backend/app/main.py\", line 3, in <module>\n"
        "    from app.modles import USERS\n"
        "ModuleNotFoundError: No module named 'app.modles'\n";
    const std::string py = FixReportBuilder::excerptMessage(traceback, {"no module named"}, "backend/app/main.py");
    CHECK(py == "  File \"backend/app/main.py\", line 3, in <module>\n    from app.modles import USERS\n"
                "ModuleNotFoundError: No module named 'app.modles'\n");

    // no keyword: lines with the file name, else the first lines; limited with "..."
    CHECK(FixReportBuilder::excerptMessage("a\nsrc/x.ts(4,1): oops\nb\n", {"zzz"}, "src/x.ts") == "src/x.ts(4,1): oops\n");
    CHECK(FixReportBuilder::excerptMessage("one\n\ntwo\n", {}, "") == "one\n...\ntwo\n");
    std::string many;
    for (int i = 0; i < 40; ++i) many += "error " + std::to_string(i) + "\n";
    const std::string limited = FixReportBuilder::excerptMessage(many, {"error"}, "", 5);
    CHECK(limited == "error 0\nerror 1\nerror 2\nerror 3\nerror 4\n...\n");
    CHECK(FixReportBuilder::excerptMessage("", {"x"}, "").empty());
}

TEST_CASE("fix report: titles and checklist keywords") {
    CHECK(FixReportBuilder::makeTitle("import_error", "backend/app.py", "ImportError: cannot import name 'x'")
          == "ImportError in backend/app.py");
    CHECK(FixReportBuilder::makeTitle("syntax_error", "main.cpp", "main.cpp:1: error: expected ';'") == "syntax_error in main.cpp");
    CHECK(FixReportBuilder::makeTitle("segfault", "", "") == "segfault");

    const std::vector<ErrorCategory> checklist = {
        {"general", "syntax_error", "detect_from_output", {"syntax error", " Expected ';'", "-", ""}},
        {"python", "syntax_error", "detect_from_output", {"SyntaxError", "syntax error"}},
        {"general", "logic_error", "ai_presume", {"-"}},
    };
    CHECK(FixReportBuilder::keywordsFor("syntax_error", checklist)
          == (std::vector<std::string>{"syntax error", "expected ';'", "syntaxerror"}));
    CHECK(FixReportBuilder::keywordsFor("logic_error", checklist).empty());
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

TEST_CASE("report config: defaults and .buggai.json values") {
    const PdfExportConfig defaults;
    CHECK(defaults.contextLines == 3);
    CHECK(defaults.showDiff);
    CHECK(!defaults.includeRawJsonAppendix);
    CHECK(defaults.language == "de");

    std::string error;
    const PdfExportConfig c = PdfReportExporter::parseConfig(
        R"({"report": {"context_lines": 5, "show_diff": false, "include_raw_json_appendix": true, "language": "EN"},
            "pdf_export": {"combined": false}})", &error);
    CHECK(error.empty());
    CHECK(c.contextLines == 5);
    CHECK(!c.showDiff);
    CHECK(c.includeRawJsonAppendix);
    CHECK(c.language == "en");
    CHECK(!c.combined);

    CHECK(PdfReportExporter::parseConfig(R"({"report": {"context_lines": 500}})").contextLines == 20);
    CHECK(PdfReportExporter::parseConfig(R"({"report": {"context_lines": -3}})").contextLines == 0);
    CHECK(PdfReportExporter::parseConfig(R"({"report": {"context_lines": "7", "show_diff": "no"}})").contextLines == 3);
    CHECK(PdfReportExporter::parseConfig(R"({"report": {"show_diff": "no"}})").showDiff);

    error.clear();
    CHECK(PdfReportExporter::parseConfig(R"({"report": {"language": "fr"}})", &error).language == "de");
    CHECK(!error.empty());
    error.clear();
    CHECK(PdfReportExporter::parseConfig(R"({"report": 3, "pdf_export": {"enabled": false}})", &error).enabled == false);
    CHECK(error == "report must be an object");
}

TEST_CASE("report config: loaded from the project folder") {
    TempProject project;
    project.write(".buggai.json", R"({"report": {"context_lines": 1, "include_raw_json_appendix": true}})");
    const PdfExportConfig c = PdfReportExporter::loadConfig(project.path());
    CHECK(c.contextLines == 1);
    CHECK(c.includeRawJsonAppendix);
    CHECK(c.enabled);
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

TEST_CASE("render: fix report with header, summary and sections") {
    TempProject project;
    json doc = {{"target", "/x"}, {"api_key", "sk-abcdefghijklmnop"}, {"fix_report", sampleFixReport(project)},
                {"ai_analysis", "## Summary\nAlles gut.\n"}};
    const std::string pdf = renderReport(doc);
    const std::string text = pdfText(pdf);

    CHECK(has(text, "2buggAI Fehlerbericht"));
    CHECK(has(text, "Run-ID:        RUN-1"));
    CHECK(has(text, "Debugger:      gemini/x"));
    CHECK(has(text, std::string("  Gefunden: 3   Behoben: 2   Nicht behoben: 1")));
    CHECK(has(text, "1. syntax_error in main.c"));
    CHECK(has(text, "BEHOBEN - Iterationen: 1"));
    CHECK(has(text, "    6 >     return 0"));
    CHECK(has(text, "    6 +     return 0;"));
    CHECK(has(text, "-    return 0\n"));
    CHECK(has(text, "WARUM DAS EIN FEHLER IST\n  Die Ursache."));
    CHECK(has(text, "ERKL\xC4RUNG DER BEHEBUNG\n  Der Fix."));
    CHECK(has(text, "M\xF6gliche Nebenwirkungen: keine angegeben"));
    CHECK(has(text, "NICHT BEHOBEN - Iterationen: 1"));
    CHECK(has(text, "Kein Code-Vorschlag vorhanden."));
    CHECK(has(text, "Keine Erkl\xE4rung vom Modell geliefert."));
    CHECK(has(text, "behoben (mit Nr. 1)"));
    CHECK(has(text, "Gesamtanalyse des Modells"));
    CHECK(!has(text, "\"detected_errors\""));
    CHECK(!has(text, "abcdefghijklmnop"));           // masked, and the JSON is not shown at all
    CHECK(!has(text, "\x01"));                       // page number placeholders were replaced
    CHECK(has(pdf, "/Subtype /Link"));
    CHECK(has(pdf, "1.00 0.87 0.87 rg"));            // red background
    CHECK(has(pdf, "0.86 0.96 0.86 rg"));            // green background
    CHECK(has(pdf, "/F2 12.00 Tf"));                  // title rows
    CHECK(pageOf(text, "Zusammenfassung") < pageOf(text, "1. syntax_error in main.c")
          || (pageOf(text, "Zusammenfassung") == pageOf(text, "1. syntax_error in main.c")
              && text.find("Zusammenfassung") < text.find("1. syntax_error in main.c")));
}

TEST_CASE("render: options for diff, appendix and language") {
    TempProject project;
    const json doc = {{"detected_errors", {"logic_error"}}, {"fix_report", sampleFixReport(project, 1)}};

    PdfExportConfig layout;
    layout.showDiff = false;
    layout.includeRawJsonAppendix = true;
    const std::string text = pdfText(renderReport(doc, layout));
    CHECK(text.find("\nDIFF\n") == std::string::npos);
    CHECK(has(text, "Anhang: roher JSON-Report"));
    CHECK(has(text, "\"detected_errors\""));
    CHECK(text.find("Anhang: roher JSON-Report") > text.find("1. syntax_error"));
    // the appendix starts on a page of its own: only the page header is above its title
    const size_t appendix = text.find("Anhang: roher JSON-Report");
    const size_t pageStart = text.rfind('\f', appendix);
    REQUIRE(pageStart != std::string::npos);
    CHECK(text.substr(pageStart + 1, appendix - pageStart - 1) == "report.json\nRun RUN-1   2026-11-17 10:00:00\n");

    json english = doc;
    english["fix_report"]["language"] = "en";
    const std::string en = pdfText(renderReport(english));
    CHECK(has(en, "2buggAI error report"));
    CHECK(has(en, "FAULTY CODE"));
    CHECK(has(en, "NOT FIXED"));
    CHECK(has(en, "No explanation provided by the model."));
}

TEST_CASE("render: without fix_report the JSON layout is unchanged") {
    const json doc = {{"detected_errors", {"x"}}, {"source_code", "a\nb\n"}};
    const std::string text = pdfText(renderReport(doc));
    CHECK(has(text, "\"detected_errors\""));
    CHECK(has(text, "Block #1  $.source_code"));
    CHECK(!has(text, "Fehlerbericht"));

    // fix_report that is not an object: JSON layout as well
    const std::string other = pdfText(renderReport({{"fix_report", "text"}}));
    CHECK(has(other, "\"fix_report\""));
}

TEST_CASE("render: broken fix_report content does not stop the report") {
    const json broken = {{"fix_report", {{"summary", "x"}, {"fixes", {1, "two", {{"id", "a"}, {"attempts", "x"}, {"lines", {"a", 3}}},
                                                                    {{"attempts", {{{"snippet", 5}, {"review", 1}}}}}}}}}};
    const std::string pdf = renderReport(broken);
    CHECK(pdf.rfind("%PDF-1.4", 0) == 0);
    CHECK(has(pdfText(pdf), "2buggAI Fehlerbericht"));

    const std::string empty = pdfText(renderReport({{"fix_report", json::object()}}));
    CHECK(has(empty, "Keine Fehler gefunden."));
}

TEST_CASE("render: a short code block is not split across pages") {
    // many sections with code blocks of a few lines: no block may start on one page and end
    // on the next one
    json fixes = json::array();
    for (int i = 1; i <= 12; ++i) {
        json lines = json::array();
        for (int l = 1; l <= 9; ++l) lines.push_back({{"line", l}, {"text", "code " + std::to_string(i) + "." + std::to_string(l)}, {"mark", l == 5}});
        fixes.push_back({{"id", i}, {"title", "e" + std::to_string(i)}, {"status", "fixed"}, {"category", "c"},
                         {"attempts", {{{"number", 1}, {"applied", true}, {"error_gone", true},
                                        {"snippet", {{"hunks", {{{"old", lines}, {"new", lines}}}}}}}}}});
    }
    const std::string text = pdfText(renderReport({{"fix_report", {{"fixes", fixes}}}}));
    for (int i = 1; i <= 12; ++i) {
        const std::string first = "code " + std::to_string(i) + ".1\n", last = "code " + std::to_string(i) + ".9\n";
        const size_t a = text.find(first), b = text.find(last, a == std::string::npos ? 0 : a);
        REQUIRE(a != std::string::npos);
        REQUIRE(b != std::string::npos);
        CHECK(text.substr(a, b - a).find('\f') == std::string::npos);
    }
    // every page break leaves the heading with its block: "FEHLERHAFTER CODE" never ends a page
    CHECK(text.find("FEHLERHAFTER CODE  (, vor dem Fix)\n\f") == std::string::npos);
}

TEST_CASE("render: long code lines wrap and keep their background") {
    TempProject project;
    json report = sampleFixReport(project);
    report["fixes"][0]["attempts"][0]["snippet"]["hunks"][0]["new"][0]["text"] = std::string(250, 'x');
    const std::string pdf = renderReport({{"fix_report", report}});
    const std::string text = pdfText(pdf);
    CHECK(has(text, std::string(99, 'x')));
    CHECK(!has(text, std::string(108, 'x')));        // no row is wider than the page
}
