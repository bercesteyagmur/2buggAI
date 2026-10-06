#include "WebPipeline.h"
#include "CodeChanger.h"
#include "ErrorCollector.h"
#include "ErrorMatcher.h"
#include "FileCollector.h"
#include "LanguageDetector.h"
#include "OpenAiClient.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

static std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    size_t end = s.find_last_not_of(" \t\r\n");
    return start == std::string::npos ? "" : s.substr(start, end - start + 1);
}

static std::string limit(const std::string& s, size_t maxChars) {
    if (s.size() <= maxChars) return s;
    return s.substr(0, maxChars / 2) + "\n...[truncated]...\n" + s.substr(s.size() - maxChars / 2);
}

std::string WebPipeline::collectErrorOutput(const std::vector<SetupStep>& steps) {
    std::string out;
    for (const auto& s : steps) {
        bool isInstall = s.name.find("install") != std::string::npos;
        if (isInstall && s.ok()) continue;

        out += "===== STEP: " + s.name + " | exit code " + std::to_string(s.result.exit_code) + " =====\n";

        std::istringstream lines(s.result.output);
        for (std::string line; std::getline(lines, line);) {
            std::string t = trim(line);
            bool frame = (t.rfind("#", 0) == 0 || t.rfind("at ", 0) == 0);
            bool library = line.find("/vendor/") != std::string::npos
                        || line.find("/node_modules/") != std::string::npos;
            if (frame && library) continue;
            out += line + "\n";
        }
    }
    return out;
}

std::vector<CodeError> WebPipeline::parseErrors(const std::string& output, const std::string& projectPath) {
    static const std::string ext = "(?:php|ts|tsx|js|jsx|mjs|cjs|vue)";
    static const std::vector<std::regex> patterns = {
        std::regex("(\\S+\\." + ext + ")\\((\\d+),\\d+\\)"),
        std::regex("in (\\S+\\.php) on line (\\d+)"),
        std::regex("(/[^\\s:'\"()]+\\." + ext + ")\\((\\d+)\\)"),
        std::regex("((?:\\.{0,2}/)?[\\w@.\\-/]+\\." + ext + "):(\\d+)"),
    };

    std::vector<CodeError> errors;
    std::set<std::string> seen;
    fs::path root = fs::weakly_canonical(projectPath);

    std::string step;
    std::istringstream stream(output);
    for (std::string line; std::getline(stream, line);) {
        if (line.rfind("===== STEP: ", 0) == 0) {
            step = line.substr(12, line.find(" | ") - 12);
            continue;
        }
        for (const auto& re : patterns) {
            std::smatch m;
            if (!std::regex_search(line, m, re)) continue;

            fs::path p(m[1].str());
            if (p.is_relative()) p = root / p;
            std::error_code ec;
            p = fs::weakly_canonical(p, ec);
            if (ec || !fs::is_regular_file(p)) continue;

            std::string rel = p.lexically_relative(root).string();
            if (rel.empty() || rel.rfind("..", 0) == 0) continue;
            if (rel.rfind("vendor/", 0) == 0 || rel.find("node_modules/") != std::string::npos) continue;

            CodeError e;
            e.file = rel;
            e.line = std::stoi(m[2].str());
            e.message = limit(trim(line), 400);
            e.step = step;

            std::string key = e.file + ":" + std::to_string(e.line) + ":" + e.message;
            if (seen.insert(key).second) errors.push_back(e);
            break;
        }
    }
    return errors;
}

bool WebPipeline::isEnvironmentError(const std::string& errorName) {
    static const std::set<std::string> environment = {
        "missing_php_extension", "php_version_mismatch", "composer_dependency_conflict",
        "missing_app_key", "db_connection_failed", "npm_dependency_conflict",
        "node_version_mismatch", "lockfile_outdated"
    };
    return environment.count(errorName) > 0;
}

static std::vector<std::string> keywordsOf(const std::string& errorName, const std::vector<ErrorCategory>& checklist) {
    std::vector<std::string> keywords;
    for (const auto& c : checklist) {
        if (c.name != errorName) continue;
        for (const auto& k : c.keywords) {
            std::string lower = toLower(trim(k));
            if (!lower.empty() && lower != "-") keywords.push_back(lower);
        }
    }
    return keywords;
}

