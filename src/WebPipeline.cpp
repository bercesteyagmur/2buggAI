#include "WebPipeline.h"
#include "CodeChanger.h"
#include "FixRecord.h"
#include "ErrorCollector.h"
#include "ErrorMatcher.h"
#include "FileCollector.h"
#include "LanguageDetector.h"
#include "OpenAiClient.h"
#include "PdfReportExporter.h"

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
        if (s.ok()) continue;

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
        "node_version_mismatch", "lockfile_outdated",
        // fullstack projects (StackToolchain setup steps)
        "pip_install_failed", "npm_install_failed", "command_timeout"
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

    static const std::set<std::string> downstream = {
        "http_server_error", "sql_table_not_found", "sql_column_not_found", "vite_manifest_missing"
    };

    std::set<std::string> checked;
    for (const auto& c : checklist) {
        if (!isEnvironmentError(c.name) || !checked.insert(c.name).second) continue;
        for (const auto& step : stepsContaining(keywordsOf(c.name, checklist), steps)) {
            if (errorSteps.count(step)) return c.name;
            if (downstream.count(errorName) && step.find("runtime") == std::string::npos) return c.name;
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
    const std::string targetPath = options.targetPath;
    const bool runtime = options.runtime;

    PipelineHooks hooks;

    LanguageDetector detector;
    hooks.languages = {"sql"};
    for (const auto& [lang, _] : detector.detectAll(files)) {
        hooks.languages.insert(lang == "blade" ? "php" : lang);
    }

    hooks.setup = [targetPath]() { return WebEnvironmentManager::setupProject(targetPath); };
    // PHP/JS projects are one unit: every re-check runs all checks.
    hooks.check = [targetPath, runtime](const std::string&) {
        return WebEnvironmentManager::runChecks(targetPath, runtime);
    };
    hooks.parseErrors = [targetPath](const std::string& output) { return parseErrors(output, targetPath); };

    return runWithHooks(options, hooks);
}

int WebPipeline::runWithHooks(const WebPipelineOptions& options, const PipelineHooks& hooks) {
    const std::string& targetPath = options.targetPath;
    const std::set<std::string>& languages = hooks.languages;

    std::vector<SetupStep> setup = hooks.setup();
    WebEnvironmentManager::printSummary(setup);

    std::vector<SetupStep> checks = hooks.check("");
    WebEnvironmentManager::printSummary(checks, "CHECK SUMMARY");

    std::vector<SetupStep> allSteps = setup;
    allSteps.insert(allSteps.end(), checks.begin(), checks.end());
    std::string errorOutput = collectErrorOutput(allSteps);

    ChecklistReader reader;
    auto checklist = reader.load();
    ErrorMatcher matcher(checklist);

    std::vector<std::string> detectedErrors = matchAll(matcher, errorOutput, languages);
    std::vector<CodeError> codeErrors = hooks.parseErrors(errorOutput);

    std::cout << "\n========== DETECTED ERRORS ==========\n";
    if (detectedErrors.empty()) std::cout << "No known errors detected.\n";
    for (const auto& e : detectedErrors) {
        std::cout << "- " << e << " (" << ErrorCollector::difficultyOf(e) << ")"
                  << (isEnvironmentError(e) ? " [environment]" : "") << "\n";
    }
    std::cout << "\nErrors with file and line: " << codeErrors.size() << "\n";
    for (size_t i = 0; i < codeErrors.size() && i < 20; ++i) {
        std::cout << "  " << codeErrors[i].file << ":" << codeErrors[i].line;
        if (!codeErrors[i].component.empty()) {
            std::cout << "  [" << codeErrors[i].component << " / " << codeErrors[i].tool << "]";
        }
        std::cout << "\n";
    }
    std::cout << "=====================================\n\n";

    // Fehler-/Fix-Abschnitte des Reports (docs/PLAN_REPORT_REDESIGN.md)
    const PdfExportConfig reportConfig = options.pdf ? options.pdf->config() : PdfReportExporter::loadConfig(targetPath);
    FixReportBuilder fixReport(targetPath, reportConfig.contextLines, reportConfig.language);
    fixReport.setPipeline(LlmProvider::pipelineEnabled());
    fixReport.setDescriber([&](const std::string& name) {
        ErrorInfo info;
        const std::vector<std::string> keywords = keywordsOf(name, checklist);
        const CodeError* hit = nullptr;
        for (const auto& e : codeErrors) {
            const std::string msg = toLower(e.message);
            const bool match = std::any_of(keywords.begin(), keywords.end(),
                                           [&](const std::string& k) { return msg.find(k) != std::string::npos; });
            if (!match) continue;
            if (!hit) hit = &e;
            if (e.file == hit->file && std::find(info.lines.begin(), info.lines.end(), e.line) == info.lines.end())
                info.lines.push_back(e.line);
        }
        if (!hit) {
            // no message names the error: the first located error of the step that does
            const std::vector<std::string> files = filesForError(name, codeErrors, checklist, errorOutput);
            for (const auto& e : codeErrors) {
                if (!files.empty() && e.file == files.front()) { hit = &e; info.lines.push_back(e.line); break; }
            }
        }
        if (hit) {
            info.file = hit->file;
            info.severity = hit->severity;
            info.tool = hit->tool.empty() ? hit->step : hit->tool;
            if (!hit->component.empty() && !hit->tool.empty()) info.tool += " (" + hit->component + ")";
            info.language = LanguageDetector::languageOf(hit->file);
        }
        info.message = FixReportBuilder::excerptMessage(errorOutput, keywords, info.file, 14,
                                                        fs::absolute(targetPath).lexically_normal().string() + "/");
        return info;
    });
    fixReport.observe(detectedErrors);
    const nlohmann::json reportMeta = {
        {"project", fs::path(targetPath).lexically_normal().filename().string()},
        {"target", targetPath},
        {"fix_description", options.fixDescription},
        {"languages", [&] { std::string l; for (const auto& x : languages) l += (l.empty() ? "" : ", ") + x; return l; }()},
        {"run_id", options.pdf ? options.pdf->runId() : std::string()},
        {"models", OpenAIClient::describeModels()}
    };

    bool allStepsOk = std::all_of(allSteps.begin(), allSteps.end(), [](const SetupStep& s) { return s.ok(); });
    if (detectedErrors.empty() && allStepsOk) {
        std::cout << "No errors found: setup, static analysis, build and runtime checks passed.\n";
        if (!options.jsonOutFile.empty()) {
            json report;
            report["target"] = targetPath;
            report["languages"] = std::vector<std::string>(languages.begin(), languages.end());
            report["steps"] = json::array();
            for (const auto& s : allSteps) {
                report["steps"].push_back({{"name", s.name}, {"command", s.command}, {"exit_code", s.result.exit_code}});
            }
            report["detected_errors"] = json::array();
            if (hooks.extendReport) hooks.extendReport(report);
            fixReport.finalize({});
            report["fix_report"] = fixReport.toJson(reportMeta);
            std::ofstream(options.jsonOutFile) << report.dump(2);
            std::cout << "JSON Report gespeichert: " << options.jsonOutFile << "\n";
            if (options.pdf) options.pdf->exportJson(report.dump(2), options.jsonOutFile);
        }
        return 0;
    }

    std::vector<std::string> giveUp;
    std::vector<std::string> fixedErrors;
    FileCollector collector;

    // name of the PDF when no --json-out file exists, and counter of the fix iterations
    const std::string pdfName = fs::path(targetPath).lexically_normal().filename().string() + "_report";
    int pdfIteration = 0;

    if (!detectedErrors.empty()) {
        OpenAIClient fixClient;
        std::string checklistRaw = reader.loadRaw();

        const int MAX_ROUNDS = 20;
        const int MAX_ITER = 5;

        auto redetect = [&](const std::string& changedFile) {
            checks = hooks.check(changedFile);
            allSteps = setup;
            allSteps.insert(allSteps.end(), checks.begin(), checks.end());
            errorOutput = collectErrorOutput(allSteps);
            codeErrors = hooks.parseErrors(errorOutput);
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
                fixReport.skipError(current, fixReport.tr("Setup-/Umgebungsproblem, kein Code-Fehler - kein Fix-Versuch.",
                                                          "Setup/environment problem, not a code bug - no fix attempt."));
                giveUp.push_back(current);
                continue;
            }

            std::string cause = environmentCause(current, checklist, errorOutput);
            if (!cause.empty()) {
                std::cout << "[ROOT CAUSE] " << current << " is caused by " << cause
                          << " (same step), which is an environment problem. Skipping fix attempts.\n";
                fixReport.skipError(current, fixReport.tr("Folgefehler des Umgebungsproblems ", "Caused by the environment problem ")
                                             + cause + fixReport.tr(" (gleicher Schritt) - kein Fix-Versuch.", " (same step) - no fix attempt."));
                giveUp.push_back(current);
                continue;
            }

            std::vector<std::string> relevantFiles = filesForError(current, codeErrors, checklist, errorOutput);
            if (relevantFiles.empty()) {
                std::cout << "No source file found in the output for " << current << ", skipping.\n";
                fixReport.skipError(current, fixReport.tr("Keine Quelldatei in der Ausgabe gefunden - kein Fix-Versuch.",
                                                          "No source file found in the output - no fix attempt."));
                giveUp.push_back(current);
                continue;
            }

            round++;
            bool fixed = false;
            const int reportId = fixReport.beginError(current);

            std::vector<std::string> keywords = keywordsOf(current, checklist);

            for (int attempt = 1; attempt <= MAX_ITER; ++attempt) {
                std::cout << "Attempt " << attempt << "/" << MAX_ITER << " for error: " << current
                          << " (difficulty: " << ErrorCollector::difficultyOf(current) << ")\n";
                std::cout << "  Files sent to the AI:";
                for (const auto& f : relevantFiles) std::cout << " " << f;
                std::cout << "\n";

                // Additional context (fullstack projects): code under test and related
                // files of the components on the other side of a boundary, appended after
                // the files that contain the error.
                FixContext context;
                if (hooks.context) context = hooks.context(current, relevantFiles, codeErrors);
                std::vector<std::string> requestFiles = relevantFiles;
                for (const auto& f : context.files) {
                    if (std::find(requestFiles.begin(), requestFiles.end(), f) == requestFiles.end()) {
                        requestFiles.push_back(f);
                    }
                }
                if (requestFiles.size() > relevantFiles.size()) {
                    std::cout << "  Additional context files:";
                    for (size_t i = relevantFiles.size(); i < requestFiles.size(); ++i) std::cout << " " << requestFiles[i];
                    std::cout << "\n";
                }

                std::stringstream source;
                for (const auto& f : requestFiles) {
                    source << "===== FILE: " << f << " =====\n";
                    source << collector.readSourceCode((fs::path(targetPath) / f).string()) << "\n\n";
                }

                FixRequest req;
                req.error_name = current;
                req.error_output = relevantOutput(errorOutput, relevantFiles, keywords);
                req.language = LanguageDetector::languageOf(relevantFiles.front());
                req.source_code = limit(source.str(), 80000);
                req.checklist = checklistRaw;
                req.cross_context = context.note;
                req.explanation_language = reportConfig.language == "en" ? "English" : "German";

                FixResult res = fixClient.fix_code(req);
                if (!res.success || res.fixed_code.empty()) {
                    fixReport.addAttempt(reportId, attempt, res, "", false, false, {}, {});
                    continue;
                }

                CodeChanger changer(targetPath);
                try {
                    changer.apply_fix(res);
                    std::cout << "  -> fix applied to " << res.file_path << "\n";
                } catch (const std::exception& e) {
                    std::cerr << "  -> could not apply fix: " << e.what() << "\n";
                    fixReport.addAttempt(reportId, attempt, res, "", false, false, {}, {}, e.what());
                    continue;
                }

                redetect(res.file_path);
                bool gone = std::find(detectedErrors.begin(), detectedErrors.end(), current) == detectedErrors.end();
                std::vector<std::string> failedChecks;
                for (const auto& step : allSteps) {
                    if (!step.ok()) failedChecks.push_back(step.name);
                }
                fixReport.addAttempt(reportId, attempt, res, changer.previousContent(), true, gone, detectedErrors, failedChecks);
                fixReport.observe(detectedErrors, reportId);

                // optional PDF of the state after this fix iteration (pdf_export.per_iteration)
                if (options.pdf && options.pdf->config().perIteration) {
                    options.pdf->exportIteration(++pdfIteration, {
                        {"iteration", pdfIteration}, {"error", current}, {"attempt", attempt},
                        {"file_path", res.file_path}, {"error_fixed", gone},
                        {"remaining_errors", detectedErrors}, {"fixed_code", res.fixed_code},
                        {"error_output", limit(errorOutput, 20000)}
                    }, options.jsonOutFile, pdfName);
                }
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

            fixReport.finishError(reportId, fixed);
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
        json entry = {
            {"file", codeErrors[i].file}, {"line", codeErrors[i].line}, {"message", codeErrors[i].message}
        };
        if (!codeErrors[i].tool.empty()) {
            entry["column"] = codeErrors[i].column;
            entry["severity"] = codeErrors[i].severity;
            entry["tool"] = codeErrors[i].tool;
            entry["service"] = codeErrors[i].service;
            entry["component"] = codeErrors[i].component;
        }
        report["errors_with_location"].push_back(entry);
        if (reportFiles.size() < 5) reportFiles.insert(codeErrors[i].file);
    }
    std::string sourceCode;
    for (const auto& f : reportFiles) {
        sourceCode += "===== FILE: " + f + " =====\n" + collector.readSourceCode((fs::path(targetPath) / f).string()) + "\n\n";
    }
    report["source_code"] = limit(sourceCode, 60000);
    if (hooks.extendReport) hooks.extendReport(report);

    fixReport.finalize(detectedErrors);
    const json fixReportJson = fixReport.toJson(reportMeta);

    // The written report also has the fix sections; the request to the model below does not
    // (they would only repeat the code it gets anyway).
    auto writeJson = [&]() {
        json written = report;
        written["fix_report"] = fixReportJson;
        if (!options.jsonOutFile.empty()) {
            std::ofstream out(options.jsonOutFile);
            out << written.dump(2);
            std::cout << "JSON Report gespeichert: " << options.jsonOutFile << "\n";
        }
        // PDF next to the JSON (or into the configured folder); never aborts the run
        if (options.pdf) options.pdf->exportJson(written.dump(2), options.jsonOutFile, pdfName);
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
