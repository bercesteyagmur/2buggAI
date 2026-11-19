#ifndef INC_2BUGGAI_STACKCONTEXT_H
#define INC_2BUGGAI_STACKCONTEXT_H

#include <string>
#include <vector>
#include "StackDetector.h"
#include "WebPipeline.h"   // CodeError, FixContext

// Context selection for a fix request.
//
// 1. Code under test: when the error is located in a test file, the project modules that
//    the test imports are added (the defect is usually there, not in the test).
//
// 2. Cross-component context:
// When an error is reported in one component (e.g. a TypeScript type error in the
// frontend) its cause is often on the other side of a boundary (the backend renamed an
// API field, a migration and an ORM model disagree). select() finds the files of the
// neighbouring components that mention the same names as the error message and returns
// them within a fixed size budget.
class StackContext {
public:
    static constexpr size_t MAX_RELATED_FILES = 3;
    static constexpr size_t MAX_FILE_CHARS = 12000;    // per related file sent in full
    static constexpr size_t MAX_TOTAL_CHARS = 24000;   // all related files together
    static constexpr size_t MAX_EXCERPT_CHARS = 3000;  // read-only excerpt of a larger file

    // projectFiles and relevantFiles are project-relative paths.
    static FixContext select(const ProjectStack& stack,
                             const std::vector<std::string>& projectFiles,
                             const std::vector<std::string>& relevantFiles,
                             const std::vector<CodeError>& errors);

    // Names mentioned in an error message that are worth searching for
    // (quoted names, snake_case / camelCase words, "no such column: x").
    static std::vector<std::string> identifiers(const std::string& message);

    // The identifier itself plus its snake_case / camelCase counterpart.
    static std::vector<std::string> nameVariants(const std::string& identifier);

    // Files that must never be sent to a model (.env, keys, credentials).
    static bool isSensitiveFile(const std::string& path);
};

#endif //INC_2BUGGAI_STACKCONTEXT_H
