#pragma once
#include <string>
#include <vector>
#include "LlmProvider.h"

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
};

struct FixResult {
    std::string fixed_code;
    std::string file_path;
    std::vector<std::string> new_errors;  // direkt in detectedErrors push_back-bar
    bool success = false;                  // hat die AI den Fehler gefixt?
    bool is_confident = false;             // ist die AI sicher dass alles ok ist?
};

class OpenAIClient {
public:
    OpenAIClient();
    OpenAIResult debug_report(const std::string& report_json) const;
    FixResult fix_code(const FixRequest& req) const;

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
