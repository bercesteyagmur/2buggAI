#include "ToolOutputParser.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <regex>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace {

constexpr size_t MAX_MESSAGE = 400;
constexpr size_t MAX_FRAMES_PER_ERROR = 4;   // project frames reported per traceback

std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    size_t end = s.find_last_not_of(" \t\r\n");
    return start == std::string::npos ? "" : s.substr(start, end - start + 1);
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

CodeError makeError(const std::string& file, int line, int column,
                    const std::string& message, const std::string& severity = "error") {
    CodeError e;
    e.file = file;
    e.line = line;
    e.column = column;
    e.message = message.size() > MAX_MESSAGE ? message.substr(0, MAX_MESSAGE) : message;
    e.severity = severity;
    return e;
}

int toInt(const std::string& s) {
    try { return std::stoi(s); } catch (...) { return 0; }
}

struct Frame {
    std::string file;
    int line = 0;
};

// A traceback lists frames from the outermost call to the place of the error. The last
// frame is the most likely place of the bug, so frames are emitted innermost first.
void emitFrames(const std::vector<Frame>& frames, const std::string& message, std::vector<CodeError>& out) {
    for (auto it = frames.rbegin(); it != frames.rend(); ++it) {
        out.push_back(makeError(it->file, it->line, 0, message));
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Python: tracebacks and compileall
// ---------------------------------------------------------------------------
//
//   Traceback (most recent call last):
//     File "/abs/app/main.py", line 3, in <module>        <- frame
//       from app.modles import USERS
//   ModuleNotFoundError: No module named 'app.modles'     <- exception line ends the block
//
//   *** Error compiling 'app/broken.py'...
//     File "app/broken.py", line 1                        <- SyntaxError frame has no ", in"
//       def f(:
//             ^
//   SyntaxError: invalid syntax
std::vector<CodeError> ToolOutputParser::parsePython(const std::string& output) {
    // File "<path>", line <n>[, in <function>]
    static const std::regex frameRe(R"re(^\s*File "(.+?)", line (\d+)(?:, in .*)?$)re");
    // First unindented "Name:" / "pkg.Name: message" line after the frames, e.g.
    // "KeyError: 'x'" or "sqlalchemy.exc.OperationalError: ...". Lines such as
    // "During handling of ..." or "Traceback (most ..." do not match because a
    // space follows the first word.
    static const std::regex exceptionRe(R"re(^[A-Za-z_][\w.]*(:.*)?$)re");

    std::vector<CodeError> errors;
    std::vector<Frame> frames;

    for (const auto& line : splitLines(output)) {
        std::smatch m;
        if (std::regex_match(line, m, frameRe)) {
            frames.push_back({m[1].str(), toInt(m[2].str())});
            continue;
        }
        if (!frames.empty() && std::regex_match(line, exceptionRe)) {
            emitFrames(frames, line, errors);
            frames.clear();
        }
    }
    return errors;
}

// ---------------------------------------------------------------------------
// pytest (--tb=short and the default long format)
// ---------------------------------------------------------------------------
//
//   ______ ERROR collecting tests/test_api.py ______      <- section start
//   tests/test_api.py:1: in <module>                      <- frame (short format)
//       from app.main import create_app
//   app/main.py:3: in <module>
//   E   ModuleNotFoundError: No module named 'app.modles' <- first "E" line = message
//   ______ test_slugify ______
//   >       assert slugify("Hello World") == "hello-world"
//   E       AssertionError: assert 'hello_world' == 'hello-world'
//   tests/test_utils.py:5: AssertionError                 <- frame (long format)
//   ====== short test summary info ======
//   FAILED tests/test_utils.py::test_slugify - AssertionErr...
std::vector<CodeError> ToolOutputParser::parsePytest(const std::string& output) {
    static const std::regex sectionRe(R"re(^(_{3,}|={3,}) .* (_{3,}|={3,})$)re");
    static const std::regex frameRe(R"re(^(\S.*?\.py):(\d+): (?:in .+|[A-Za-z_][\w.]*)$)re");
    static const std::regex messageRe(R"re(^E\s+(\S.*)$)re");
    static const std::regex summaryRe(R"re(^(FAILED|ERROR) (\S+?\.py)(?:::\S+)?(?: - (.*))?$)re");

    std::vector<CodeError> errors;
    std::vector<Frame> frames;
    std::string message;
    std::vector<CodeError> summary;

    auto flush = [&]() {
        if (!frames.empty() && !message.empty()) emitFrames(frames, message, errors);
        frames.clear();
        message.clear();
    };

    for (const auto& line : splitLines(output)) {
        std::smatch m;
        if (std::regex_match(line, sectionRe)) {
            flush();
        } else if (std::regex_match(line, m, frameRe)) {
            frames.push_back({m[1].str(), toInt(m[2].str())});
        } else if (message.empty() && std::regex_match(line, m, messageRe)) {
            message = m[1].str();
        } else if (std::regex_match(line, m, summaryRe)) {
            summary.push_back(makeError(m[2].str(), 0, 0,
                m[1].str() + " " + m[2].str() + (m[3].matched ? " - " + m[3].str() : "")));
        }
    }
    flush();

    // The summary has no line numbers; it is only used when no traceback was printed
    // (e.g. --tb=no or a custom command).
    return errors.empty() ? summary : errors;
}

// ---------------------------------------------------------------------------
// ruff / flake8 / mypy
// ---------------------------------------------------------------------------
//
//   app/lint_me.py:5:26: F821 undefined name 'undefined_bonus'          (flake8, ruff concise)
//   app/lint_me.py:9: error: Incompatible return value type ...  [return-value]   (mypy)
std::vector<CodeError> ToolOutputParser::parseLint(const std::string& output) {
    // <path>:<line>:[<col>:] <rest>   The path must end in an extension so that
    // lines like "note: 12:3: ..." are not mistaken for a location.
    static const std::regex lineRe(R"re(^(\S.*?\.\w+):(\d+):(?:(\d+):)? (.+)$)re");

    std::vector<CodeError> errors;
    for (const auto& line : splitLines(output)) {
        std::smatch m;
        if (!std::regex_match(line, m, lineRe)) continue;

        std::string message = m[4].str();
        std::string severity = "error";
        if (message.rfind("note:", 0) == 0) continue;               // mypy hints belong to the line above
        if (message.rfind("warning:", 0) == 0 || message[0] == 'W') severity = "warning";   // W605 etc.

        errors.push_back(makeError(m[1].str(), toInt(m[2].str()), m[3].matched ? toInt(m[3].str()) : 0,
                                   message, severity));
    }
    return errors;
}

// ---------------------------------------------------------------------------
// TypeScript compiler
// ---------------------------------------------------------------------------
//
//   src/format.ts(4,18): error TS2339: Property 'name' does not exist on type 'User'.   (--pretty false)
//   src/format.ts:4:18 - error TS2339: Property 'name' does not exist on type 'User'.   (--pretty)
std::vector<CodeError> ToolOutputParser::parseTsc(const std::string& output) {
    static const std::regex plainRe(R"re(^(\S.*?)\((\d+),(\d+)\): (error|warning) (TS\d+: .*)$)re");
    static const std::regex prettyRe(R"re(^(\S.*?):(\d+):(\d+) - (error|warning) (TS\d+: .*)$)re");

    std::vector<CodeError> errors;
    for (const auto& line : splitLines(output)) {
        std::smatch m;
        if (!std::regex_match(line, m, plainRe) && !std::regex_match(line, m, prettyRe)) continue;
        // keep "error TS2339: ..." in the message: the checklist matches on the TS code
        errors.push_back(makeError(m[1].str(), toInt(m[2].str()), toInt(m[3].str()),
                                   m[4].str() + " " + m[5].str(), m[4].str()));
    }
    return errors;
}

// ---------------------------------------------------------------------------
// ESLint
// ---------------------------------------------------------------------------
//
// "stylish" (default):
//   /abs/frontend/src/app.js                 <- unindented path line
//     12:5  error  'foo' is not defined  no-undef
//   ✖ 1 problem (1 error, 0 warnings)
//
// "unix":
//   /abs/frontend/src/app.js:12:5: 'foo' is not defined. [Error/no-undef]
std::vector<CodeError> ToolOutputParser::parseEslint(const std::string& output) {
    // <line>:<col>  error|warning  <message>  [<rule>]   (rule separated by 2+ spaces)
    static const std::regex itemRe(R"re(^\s+(\d+):(\d+)\s+(error|warning)\s+(.*?)(?:\s{2,}(\S+))?\s*$)re");
    static const std::regex unixRe(R"re(^(\S.*?):(\d+):(\d+): (.*) \[(Error|Warning)(?:/(.+))?\]$)re");

    std::vector<CodeError> errors;
    std::string currentFile;

    for (const auto& line : splitLines(output)) {
        std::smatch m;
        if (std::regex_match(line, m, unixRe)) {
            const std::string rule = m[6].matched ? " (" + m[6].str() + ")" : "";
            errors.push_back(makeError(m[1].str(), toInt(m[2].str()), toInt(m[3].str()),
                                       m[4].str() + rule, m[5].str() == "Error" ? "error" : "warning"));
        } else if (std::regex_match(line, m, itemRe)) {
            if (currentFile.empty()) continue;
            const std::string rule = m[5].matched ? " (" + m[5].str() + ")" : "";
            errors.push_back(makeError(currentFile, toInt(m[1].str()), toInt(m[2].str()),
                                       m[4].str() + rule, m[3].str()));
        } else if (!line.empty() && line[0] != ' ' && line[0] != '\t') {
            // any unindented line can be the path header of the next block
            currentFile = trim(line);
        }
    }
    return errors;
}

// ---------------------------------------------------------------------------
// Jest / Vitest
// ---------------------------------------------------------------------------
//
// jest:
//   FAIL src/sum.test.js
//     ● sum › adds numbers                          <- test title
//       expect(received).toBe(expected)             <- first text line after the title
//         at Object.<anonymous> (src/sum.test.js:4:21)
//
// vitest:
//    FAIL  src/format.test.ts > formatUser shows the full name
//   AssertionError: expected 'undefined <a@b>' to be 'Ada <a@b>'
//    ❯ src/format.test.ts:6:28
std::vector<CodeError> ToolOutputParser::parseJest(const std::string& output) {
    static const std::regex failRe(R"re(^\s*FAIL\s+(\S+\.[cm]?[jt]sx?)(?:\s+>\s+(.*))?$)re");
    static const std::regex titleRe(R"re(^\s*● (.+)$)re");
    static const std::regex errorRe(R"re(^\s*((?:[A-Z]\w*)?Error: .*)$)re");
    // "at fn (path:line:col)" or "at path:line:col"
    static const std::regex atRe(R"re(^\s*at (?:.*\()?([^()\s]+):(\d+):(\d+)\)?$)re");
    static const std::regex arrowRe(R"re(^\s*❯ (\S.*?):(\d+):(\d+)$)re");

    std::vector<CodeError> errors;
    std::vector<std::string> failedFiles;
    std::string title, message;
    bool wantMessage = false;   // jest: the message is the first text line after "● title"

    for (const auto& line : splitLines(output)) {
        std::smatch m;
        if (std::regex_match(line, m, failRe)) {
            failedFiles.push_back(m[1].str());
            title = m[2].matched ? m[2].str() : "";
            message.clear();
            wantMessage = false;
        } else if (std::regex_match(line, m, titleRe)) {
            title = m[1].str();
            message.clear();
            wantMessage = true;
        } else if (std::regex_match(line, m, errorRe)) {
            message = m[1].str();
            wantMessage = false;
        } else if (std::regex_match(line, m, atRe) || std::regex_match(line, m, arrowRe)) {
            std::string text = title;
            if (!message.empty()) text += (text.empty() ? "" : ": ") + message;
            if (text.empty()) text = "test failed";
            errors.push_back(makeError(m[1].str(), toInt(m[2].str()), toInt(m[3].str()), text));
        } else if (wantMessage && !trim(line).empty()) {
            message = trim(line);
            wantMessage = false;
        }
    }

    // A failed file without any usable location still needs to reach the fix loop.
    for (const auto& f : failedFiles) {
        const bool located = std::any_of(errors.begin(), errors.end(), [&](const CodeError& e) {
            return e.file.size() >= f.size() && e.file.compare(e.file.size() - f.size(), f.size(), f) == 0;
        });
        if (!located) errors.push_back(makeError(f, 0, 0, "FAIL " + f));
    }
    return errors;
}

// ---------------------------------------------------------------------------
// SQL dry run (StackToolchain) and alembic
// ---------------------------------------------------------------------------
//
//   db/002_orders.sql:2: error: near "TABEL": syntax error
std::vector<CodeError> ToolOutputParser::parseSql(const std::string& output) {
    static const std::regex lineRe(R"re(^(\S.*?\.sql):(\d+): (error|warning): (.*)$)re");

    std::vector<CodeError> errors;
    for (const auto& line : splitLines(output)) {
        std::smatch m;
        if (std::regex_match(line, m, lineRe)) {
            errors.push_back(makeError(m[1].str(), toInt(m[2].str()), 0, m[4].str(), m[3].str()));
        }
    }
    // alembic reports failing migrations as Python tracebacks
    for (auto& e : parsePython(output)) errors.push_back(e);
    return errors;
}

// ---------------------------------------------------------------------------
// sqlfluff
// ---------------------------------------------------------------------------
//
//   == [db/002_orders.sql] FAIL
//   L:   2 | P:   1 |  PRS | Line 2, Position 1: Found unparsable section: 'CREATE
//                          | TABEL orders ...                      <- continuation lines are ignored
std::vector<CodeError> ToolOutputParser::parseSqlfluff(const std::string& output) {
    static const std::regex fileRe(R"re(^== \[(.+)\] FAIL\s*$)re");
    static const std::regex itemRe(R"re(^L:\s*(\d+) \| P:\s*(\d+) \|\s*(\w+) \| (.*)$)re");

    std::vector<CodeError> errors;
    std::string currentFile;
    for (const auto& line : splitLines(output)) {
        std::smatch m;
        if (std::regex_match(line, m, fileRe)) {
            currentFile = m[1].str();
        } else if (!currentFile.empty() && std::regex_match(line, m, itemRe)) {
            const std::string code = m[3].str();
            // PRS/LXR/TMP = the file cannot be parsed; everything else is a style rule
            const bool fatal = code == "PRS" || code == "LXR" || code == "TMP";
            errors.push_back(makeError(currentFile, toInt(m[1].str()), toInt(m[2].str()),
                                       code + ": " + trim(m[4].str()), fatal ? "error" : "warning"));
        }
    }
    return errors;
}

// ---------------------------------------------------------------------------
// Unknown tools (commands added in .buggai.json)
// ---------------------------------------------------------------------------
std::vector<CodeError> ToolOutputParser::parseGeneric(const std::string& output) {
    std::vector<CodeError> errors = parsePython(output);
    for (auto& e : parseTsc(output)) errors.push_back(e);
    for (auto& e : parseSql(output)) errors.push_back(e);
    if (errors.empty()) errors = parseLint(output);
    return errors;
}

// ---------------------------------------------------------------------------
// registry + path resolution
// ---------------------------------------------------------------------------

std::string ToolOutputParser::parserFor(const std::string& tool) {
    static const std::map<std::string, std::string> registry = {
        {"python", "python"}, {"compileall", "python"}, {"py_compile", "python"},
        {"pytest", "pytest"},
        {"ruff", "lint"}, {"flake8", "lint"}, {"mypy", "lint"},
        {"tsc", "tsc"},
        {"eslint", "eslint"},
        {"jest", "jest"}, {"vitest", "jest"}, {"test", "jest"},
        {"sql", "sql"}, {"alembic", "sql"},
        {"sqlfluff", "sqlfluff"},
    };
    auto it = registry.find(tool);
    return it == registry.end() ? "generic" : it->second;
}

std::vector<CodeError> ToolOutputParser::parse(const std::string& tool,
                                               const std::string& output,
                                               const std::string& componentRoot,
                                               const std::string& projectRoot) {
    using ParserFn = std::vector<CodeError> (*)(const std::string&);
    static const std::map<std::string, ParserFn> parsers = {
        {"python", &parsePython}, {"pytest", &parsePytest}, {"lint", &parseLint},
        {"tsc", &parseTsc}, {"eslint", &parseEslint}, {"jest", &parseJest},
        {"sql", &parseSql}, {"sqlfluff", &parseSqlfluff}, {"generic", &parseGeneric},
    };

    std::error_code ec;
    const fs::path project = fs::weakly_canonical(projectRoot, ec);
    const fs::path component = fs::weakly_canonical(componentRoot, ec);

    std::vector<CodeError> result;
    std::set<std::string> seen;
    std::map<std::string, size_t> framesPerMessage;
    const std::string parser = parserFor(tool);
    const bool traceback = parser == "python" || parser == "pytest";

    for (CodeError e : parsers.at(parser)(output)) {
        // Tools print paths relative to their working directory (the component root);
        // some print absolute paths. Try the component first, then the project root.
        fs::path p(e.file);
        if (p.is_relative()) {
            p = fs::exists(component / p) ? component / p : project / p;
        }
        p = fs::weakly_canonical(p, ec);
        if (ec || !fs::is_regular_file(p, ec)) continue;

        const std::string rel = p.lexically_relative(project).generic_string();
        if (rel.empty() || rel.rfind("..", 0) == 0) continue;      // outside the project (stdlib, tools)
        if (rel.find("node_modules/") != std::string::npos || rel.find("site-packages/") != std::string::npos
            || rel.find(".venv/") != std::string::npos || rel.find("venv/") == 0
            || rel.find("/venv/") != std::string::npos) continue;  // dependencies, not project code

        e.file = rel;
        e.tool = tool;
        if (!seen.insert(rel + ":" + std::to_string(e.line) + ":" + e.message).second) continue;
        // long tracebacks: keep only the innermost project frames of one error
        if (traceback && ++framesPerMessage[e.message] > MAX_FRAMES_PER_ERROR) continue;
        result.push_back(e);
    }
    return result;
}
