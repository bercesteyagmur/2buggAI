#ifndef INC_2BUGGAI_WEBPIPELINE_H
#define INC_2BUGGAI_WEBPIPELINE_H

#include <string>
#include <vector>
#include "ChecklistReader.h"
#include "WebEnvironmentManager.h"

struct CodeError {
    std::string file;
    int line = 0;
    std::string message;
    std::string step;
};

struct WebPipelineOptions {
    std::string targetPath;
    std::string fixDescription;
    std::string jsonOutFile;
    bool runtime = false;
};

class WebPipeline {
public:
    static int run(const WebPipelineOptions& options, const std::vector<std::string>& files);

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
