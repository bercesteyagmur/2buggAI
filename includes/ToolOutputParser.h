#ifndef INC_2BUGGAI_TOOLOUTPUTPARSER_H
#define INC_2BUGGAI_TOOLOUTPUTPARSER_H

#include <string>
#include <vector>
#include "WebPipeline.h"   // CodeError

// Turns the output of one tool into structured CodeError entries.
//
// parse() is the entry point: it picks the parser registered for the tool, resolves the
// reported paths against the component root and returns only files that exist inside
// the project (no node_modules, virtualenvs or system files), relative to the project root.
//
// The parseXxx functions do the text work only and return the paths exactly as printed.
class ToolOutputParser {
public:
    static std::vector<CodeError> parse(const std::string& tool,
                                        const std::string& output,
                                        const std::string& componentRoot,
                                        const std::string& projectRoot);

    // Name of the parser used for a tool ("python", "pytest", "lint", "tsc", "eslint",
    // "jest", "sql", "sqlfluff") or "generic" for unknown tools.
    static std::string parserFor(const std::string& tool);

    static std::vector<CodeError> parsePython(const std::string& output);     // tracebacks, compileall
    static std::vector<CodeError> parsePytest(const std::string& output);
    static std::vector<CodeError> parseLint(const std::string& output);       // ruff, flake8, mypy
    static std::vector<CodeError> parseTsc(const std::string& output);
    static std::vector<CodeError> parseEslint(const std::string& output);
    static std::vector<CodeError> parseJest(const std::string& output);       // jest, vitest
    static std::vector<CodeError> parseSql(const std::string& output);        // SQL dry run, alembic
    static std::vector<CodeError> parseSqlfluff(const std::string& output);
    static std::vector<CodeError> parseGeneric(const std::string& output);    // custom commands
};

#endif //INC_2BUGGAI_TOOLOUTPUTPARSER_H