static std::map<std::string, std::string> splitSteps(const std::string& errorOutput) {
    std::map<std::string, std::string> steps;
    std::string current;
    std::istringstream stream(errorOutput);
    for (std::string line; std::getline(stream, line);) {
        if (line.rfind("===== STEP: ", 0) == 0) {
            current = line.substr(12, line.find(" | ") - 12);
            continue;
        }
        steps[current] += toLower(line) + "\n";
    }
    return steps;
}

static std::set<std::string> stepsContaining(const std::vector<std::string>& keywords,
                                             const std::map<std::string, std::string>& steps) {
    std::set<std::string> result;
    for (const auto& [name, text] : steps) {
        for (const auto& k : keywords) {
            if (text.find(k) != std::string::npos) { result.insert(name); break; }
        }
    }
    return result;
}

std::string WebPipeline::environmentCause(const std::string& errorName,
                                          const std::vector<ErrorCategory>& checklist,
                                          const std::string& errorOutput) {
    auto steps = splitSteps(errorOutput);
    std::set<std::string> errorSteps = stepsContaining(keywordsOf(errorName, checklist), steps);
    if (errorSteps.empty()) return "";

    std::set<std::string> checked;
    for (const auto& c : checklist) {
        if (!isEnvironmentError(c.name) || !checked.insert(c.name).second) continue;
        for (const auto& step : stepsContaining(keywordsOf(c.name, checklist), steps)) {
            if (errorSteps.count(step)) return c.name;
        }
    }
    return "";
}

std::vector<std::string> WebPipeline::filesForError(const std::string& errorName,
                                                    const std::vector<CodeError>& errors,
                                                    const std::vector<ErrorCategory>& checklist,
                                                    const std::string& errorOutput) {
    std::vector<std::string> keywords = keywordsOf(errorName, checklist);

    std::vector<std::string> files;
    auto add = [&](const std::string& f) {
        if (files.size() < 5 && std::find(files.begin(), files.end(), f) == files.end()) files.push_back(f);
    };

    for (const auto& e : errors) {
        std::string msg = toLower(e.message);
        for (const auto& k : keywords) {
            if (msg.find(k) != std::string::npos) {
                add(e.file);
                break;
            }
        }
    }

    if (files.empty()) {
        std::set<std::string> steps = stepsContaining(keywords, splitSteps(errorOutput));
        for (const auto& e : errors) {
            if (steps.count(e.step)) add(e.file);
        }
    }
    return files;
}

static std::vector<std::string> matchAll(ErrorMatcher& matcher, const std::string& output,
                                         const std::set<std::string>& languages) {
    std::vector<std::string> detected;
    for (const auto& lang : languages) {
        for (const auto& e : matcher.match(output, lang)) {
            if (std::find(detected.begin(), detected.end(), e) == detected.end()) detected.push_back(e);
        }
    }
    ErrorCollector collector;
    return collector.sortedErrors(detected);
}

static std::string relevantOutput(const std::string& output, const std::vector<std::string>& files,
                                  const std::vector<std::string>& keywords) {
    std::string out;
    std::istringstream stream(output);
    for (std::string line; std::getline(stream, line);) {
        std::string lower = toLower(line);
        bool relevant = line.rfind("===== STEP:", 0) == 0;
        for (const auto& f : files) relevant = relevant || line.find(f) != std::string::npos;
        for (const auto& k : keywords) relevant = relevant || (!k.empty() && k != "-" && lower.find(k) != std::string::npos);
        if (relevant) out += line + "\n";
    }
    return limit(out.empty() ? output : out, 12000);
}

