#ifndef INC_2BUGGAI_FIXRECORD_H
#define INC_2BUGGAI_FIXRECORD_H

#include <functional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

#include "ChecklistReader.h"
#include "FixSnippet.h"
#include "OpenAiClient.h"

// Where and what an error is, as far as the tool output tells (see FixReportBuilder::Describer).
struct ErrorInfo {
    std::string file;              // project-relative, empty when unknown
    std::vector<int> lines;        // 1-based lines in that file
    std::string language;
    std::string tool;              // "Compiler (g++)", "pytest", "tsc", ...
    std::string severity;          // "error" | "warning" | ...
    std::string message;           // shortened error message (the relevant lines)
};

// One run of the fix loop for an error: agent answer, applied or not, result of the re-run.
struct FixAttempt {
    int number = 0;                         // 1-based
    std::string file;                       // file of the (proposed) fix
    bool applied = false;                   // written to the file
    bool errorGone = false;                 // the real re-run no longer shows the error
    std::string outcome;                    // fixed | still_present | rejected | no_fix | apply_failed
    std::string reason;                     // why the attempt did not fix the error
    FixExplanation explanation;
    ReviewOutcome review;
    VerifyOutcome verification;
    bool verificationExpected = false;      // agent pipeline active (Reviewer + Verifier)
    std::vector<std::string> remainingErrors;   // after the re-run (only for applied attempts)
    std::vector<std::string> failedChecks;      // check steps that failed after the re-run
    Snippet snippet;                        // faulty / fixed code (precomputed with context_lines)
    std::string diff;                       // unified diff of the (proposed) fix
};

// Everything the report shows for one error (one section).
struct FixRecord {
    int id = 0;                       // section number, 1-based
    std::string errorName;            // checklist category that was detected
    std::string difficulty;           // leicht | mittel | schwer
    std::string title;
    ErrorInfo info;
    Snippet excerpt;                  // faulty lines of the original file (without a fix)
    std::string status = "not_fixed"; // fixed | not_fixed
    std::string statusReason;
    int resolvedBy = 0;               // fixed together with the fix of record #n
    std::vector<FixAttempt> attempts; // iterations = attempts.size()
};

// Collects one FixRecord per error while the fix loop runs and turns them into the
// "fix_report" object of the JSON report (rendered by PdfReportExporter).
//
// Every method catches its own exceptions and logs "[REPORT] ...": the report must never
// stop a run.
class FixReportBuilder {
public:
    using Describer = std::function<ErrorInfo(const std::string& errorName)>;

    // projectRoot: base of the project-relative paths (folder of a single-file target).
    // contextLines: unchanged lines around a change in the code excerpts.
    FixReportBuilder(std::string projectRoot, int contextLines, std::string language = "de");

    // Called for new error names (from beginError / skipError / observe) while the error
    // output that mentions them is current.
    void setDescriber(Describer describer) { describer_ = std::move(describer); }
    void setPipeline(bool pipeline) { pipeline_ = pipeline; }

    // Records the errors that are currently detected. Names that were seen before and are
    // gone now are noted as resolved by the record `causedBy` (0 = initial detection).
    void observe(const std::vector<std::string>& errors, int causedBy = 0) noexcept;

    // Starts the section of an error the loop works on; returns its id (0 on failure).
    int beginError(const std::string& errorName) noexcept;

    // One attempt for record `id`. originalCode is the file content before the attempt;
    // when empty it is read from disk (the attempt was not applied, the file is unchanged).
    void addAttempt(int id, int number, const FixResult& result, const std::string& originalCode,
                    bool applied, bool errorGone, const std::vector<std::string>& remainingErrors,
                    const std::vector<std::string>& failedChecks, const std::string& applyError = "",
                    const std::string& stillPresentReason = "") noexcept;

    void finishError(int id, bool fixed, const std::string& reason = "") noexcept;

    // An error the loop skipped without a fix attempt (environment problem, no file, ...).
    int skipError(const std::string& errorName, const std::string& reason) noexcept;

    // End of the loop: every error that was seen but has no section of its own gets one
    // (fixed together with another fix, or not handled), and records that were given up
    // but are gone in finalErrors are marked as fixed by the later fix.
    // An empty unhandledReason uses the default text ("not handled, round limit reached").
    void finalize(const std::vector<std::string>& finalErrors, const std::string& unhandledReason = "") noexcept;

    // The "fix_report" object. meta is merged into it (project, target, models, ...).
    nlohmann::json toJson(const nlohmann::json& meta = nlohmann::json::object()) const noexcept;

    const std::vector<FixRecord>& records() const { return records_; }

    // Text in the report language (report.language): German unless it is "en".
    std::string tr(const char* german, const char* english) const { return language_ == "en" ? english : german; }
    std::string tr(const std::string& german, const std::string& english) const { return language_ == "en" ? english : german; }

    // ---- building blocks ----

    // Keywords of a checklist category (lower case, without "-").
    static std::vector<std::string> keywordsFor(const std::string& errorName, const std::vector<ErrorCategory>& checklist);

    // The relevant lines of a tool output for an error: lines with a keyword, the source/caret
    // lines after a compiler message and the context above it ("file: In function ...",
    // traceback frames of the file). Without a keyword hit: the lines that name the file and
    // exception lines. Gaps are shown as "...". Falls back to the first lines of the output.
    // stripPrefix (e.g. "/abs/project/") is removed from every line to keep paths short.
    static std::string excerptMessage(const std::string& output, const std::vector<std::string>& keywords,
                                      const std::string& file, size_t maxLines = 14, const std::string& stripPrefix = "");

    // "ImportError in backend/app.py": the exception name from the message when there is
    // one, otherwise the category.
    static std::string makeTitle(const std::string& errorName, const std::string& file, const std::string& message);

    static nlohmann::json recordToJson(const FixRecord& record);

private:
    FixRecord* find(int id);
    ErrorInfo describe(const std::string& errorName) const;
    FixRecord& newRecord(const std::string& errorName, const ErrorInfo& info);
    std::string readFile(const std::string& relative) const;

    std::string root_;
    int context_;
    std::string language_;
    bool pipeline_ = false;
    Describer describer_;
    std::vector<FixRecord> records_;

    struct Seen {
        std::string name;
        ErrorInfo info;
        int resolvedBy = 0;
    };
    std::vector<Seen> seen_;                 // every error name seen during the run, in order
    std::vector<std::string> lastObserved_;
};

#endif //INC_2BUGGAI_FIXRECORD_H
