#include "StackContext.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace {

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

bool isWordChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

void addUnique(std::vector<std::string>& list, const std::string& value) {
    if (std::find(list.begin(), list.end(), value) == list.end()) list.push_back(value);
}

// Words that appear in almost every compiler message and say nothing about the code.
bool isStopWord(const std::string& word) {
    static const std::set<std::string> stop = {
        "error", "type", "types", "property", "module", "named", "does", "not", "exist", "the",
        "and", "for", "from", "with", "string", "number", "boolean", "object", "any", "undefined",
        "null", "none", "true", "false", "int", "str", "self", "this", "assert", "main", "line",
        "file", "syntax", "near", "such", "table", "column", "cannot", "find"
    };
    return stop.count(toLower(word)) > 0;
}

// "ModuleNotFoundError", "ValidationException": the kind of error, not a name in the code.
bool isExceptionName(const std::string& word) {
    for (const char* suffix : {"Error", "Exception", "Warning"}) {
        const size_t n = std::char_traits<char>::length(suffix);
        if (word.size() > n && word.compare(word.size() - n, n, suffix) == 0) return true;
    }
    return false;
}

// Case-insensitive whole-word search ("name" does not match "full_name").
bool containsWord(const std::string& lowerText, const std::string& lowerWord) {
    for (size_t pos = lowerText.find(lowerWord); pos != std::string::npos; pos = lowerText.find(lowerWord, pos + 1)) {
        const bool startOk = pos == 0 || !isWordChar(lowerText[pos - 1]);
        const size_t end = pos + lowerWord.size();
        const bool endOk = end >= lowerText.size() || !isWordChar(lowerText[end]);
        if (startOk && endOk) return true;
    }
    return false;
}

bool isTestFile(const std::string& path) {
    const std::string name = fs::path(path).filename().string();
    return path.find("/tests/") != std::string::npos || path.rfind("tests/", 0) == 0
        || path.find("/test/") != std::string::npos || path.find("__tests__") != std::string::npos
        || name.rfind("test_", 0) == 0 || name.find(".test.") != std::string::npos
        || name.find(".spec.") != std::string::npos || name.find("_test.") != std::string::npos;
}

// Files that usually define the contract between components.
bool isContractFile(const std::string& path) {
    const std::string lower = toLower(path);
    for (const char* hint : {"model", "schema", "route", "api", "types", "dto", "serializer",
                             "migration", "entity", "view", "controller", ".sql"}) {
        if (lower.find(hint) != std::string::npos) return true;
    }
    return false;
}

struct Candidate {
    std::string file;
    int score = 0;
    std::vector<std::string> matches;
    std::string content;
};

// Lines around the first occurrences of the matched names, for files that are too large
// to send completely.
std::string excerpt(const Candidate& c, size_t maxChars) {
    std::vector<std::string> lines;
    std::istringstream in(c.content);
    for (std::string line; std::getline(in, line);) lines.push_back(line);

    std::set<size_t> keep;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string lower = toLower(lines[i]);
        for (const auto& m : c.matches) {
            if (!containsWord(lower, toLower(m))) continue;
            for (size_t j = (i >= 4 ? i - 4 : 0); j <= i + 4 && j < lines.size(); ++j) keep.insert(j);
            break;
        }
    }

    std::string out;
    size_t previous = 0;
    for (size_t i : keep) {
        if (!out.empty() && i != previous + 1) out += "    ...\n";
        const std::string row = std::to_string(i + 1) + ": " + lines[i] + "\n";
        if (out.size() + row.size() > maxChars) { out += "    ... [excerpt truncated]\n"; break; }
        out += row;
        previous = i;
    }
    return out;
}

std::string describe(const StackComponent& c) {
    std::string s = "'" + c.name + "' (" + c.language;
    if (!c.service.empty() && c.service != c.name) s += ", service '" + c.service + "'";
    return s + ", folder " + c.root + ")";
}

} // namespace

bool StackContext::isSensitiveFile(const std::string& path) {
    const std::string name = toLower(fs::path(path).filename().string());
    const std::string ext = toLower(fs::path(path).extension().string());
    if (name.rfind(".env", 0) == 0 || name == ".npmrc" || name == ".pypirc" || name == ".netrc") return true;
    if (ext == ".pem" || ext == ".key" || ext == ".p12" || ext == ".pfx" || ext == ".jks") return true;
    return name.rfind("id_rsa", 0) == 0 || name.rfind("id_ed25519", 0) == 0
        || name.find("secret") != std::string::npos || name.find("credential") != std::string::npos;
}

