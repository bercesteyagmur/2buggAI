#ifndef INC_2BUGGAI_PDFREPORTEXPORTER_H
#define INC_2BUGGAI_PDFREPORTEXPORTER_H

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

// Settings of the PDF export. Defaults < <project>/.buggai.json ("pdf_export") < CLI flags.
struct PdfExportConfig {
    bool enabled = true;         // pdf_export.enabled        / --no-pdf
    bool combined = true;        // pdf_export.combined       / --no-pdf-combined
    bool perIteration = false;   // pdf_export.per_iteration  / --pdf-per-iteration
    std::string outputDir;       // pdf_export.output_dir     / --pdf-dir   (empty: next to the JSON)

    // Layout of reports with fix sections (see docs/PLAN_REPORT_REDESIGN.md), ".buggai.json" -> "report"
    int contextLines = 3;                 // report.context_lines: unchanged lines around a change (0..20)
    bool showDiff = true;                 // report.show_diff: unified diff in every fix section
    bool includeRawJsonAppendix = false;  // report.include_raw_json_appendix: raw JSON at the end
    std::string language = "de";          // report.language: "de" or "en" (labels and model explanations)
};

// Writes a readable PDF for every JSON report of a run, without any external tool or
// library (see docs/PLAN_PDF_EXPORT.md).
//
// Usage: create one exporter per run, call exportJson() wherever a JSON report is
// written and exportIteration() after each applied fix. finish() (also called by the
// destructor) writes the combined report of the run.
//
// The hooks never throw: a failing export is logged and the run continues.
class PdfReportExporter {
public:
    // A multi-line string that was moved out of the JSON into its own code block.
    struct TextBlock {
        int number = 0;       // 1-based, as used in "[block #1: ...]"
        std::string path;     // JSON path, e.g. $.steps[0].output
        std::string text;
        int lineCount = 0;
    };

    // One JSON file inside a PDF.
    struct Document {
        std::string name;      // shown in the page header
        std::string jsonText;  // raw file content (may be invalid JSON)
    };

    PdfReportExporter();
    explicit PdfReportExporter(PdfExportConfig config);
    ~PdfReportExporter();
    PdfReportExporter(const PdfReportExporter&) = delete;
    PdfReportExporter& operator=(const PdfReportExporter&) = delete;

    // ---- configuration ----

    // Reads "pdf_export" and "report" from the text of a .buggai.json. Unknown, missing or
    // invalid keys keep their defaults; `error` is set when the text is not a JSON object.
    static PdfExportConfig parseConfig(const std::string& buggaiJsonText, std::string* error = nullptr);

    // Config of the analysed project: <target>/.buggai.json, or next to a single file.
    // A relative output_dir is resolved against that folder.
    static PdfExportConfig loadConfig(const std::string& targetPath);

    // ---- hooks (never throw) ----

    // PDF for one JSON report. jsonPath is the JSON file that was written; when it is
    // empty (no --json-out) a PDF is only written if an output folder is configured and
    // is then named <fallbackName>.pdf. Returns true when a PDF was written.
    bool exportJson(const std::string& jsonText, const std::string& jsonPath,
                    const std::string& fallbackName = "report") noexcept;

    // PDF for the state after one fix iteration (<name>.iterNN.pdf). Only with
    // perIteration. No JSON file is written for it.
    bool exportIteration(int iteration, const nlohmann::ordered_json& data, const std::string& jsonPath,
                         const std::string& fallbackName = "report") noexcept;

    // Combined report of everything exported so far (<name>.combined.pdf). Once per run.
    bool finish() noexcept;

    const PdfExportConfig& config() const { return config_; }
    const std::string& runId() const { return runId_; }
    const std::vector<std::string>& writtenFiles() const { return written_; }

    // ---- building blocks ----

    // Keys whose values must not appear in a report (api_key, token, authorization, ...).
    static bool isSensitiveKey(const std::string& key);

    // Masks key-like values inside free text: "sk-..." keys and "Bearer <token>".
    static std::string maskText(const std::string& text);

    // Masks string literals assigned to secret-like names in source code, line by line:
    //   api_key = "abc"  ->  api_key = "***",   "password": 'x'  ->  "password": '***'
    // Comparisons (==) and names without a literal value are left alone.
    static std::string maskCodeText(const std::string& text);

    // Copy of the value with all sensitive fields replaced by "***" and all strings
    // passed through maskText and maskCodeText.
    static nlohmann::ordered_json maskSecrets(const nlohmann::ordered_json& value);

    // Copy of the value in which every string with two or more lines is replaced by
    // blockReference(); the original texts are appended to `blocks` with their JSON path.
    static nlohmann::ordered_json extractBlocks(const nlohmann::ordered_json& value, std::vector<TextBlock>& blocks);

    // Top-level fields that hold the fix recommendations of the run (the final AI analysis:
    // "ai_analysis", "fix_recommendations", "recommendations"). Their text is not listed
    // as an ordinary block: it gets its own "Fix recommendations" text field at the top
    // of the document, before the JSON.
    static bool isRecommendationKey(const std::string& key);

    // "[block #1: 12 lines] <start of the first non-empty line>..."
    static std::string blockReference(int number, const std::string& text);

    // The complete PDF file as bytes. Every document starts on a new page. A non-empty
    // coverTitle adds a first page that lists the documents.
    //
    // A document whose JSON has a "fix_report" object (FixReportBuilder) is shown as a fix
    // report: header, summary table, one section per error (faulty code, root cause, fixed
    // code, diff, explanation, result), the final model analysis and - with
    // includeRawJsonAppendix - the raw JSON as an appendix. Any other JSON is shown as
    // before (coloured JSON plus its multi-line strings as numbered blocks).
    static std::string render(const std::vector<Document>& documents, const std::string& runId,
                              const std::string& timestamp, const std::string& coverTitle = "",
                              const PdfExportConfig& layout = PdfExportConfig{});

private:
    std::string targetFor(const std::string& jsonPath, const std::string& fallbackName,
                          const std::string& suffix, std::string& displayName) const;
    bool write(const std::string& path, const std::string& pdf);

    PdfExportConfig config_;
    std::string runId_;
    std::string timestamp_;
    std::string combinedPath_;            // set by the first exportJson()
    std::vector<Document> reports_;       // JSON reports, in export order
    std::vector<Document> iterations_;    // per-iteration snapshots
    std::vector<std::string> written_;
    bool finished_ = false;
};

#endif //INC_2BUGGAI_PDFREPORTEXPORTER_H
