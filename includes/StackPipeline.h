#ifndef INC_2BUGGAI_STACKPIPELINE_H
#define INC_2BUGGAI_STACKPIPELINE_H

#include <string>
#include <vector>
#include "StackDetector.h"
#include "WebPipeline.h"

// Fullstack / multi-service projects (Python backend, JS/TS frontend, SQL, services).
// Provides the stack specific hooks and runs the shared loop of WebPipeline.
class StackPipeline {
public:
    static int run(const WebPipelineOptions& options, const std::vector<std::string>& files,
                   const ProjectStack& stack);

    // Splits the collected output ("===== STEP: <component>: <tool> | ..." sections) and
    // parses every section with the parser of its tool. Sets step, tool, service and
    // component of each error.
    static std::vector<CodeError> parseErrors(const std::string& errorOutput, const ProjectStack& stack);

    static void printStack(const ProjectStack& stack);
};

#endif //INC_2BUGGAI_STACKPIPELINE_H