int WebPipeline::run(const WebPipelineOptions& options, const std::vector<std::string>& files) {
    const std::string& targetPath = options.targetPath;

    LanguageDetector detector;
    std::set<std::string> languages = {"sql"};
    for (const auto& [lang, _] : detector.detectAll(files)) {
        languages.insert(lang == "blade" ? "php" : lang);
    }

    std::vector<SetupStep> setup = WebEnvironmentManager::setupProject(targetPath);
    WebEnvironmentManager::printSummary(setup);

    auto check = [&]() {
        return WebEnvironmentManager::runChecks(targetPath, options.runtime);
    };
    std::vector<SetupStep> checks = check();
    WebEnvironmentManager::printSummary(checks, "CHECK SUMMARY");

    std::vector<SetupStep> allSteps = setup;
    allSteps.insert(allSteps.end(), checks.begin(), checks.end());
    std::string errorOutput = collectErrorOutput(allSteps);

    ChecklistReader reader;
    auto checklist = reader.load();
    ErrorMatcher matcher(checklist);

    std::vector<std::string> detectedErrors = matchAll(matcher, errorOutput, languages);
    std::vector<CodeError> codeErrors = parseErrors(errorOutput, targetPath);

    std::cout << "\n========== DETECTED ERRORS ==========\n";
    if (detectedErrors.empty()) std::cout << "No known errors detected.\n";
    for (const auto& e : detectedErrors) {
        std::cout << "- " << e << " (" << ErrorCollector::difficultyOf(e) << ")"
                  << (isEnvironmentError(e) ? " [environment]" : "") << "\n";
    }
    std::cout << "\nErrors with file and line: " << codeErrors.size() << "\n";
    for (size_t i = 0; i < codeErrors.size() && i < 20; ++i) {
        std::cout << "  " << codeErrors[i].file << ":" << codeErrors[i].line << "\n";
    }
    std::cout << "=====================================\n\n";

    std::vector<std::string> giveUp;
    std::vector<std::string> fixedErrors;
    FileCollector collector;

    if (!detectedErrors.empty()) {
        OpenAIClient fixClient;
        std::string checklistRaw = reader.loadRaw();

        const int MAX_ROUNDS = 20;
        const int MAX_ITER = 5;

        auto redetect = [&]() {
            checks = check();
            allSteps = setup;
            allSteps.insert(allSteps.end(), checks.begin(), checks.end());
            errorOutput = collectErrorOutput(allSteps);
            codeErrors = parseErrors(errorOutput, targetPath);
            detectedErrors = matchAll(matcher, errorOutput, languages);
        };

        auto isGivenUp = [&](const std::string& e) {
            return std::find(giveUp.begin(), giveUp.end(), e) != giveUp.end();
        };

        int round = 0;
        while (round < MAX_ROUNDS) {
            std::string current;
            for (const auto& e : detectedErrors) {
                if (!isGivenUp(e)) { current = e; break; }
            }
            if (current.empty()) break;

            if (isEnvironmentError(current)) {
                std::cout << "[ENVIRONMENT] " << current
                          << " is a setup/environment problem, not a code bug. Skipping fix attempts.\n";
                giveUp.push_back(current);
                continue;
            }

            std::string cause = environmentCause(current, checklist, errorOutput);
            if (!cause.empty()) {
                std::cout << "[ROOT CAUSE] " << current << " is caused by " << cause
                          << " (same step), which is an environment problem. Skipping fix attempts.\n";
                giveUp.push_back(current);
                continue;
            }

            std::vector<std::string> relevantFiles = filesForError(current, codeErrors, checklist, errorOutput);
            if (relevantFiles.empty()) {
                std::cout << "No source file found in the output for " << current << ", skipping.\n";
                giveUp.push_back(current);
                continue;
            }

            round++;
            bool fixed = false;

            std::vector<std::string> keywords = keywordsOf(current, checklist);

            for (int attempt = 1; attempt <= MAX_ITER; ++attempt) {
                std::cout << "Attempt " << attempt << "/" << MAX_ITER << " for error: " << current
                          << " (difficulty: " << ErrorCollector::difficultyOf(current) << ")\n";
                std::cout << "  Files sent to the AI:";
                for (const auto& f : relevantFiles) std::cout << " " << f;
                std::cout << "\n";

                std::stringstream source;
                for (const auto& f : relevantFiles) {
                    source << "===== FILE: " << f << " =====\n";
                    source << collector.readSourceCode((fs::path(targetPath) / f).string()) << "\n\n";
                }

                FixRequest req;
                req.error_name = current;
                req.error_output = relevantOutput(errorOutput, relevantFiles, keywords);
                req.language = LanguageDetector::languageOf(relevantFiles.front());
                req.source_code = limit(source.str(), 80000);
                req.checklist = checklistRaw;

                FixResult res = fixClient.fix_code(req);
                if (!res.success || res.fixed_code.empty()) continue;

                try {
                    CodeChanger changer(targetPath);
                    changer.apply_fix(res);
                    std::cout << "  -> fix applied to " << res.file_path << "\n";
                } catch (const std::exception& e) {
                    std::cerr << "  -> could not apply fix: " << e.what() << "\n";
                    continue;
                }

                redetect();
                bool gone = std::find(detectedErrors.begin(), detectedErrors.end(), current) == detectedErrors.end();
                if (gone) {
                    std::cout << "  -> fixed. Remaining errors: " << detectedErrors.size() << "\n";
                    fixedErrors.push_back(current);
                    fixed = true;
                    break;
                }
                relevantFiles = filesForError(current, codeErrors, checklist, errorOutput);
                if (relevantFiles.empty()) break;
                std::cout << "  -> error still present, retrying\n";
            }

            if (!fixed) {
                std::cerr << "Could not fix error: " << current << "\n";
                giveUp.push_back(current);
            }
        }

        if (detectedErrors.empty()) std::cout << "\nAll errors fixed.\n";
        else std::cout << "\nFinished. " << giveUp.size() << " error(s) could not be fixed.\n";
    }

    json report;
    report["target"] = targetPath;
    report["fix_description"] = options.fixDescription;
    report["languages"] = std::vector<std::string>(languages.begin(), languages.end());
    report["steps"] = json::array();
    for (const auto& s : allSteps) {
        report["steps"].push_back({
            {"name", s.name}, {"command", s.command}, {"exit_code", s.result.exit_code},
            {"output", limit(s.result.output, s.ok() ? 1000 : 6000)}
        });
    }
    report["detected_errors"] = detectedErrors;
    report["fixed_errors"] = fixedErrors;
    report["errors_with_location"] = json::array();
    std::set<std::string> reportFiles;
    for (size_t i = 0; i < codeErrors.size() && i < 50; ++i) {
        report["errors_with_location"].push_back({
            {"file", codeErrors[i].file}, {"line", codeErrors[i].line}, {"message", codeErrors[i].message}
        });
        if (reportFiles.size() < 5) reportFiles.insert(codeErrors[i].file);
    }
    std::string sourceCode;
    for (const auto& f : reportFiles) {
        sourceCode += "===== FILE: " + f + " =====\n" + collector.readSourceCode((fs::path(targetPath) / f).string()) + "\n\n";
    }
    report["source_code"] = limit(sourceCode, 60000);

    auto writeJson = [&]() {
        if (options.jsonOutFile.empty()) return;
        std::ofstream out(options.jsonOutFile);
        out << report.dump(2);
        std::cout << "JSON Report gespeichert: " << options.jsonOutFile << "\n";
    };
    writeJson();

    std::cout << "\n╔════════════════════════════════════════╗\n";
    std::cout << "║      OPENAI ANALYSE WIRD GESTARTET     ║\n";
    std::cout << "╚════════════════════════════════════════╝\n\n";

    OpenAIClient client;
    OpenAIResult r = client.debug_report(report.dump());
    if (r.http_status < 200 || r.http_status >= 300) {
        std::cerr << "OpenAI HTTP " << r.http_status << "\n" << r.raw_json << "\n";
        return 5;
    }

    std::cout << "\n===== DEBUG REPORT (OpenAI) =====\n";
    std::cout << (r.text.empty() ? r.raw_json : r.text) << "\n";
    std::cout << "=================================\n";

    for (const auto& entry : reader.appendFromAiAnalysis(r.text, checklist)) {
        std::cout << "New Checklist Entry: " << entry << "\n";
    }

    report["ai_analysis"] = r.text;
    writeJson();
    return 0;
}
