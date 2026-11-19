#ifndef INC_2BUGGAI_WEBPIPELINE_H
#define INC_2BUGGAI_WEBPIPELINE_H

#include <functional>
#include <set>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "ChecklistReader.h"
#include "WebEnvironmentManager.h"

struct CodeError {
    std::string file;
    int line = 0;
    std::string message;
    std::string step;

    // Filled by ToolOutputParser (fullstack projects); empty/0 for the generic PHP/JS parser.
    int column = 0;
    std::string severity;    // "error" | "warning"
    std::string tool;        // "pytest", "tsc", "eslint", "sql", ...
    std::string service;     // docker-compose service / services/<name>
    std::string component;   // StackComponent::name
};

// Extra context for one fix request (see StackContext).
struct FixContext {
    std::vector<std::string> files;   // project-relative files added as "===== FILE:" blocks
    std::string note;                 // becomes FixRequest::cross_context
};

// The project-type specific parts of the fix loop. WebPipeline::run fills them with the
// PHP/JS implementation, StackPipeline::run with the fullstack implementation.
struct PipelineHooks {
    std::set<std::string> languages;   // checklist languages to match against

    std::function<std::vector<SetupStep>()> setup;

    // changedFile: project-relative path of the file that was just fixed, empty for the
    // initial run. Always returns the complete, current list of check steps.
    std::function<std::vector<SetupStep>(const std::string& changedFile)> check;

    std::function<std::vector<CodeError>(const std::string& errorOutput)> parseErrors;

    // Optional: related files of other components for an error.
    std::function<FixContext(const std::string& errorName,
                             const std::vector<std::string>& relevantFiles,
                             const std::vector<CodeError>& errors)> context;

    // Optional: add project-type specific data to the JSON report.
    std::function<void(nlohmann::json& report)> extendReport;
};

class PdfReportExporter;

struct WebPipelineOptions {
    std::string targetPath;
    std::string fixDescription;
    std::string jsonOutFile;
    bool runtime = false;
    PdfReportExporter* pdf = nullptr;   // optional: PDF export of the JSON report
};

class WebPipeline {
public:
    static int run(const WebPipelineOptions& options, const std::vector<std::string>& files);

    // The shared detect -> fix -> re-check loop.
    static int runWithHooks(const WebPipelineOptions& options, const PipelineHooks& hooks);

    static std::vector<CodeError> parseErrors(const std::string& output, const std::string& projectPath);

    static std::string collectErrorOutput(const std::vector<SetupStep>& steps);

    static std::vector<std::string> filesForError(const std::string& errorName,
                                                  const std::vector<CodeError>& errors,
                                                  const std::vector<ErrorCategory>& checklist,
                                                  const std::string& errorOutput);

    static std::string environmentCause(const std::string& errorName,
                                        const std::vector<ErrorCategory>& checklist,
                                        const std::string& errorOutput);

    static bool isEnvironmentError(const std::string& errorName);
};

#endif //INC_2BUGGAI_WEBPIPELINE_H
