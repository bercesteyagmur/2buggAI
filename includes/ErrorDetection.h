#pragma once
#include <set>
#include <string>
#include <vector>

#include "ChecklistReader.h"

// Turns build / program output into the errors the fix loop works on.
//
//  - checklist categories (detect_from_output), as before
//  - a fallback when the build failed or the program crashed but no category matched, so an
//    unknown failure is never reported as "no errors"
//  - environment problems (detection type "environment": database not reachable, port in use,
//    unknown host, missing command, ...). They are not code bugs: errors that only show up in
//    the same part of the output (build or run) are marked as caused by the environment, and the
//    fix loop does not try to fix them in code.
namespace ErrorDetection {

inline const char* const kUnrecognizedBuild   = "unrecognized_build_failure";
inline const char* const kUnrecognizedRuntime = "unrecognized_runtime_failure";

struct Result {
    std::vector<std::string> errors;            // sorted easy -> hard, catch-alls last
    std::vector<std::string> environment;       // environment problems found (category names)
    std::set<std::string> environmentCaused;    // errors that only appear next to such a problem
};

// buildOutput: compiler / build tool output; buildFailed: the build did not produce a program.
// runOutput:   program, debugger and Valgrind output; runFailed: the program crashed, exited
//              with an error code or its tests failed (a timeout is not a failure).
Result detect(const std::vector<ErrorCategory>& checklist, const std::string& language,
              const std::string& buildOutput, bool buildFailed,
              const std::string& runOutput, bool runFailed);

// Environment problems found in the output (names of "environment" categories).
std::vector<std::string> environmentProblems(const std::vector<ErrorCategory>& checklist,
                                             const std::string& output, const std::string& language);

// One line for the user: what is wrong with the environment and what to do.
std::string environmentHint(const std::string& name);

} // namespace ErrorDetection
