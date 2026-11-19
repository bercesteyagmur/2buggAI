#pragma once
#include <string>
#include <vector>
#include "LlmProvider.h"
#include <nlohmann/json.hpp>

struct OpenAIResult {
    long http_status = 0;
    std::string raw_json;   // komplette API-Antwort (oder Fehler)
    std::string text;       // extrahierter Text (wenn vorhanden)
};

struct FixRequest {
    std::string error_name;      // z.B. "segmentation_fault"
    std::string error_output;    // roher Compiler/GDB Output
    std::string language;
    std::string source_code;
    std::string checklist;
    // Optional. Describes a component boundary (fullstack projects): which component the
    // error is in and which files/excerpts of other components are related. When empty,
    // all agent prompts are identical to a request without this field.
    std::string cross_context;
    // Optional. Language of the explanation fields for the report ("German", "English").
    // When empty, the prompt does not mention a language for them.
    std::string explanation_language;
    // Optional. Earlier attempts for the same error that did not remove it (see
    // FixHistory::describeAttempt), so the model does not repeat a fix that failed.
    std::vector<std::string> previous_attempts;
};

// Explanation fields of the Debugger answer (for the report). Every field may be empty:
// a model that leaves them out still produces a valid fix.
struct FixExplanation {
    std::string root_cause;            // root_cause_explanation: why this is an error
    std::string fix;                   // fix_explanation: what was changed and why it works
    std::vector<int> affected_lines;   // faulty lines in the ORIGINAL file (1-based)
    std::string error_category;        // category named by the model
    std::string side_effects;
};

struct ReviewOutcome {
    bool ran = false;                  // a valid review was received
    bool approved = false;
    std::string verdict;               // APPROVE | CHANGES_REQUESTED
    std::string comment;               // review_comment, falls back to summary
    std::vector<std::string> findings; // "[severity] file: problem"
    bool revised = false;              // the Debugger revised its fix once after CHANGES_REQUESTED
};

struct VerifyOutcome {
    bool ran = false;                  // a valid verdict was received
    bool confirmed = false;
    std::string reason;
    std::vector<std::string> remaining_issues;
};

struct FixResult {
    std::string fixed_code;
    std::string file_path;
    std::vector<std::string> new_errors;  // direkt in detectedErrors push_back-bar
    bool success = false;                  // hat die AI den Fehler gefixt?
    bool is_confident = false;             // ist die AI sicher dass alles ok ist?

    // For the report. When success is false, file_path / fixed_code may still hold the
    // rejected proposal and failure_reason says why it was not accepted.
    FixExplanation explanation;
    ReviewOutcome review;
    VerifyOutcome verification;
    std::string failure_reason;
};

// Parses the explanation fields of a Debugger answer (raw model text, JSON may be wrapped in
// prose or ```json fences). Missing or broken fields stay empty; never throws.
FixExplanation parse_fix_explanation(const std::string& agentText);

// Parses a Reviewer answer. ran is false when the answer has neither a valid verdict nor
// an "approved" flag. Never throws.
ReviewOutcome parse_review_outcome(const std::string& agentText);

class OpenAIClient {
public:
    OpenAIClient();
    OpenAIResult debug_report(const std::string& report_json) const;
    FixResult fix_code(const FixRequest& req) const;

    // Provider/model per role for the report header, e.g. {"mode": "pipeline",
    // "debugger": "gemini/gemini-2.5-pro", ...}. Never contains keys; never throws.
    static nlohmann::json describeModels();

private:
    FixResult fix_code_single(const FixRequest& req) const;
    FixResult fix_code_pipeline(const FixRequest& req) const;

    LlmProvider report_;     // debug_report (LLM_* / OPENAI_*)
    LlmProvider fixer_;      // fix_code ohne Pipeline (LLM_* / OPENAI_*)

    bool pipeline_ = false;  // Debugger -> Reviewer -> Verifier
    LlmProvider debugger_;
    LlmProvider reviewer_;
    LlmProvider verifier_;
};