std::vector<std::string> StackContext::nameVariants(const std::string& identifier) {
    std::vector<std::string> variants = {identifier};

    // snake_case -> camelCase: full_name -> fullName
    if (identifier.find('_') != std::string::npos) {
        std::string camel;
        bool upper = false;
        for (char c : identifier) {
            if (c == '_') { upper = !camel.empty(); continue; }
            camel += upper ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
            upper = false;
        }
        if (!camel.empty()) addUnique(variants, camel);
    }

    // camelCase / PascalCase -> snake_case: fullName -> full_name
    std::string snake;
    for (size_t i = 0; i < identifier.size(); ++i) {
        const unsigned char c = identifier[i];
        if (std::isupper(c) && i > 0 && std::islower(static_cast<unsigned char>(identifier[i - 1]))) snake += '_';
        snake += static_cast<char>(std::tolower(c));
    }
    if (snake.find('_') != std::string::npos) addUnique(variants, snake);

    return variants;
}

std::vector<std::string> StackContext::identifiers(const std::string& message) {
    std::vector<std::string> result;
    static const std::regex wordRe(R"re([A-Za-z_][A-Za-z0-9_]*)re");

    auto addWords = [&](const std::string& text, bool onlyCompound) {
        for (auto it = std::sregex_iterator(text.begin(), text.end(), wordRe); it != std::sregex_iterator(); ++it) {
            const std::string word = it->str();
            if (word.size() < 3) continue;
            // compound = snake_case or camelCase: specific enough to search for even
            // when it is not quoted in the message
            bool compound = word.find('_') != std::string::npos;
            for (size_t i = 1; i < word.size() && !compound; ++i) {
                // a lower case letter followed by an upper case one: userId, UserProfile.
                // Codes such as TS2339 or E501 do not qualify.
                compound = std::islower(static_cast<unsigned char>(word[i - 1]))
                        && std::isupper(static_cast<unsigned char>(word[i]));
            }
            if (onlyCompound ? !compound || isExceptionName(word) : isStopWord(word)) continue;
            addUnique(result, word);
        }
    };

    // 1. everything in quotes or backticks:  Property 'name' ... type 'User'  /  No module named 'app.modles'
    static const std::regex quotedRe(R"re('([^'\n]{1,80})'|"([^"\n]{1,80})"|`([^`\n]{1,80})`)re");
    for (auto it = std::sregex_iterator(message.begin(), message.end(), quotedRe); it != std::sregex_iterator(); ++it) {
        for (size_t g = 1; g <= 3; ++g) {
            if ((*it)[g].matched) addWords((*it)[g].str(), false);
        }
    }

    // 2. database messages name the object after a colon or keyword without quotes:
    //    "no such column: full_name", "table users has no column named email"
    static const std::regex dbRe(R"re((?:no such (?:table|column|index): |has no column named |table |column |relation )([A-Za-z_][\w.]*))re",
                                 std::regex::icase);
    for (auto it = std::sregex_iterator(message.begin(), message.end(), dbRe); it != std::sregex_iterator(); ++it) {
        addWords((*it)[1].str(), false);
    }

    // 3. unquoted snake_case / camelCase words anywhere
    addWords(message, true);

    return result;
}

namespace {

// Related files of neighbouring components (see the class comment). Adds to context.files
// and sets context.note; `total` is the number of characters already used by the budget.
void addCrossComponentContext(const ProjectStack& stack,
                              const std::vector<std::string>& projectFiles,
                              const std::vector<std::string>& relevantFiles,
                              const std::vector<CodeError>& errors,
                              FixContext& context, size_t total) {
    using SC = StackContext;

    // Components that contain the error
    std::vector<const StackComponent*> owners;
    for (const auto& f : relevantFiles) {
        const StackComponent* c = stack.componentOf(f);
        if (c && std::find(owners.begin(), owners.end(), c) == owners.end()) owners.push_back(c);
    }
    if (owners.empty()) return;

    // Components across a boundary: what the owners depend on and what depends on them
    std::vector<std::string> neighbours;
    for (const auto* owner : owners) {
        for (const auto& n : stack.neighboursOf(owner->name)) {
            const bool isOwner = std::any_of(owners.begin(), owners.end(),
                                             [&](const StackComponent* o) { return o->name == n; });
            if (!isOwner) addUnique(neighbours, n);
        }
    }
    if (neighbours.empty()) return;

    // Names from the messages of this error, each with its snake_case/camelCase variant:
    // API and ORM renames usually differ exactly in that spelling.
    std::vector<std::string> names;
    for (const auto& e : errors) {
        if (std::find(relevantFiles.begin(), relevantFiles.end(), e.file) == relevantFiles.end()) continue;
        for (const auto& id : SC::identifiers(e.message)) {
            for (const auto& v : SC::nameVariants(id)) addUnique(names, v);
        }
    }
    if (names.empty()) return;

    // Score the neighbour files: one point per distinct name, one more for contract files.
    std::vector<Candidate> candidates;
    for (const auto& f : projectFiles) {
        if (SC::isSensitiveFile(f) || isTestFile(f)) continue;
        if (std::find(relevantFiles.begin(), relevantFiles.end(), f) != relevantFiles.end()) continue;
        const StackComponent* c = stack.componentOf(f);
        if (!c || std::find(neighbours.begin(), neighbours.end(), c->name) == neighbours.end()) continue;

        std::error_code ec;
        const fs::path full = fs::path(stack.projectRoot) / f;
        if (fs::file_size(full, ec) > 500 * 1024 || ec) continue;   // generated or data files

        Candidate candidate;
        candidate.file = f;
        candidate.content = readFile(full);
        const std::string lower = toLower(candidate.content);
        for (const auto& n : names) {
            if (containsWord(lower, toLower(n))) candidate.matches.push_back(n);
        }
        if (candidate.matches.empty()) continue;
        // A single matching name is weak evidence (short words like "app" or "id" appear
        // everywhere). It only counts in a contract file and for a name of 4+ characters.
        if (candidate.matches.size() == 1
            && !(isContractFile(f) && candidate.matches.front().size() >= 4)) continue;
        candidate.score = static_cast<int>(candidate.matches.size()) + (isContractFile(f) ? 1 : 0);
        candidates.push_back(std::move(candidate));
    }
    if (candidates.empty()) return;

    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        return a.score != b.score ? a.score > b.score : a.file < b.file;
    });
    if (candidates.size() > SC::MAX_RELATED_FILES) candidates.resize(SC::MAX_RELATED_FILES);

    // Budget. A file that fits is sent in full as a FILE block, so the Debugger can fix it
    // there. A file that does not fit is never cut (the answer would overwrite the file
    // with the cut version); it is shown as a read-only excerpt in the note instead.
    std::string included, excerpts;
    for (const auto& c : candidates) {
        std::string matched;
        for (const auto& m : c.matches) matched += (matched.empty() ? "" : ", ") + m;

        if (c.content.size() <= SC::MAX_FILE_CHARS && total + c.content.size() <= SC::MAX_TOTAL_CHARS) {
            total += c.content.size();
            context.files.push_back(c.file);
            included += "- " + c.file + " (mentions: " + matched + ")\n";
        } else {
            excerpts += "--- " + c.file + " (mentions: " + matched + ") ---\n" + excerpt(c, SC::MAX_EXCERPT_CHARS);
        }
    }

    std::ostringstream note;
    note << "The error was reported in component ";
    for (size_t i = 0; i < owners.size(); ++i) note << (i ? " and " : "") << describe(*owners[i]);
    note << ".\nComponents on the other side of its boundary:\n";
    for (const auto& n : neighbours) {
        const StackComponent* c = stack.find(n);
        if (!c) continue;
        const bool ownerDependsOnIt = std::any_of(owners.begin(), owners.end(), [&](const StackComponent* o) {
            return std::find(o->dependsOn.begin(), o->dependsOn.end(), n) != o->dependsOn.end();
        });
        note << "- " << describe(*c) << (ownerDependsOnIt ? ": the failing component depends on it" : ": it depends on the failing component") << "\n";
    }
    if (!included.empty()) {
        note << "Related files of those components are included above as FILE blocks. The root cause "
                "may be in one of them (for example a renamed API field, column or type):\n" << included;
    }
    if (!excerpts.empty()) {
        note << "Read-only excerpts of larger related files (do NOT use them as file_path):\n" << excerpts;
    }
    context.note = note.str();
}

// Project files imported by `file` (Python "import a.b" / "from a.b import c",
// JS/TS relative imports). Only files that exist in the project are returned.
std::vector<std::string> localImports(const ProjectStack& stack, const std::string& file,
                                      const std::set<std::string>& projectFiles) {
    std::vector<std::string> result;
    const std::string content = readFile(fs::path(stack.projectRoot) / file);
    const std::string dir = fs::path(file).parent_path().generic_string();
    const StackComponent* component = stack.componentOf(file);

    auto join = [](const std::string& base, const std::string& rel) {
        std::string p = (fs::path(base.empty() || base == "." ? "" : base) / rel).lexically_normal().generic_string();
        return p;
    };
    auto addIfProjectFile = [&](const std::string& path) {
        if (projectFiles.count(path) && path != file) addUnique(result, path);
    };

    const std::string ext = fs::path(file).extension().string();
    if (ext == ".py") {
        // "from pkg.mod import a, b"  or  "import pkg.mod"; leading dots = relative import
        static const std::regex importRe(R"re(^\s*(?:from\s+(\.*[\w.]*)\s+import\s+([\w, ]+)|import\s+([\w.]+)))re");
        std::istringstream lines(content);
        for (std::string line; std::getline(lines, line);) {
            std::smatch m;
            if (!std::regex_search(line, m, importRe)) continue;
            std::string module = m[1].matched ? m[1].str() : m[3].str();

            // absolute imports are looked up from the component root (pytest/python run
            // there) and from the importing file's folder; relative ones from the folder
            std::vector<std::string> bases;
            const size_t dots = module.find_first_not_of('.') == std::string::npos ? module.size() : module.find_first_not_of('.');
            if (dots > 0) {
                std::string base = dir;
                for (size_t i = 1; i < dots; ++i) base = fs::path(base).parent_path().generic_string();
                bases = {base};
                module = module.substr(dots);
            } else {
                if (component) bases.push_back(component->root);
                bases.push_back(dir);
            }

            std::string path = module;
            std::replace(path.begin(), path.end(), '.', '/');
            for (const auto& base : bases) {
                if (!path.empty()) {
                    addIfProjectFile(join(base, path + ".py"));
                    addIfProjectFile(join(base, path + "/__init__.py"));
                }
                // "from pkg import mod": the imported names can be submodules
                if (m[2].matched) {
                    std::istringstream names(m[2].str());
                    for (std::string name; std::getline(names, name, ',');) {
                        name = name.substr(0, name.find(" as "));
                        name.erase(std::remove(name.begin(), name.end(), ' '), name.end());
                        if (!name.empty()) addIfProjectFile(join(base, (path.empty() ? "" : path + "/") + name + ".py"));
                    }
                }
            }
        }
    } else {
        // import x from "./format"  /  require("../api/client")
        static const std::regex importRe(R"re((?:from|import|require\()\s*["'](\.{1,2}/[^"']+)["'])re");
        for (auto it = std::sregex_iterator(content.begin(), content.end(), importRe); it != std::sregex_iterator(); ++it) {
            const std::string target = join(dir, (*it)[1].str());
            for (const char* suffix : {"", ".ts", ".tsx", ".js", ".jsx", ".mjs", ".vue", "/index.ts", "/index.tsx", "/index.js"}) {
                addIfProjectFile(target + suffix);
            }
        }
    }
    return result;
}

} // namespace

FixContext StackContext::select(const ProjectStack& stack,
                                const std::vector<std::string>& projectFiles,
                                const std::vector<std::string>& relevantFiles,
                                const std::vector<CodeError>& errors) {
    FixContext context;
    size_t total = 0;

    // 1. Code under test. The traceback of a failing test names the test file, but the
    //    defect is normally in the module the test imports. Without that module the
    //    Debugger could only "fix" the test.
    const std::set<std::string> known(projectFiles.begin(), projectFiles.end());
    for (const auto& f : relevantFiles) {
        if (!isTestFile(f)) continue;
        for (const auto& imported : localImports(stack, f, known)) {
            if (context.files.size() >= MAX_RELATED_FILES) break;
            if (isTestFile(imported) || isSensitiveFile(imported)) continue;
            if (std::find(relevantFiles.begin(), relevantFiles.end(), imported) != relevantFiles.end()) continue;
            if (std::find(context.files.begin(), context.files.end(), imported) != context.files.end()) continue;

            std::error_code ec;
            const size_t size = fs::file_size(fs::path(stack.projectRoot) / imported, ec);
            if (ec || size > MAX_FILE_CHARS || total + size > MAX_TOTAL_CHARS) continue;
            total += size;
            context.files.push_back(imported);
        }
    }

    // 2. Files of neighbouring components that mention the names from the error.
    addCrossComponentContext(stack, projectFiles, relevantFiles, errors, context, total);
    return context;
}
