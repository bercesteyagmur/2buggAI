#include "FixRecord.h"
#include "ErrorCollector.h"
#include "LanguageDetector.h"
#include "PdfReportExporter.h"
#include "UnifiedDiff.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

constexpr std::uintmax_t MAX_FILE_BYTES = 5 * 1024 * 1024;
constexpr size_t MAX_MESSAGE_LINE = 300;

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string trimRight(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
    return s;
}

bool contains(const std::vector<std::string>& list, const std::string& value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

void logFailure(const char* where, const std::exception* e) {
    std::cerr << "[REPORT] " << where << " fehlgeschlagen";
    if (e) std::cerr << " (" << e->what() << ")";
    std::cerr << " - der Run läuft weiter\n";
}

json explanationJson(const FixExplanation& e) {
    return {{"root_cause_explanation", e.root_cause}, {"fix_explanation", e.fix},
            {"affected_lines", e.affected_lines}, {"error_category", e.error_category},
            {"side_effects", e.side_effects}};
}

} // namespace

FixReportBuilder::FixReportBuilder(std::string projectRoot, int contextLines, std::string language)
    : root_(std::move(projectRoot)), context_(std::clamp(contextLines, 0, 20)), language_(std::move(language)) {}

// ---------------------------------------------------------------------------
// building blocks
// ---------------------------------------------------------------------------

std::vector<std::string> FixReportBuilder::keywordsFor(const std::string& errorName,
                                                       const std::vector<ErrorCategory>& checklist) {
    std::vector<std::string> keywords;
    for (const auto& c : checklist) {
        if (c.name != errorName) continue;
        for (const auto& k : c.keywords) {
            std::string lower = toLower(k);
            const size_t start = lower.find_first_not_of(" \t");
            const size_t end = lower.find_last_not_of(" \t\r");
            if (start == std::string::npos) continue;
            lower = lower.substr(start, end - start + 1);
            if (lower != "-" && !contains(keywords, lower)) keywords.push_back(lower);
        }
    }
    return keywords;
}

std::string FixReportBuilder::excerptMessage(const std::string& output, const std::vector<std::string>& keywords,
                                             const std::string& file, size_t maxLines, const std::string& stripPrefix) {
    std::vector<std::string> lines;
    {
        std::istringstream in(output);
        for (std::string line; std::getline(in, line);) {
            line = trimRight(line);
            if (line.rfind("===== STEP: ", 0) == 0) continue;   // section headers of the tool itself
            if (!stripPrefix.empty()) {
                for (size_t pos = line.find(stripPrefix); pos != std::string::npos; pos = line.find(stripPrefix, pos)) {
                    line.erase(pos, stripPrefix.size());
                }
            }
            if (line.size() > MAX_MESSAGE_LINE) line = line.substr(0, MAX_MESSAGE_LINE) + " ...";
            lines.push_back(std::move(line));
        }
    }
    const std::string base = file.empty() ? "" : fs::path(file).filename().string();

    auto lower = [&](size_t i) { return toLower(lines[i]); };
    auto hasKeyword = [&](size_t i) {
        const std::string l = lower(i);
        return std::any_of(keywords.begin(), keywords.end(),
                           [&](const std::string& k) { return !k.empty() && l.find(k) != std::string::npos; });
    };
    auto trimmedStartsWith = [&](size_t i, const char* prefix) {
        const size_t start = lines[i].find_first_not_of(" \t");
        return start != std::string::npos && lines[i].compare(start, std::char_traits<char>::length(prefix), prefix) == 0;
    };
    // a diagnostic of its own ("file:12:3: error: ...", "ERROR ..."): ends the context of the one below
    auto isDiagnostic = [&](size_t i) {
        const std::string l = lower(i);
        return l.find("error:") != std::string::npos || l.find("error ") == 0 || l.find("warning:") != std::string::npos;
    };
    // context lines above a message: "main.cpp: In function 'int main()':", traceback frames
    auto isContext = [&](size_t i) {
        if (base.empty() || lines[i].find(base) == std::string::npos) return false;
        return lines[i].find(": In ") != std::string::npos || trimmedStartsWith(i, "File \"") || trimmedStartsWith(i, "at ");
    };

    std::set<size_t> picked;
    auto pick = [&](size_t i) { if (i < lines.size()) picked.insert(i); };
    auto pickWithFollow = [&](size_t i) {
        pick(i);
        // compiler: source line and caret after the message; traceback: the code line after "File ..."
        if (i + 1 < lines.size() && !lines[i + 1].empty() && (lines[i + 1][0] == ' ' || lines[i + 1][0] == '\t')) pick(i + 1);
        if (i + 2 < lines.size() && lines[i + 2].find('^') != std::string::npos
            && lines[i + 2].find_first_not_of(" \t|^~0123456789") == std::string::npos) pick(i + 2);
    };

    std::vector<size_t> hits;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (hasKeyword(i)) hits.push_back(i);
    }
    for (size_t h : hits) {
        pickWithFollow(h);
        for (size_t j = h; j-- > 0 && h - j <= 8;) {
            if (lines[j].empty() || hasKeyword(j)) break;
            if (isContext(j)) { pickWithFollow(j); continue; }
            if (isDiagnostic(j)) break;
        }
    }
    if (hits.empty()) {
        // no keyword in the output: the lines that name the file, and exception lines
        static const std::regex exceptionRe(R"re(^[A-Za-z_][\w.]*(Error|Exception|Exit|Interrupt)(:.*)?$)re");
        for (size_t i = 0; i < lines.size(); ++i) {
            if (!base.empty() && lines[i].find(base) != std::string::npos) pickWithFollow(i);
            else if (lines[i].size() < 200 && std::regex_match(lines[i], exceptionRe)) pick(i);
        }
    }
    if (picked.empty()) {
        for (size_t i = 0; i < lines.size() && picked.size() < maxLines; ++i) {
            if (lines[i].find_first_not_of(" \t") != std::string::npos) pick(i);
        }
    }

    std::string out;
    size_t shown = 0;
    size_t previous = 0;
    bool first = true;
    for (size_t i : picked) {
        if (shown >= maxLines) {
            out += "...\n";
            break;
        }
        if (!first && i > previous + 1) out += "...\n";
        out += lines[i] + "\n";
        previous = i;
        first = false;
        ++shown;
    }
    return out;
}

std::string FixReportBuilder::makeTitle(const std::string& errorName, const std::string& file,
                                        const std::string& message) {
    static const std::regex exceptionRe(R"re(\b([A-Z][A-Za-z0-9_]*(?:Error|Exception)))re");
    std::string name = errorName;
    std::smatch m;
    if (message.size() < 20000 && std::regex_search(message, m, exceptionRe)) name = m[1].str();
    return file.empty() ? name : name + " in " + file;
}

// ---------------------------------------------------------------------------
// records
// ---------------------------------------------------------------------------

std::string FixReportBuilder::readFile(const std::string& path) const {
    if (path.empty()) return "";
    fs::path p(path);
    if (p.is_relative() && !root_.empty() && !fs::exists(p)) p = fs::path(root_) / p;
    std::error_code ec;
    if (!fs::is_regular_file(p, ec) || fs::file_size(p, ec) > MAX_FILE_BYTES) return "";
    std::ifstream in(p, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

ErrorInfo FixReportBuilder::describe(const std::string& errorName) const {
    ErrorInfo info;
    if (describer_) {
        try {
            info = describer_(errorName);
        } catch (const std::exception& e) {
            logFailure("Fehlerbeschreibung", &e);
        } catch (...) {
            logFailure("Fehlerbeschreibung", nullptr);
        }
    }
    if (info.language.empty() && !info.file.empty()) info.language = LanguageDetector::languageOf(info.file);
    if (info.severity.empty()) info.severity = "error";
    return info;
}

FixRecord* FixReportBuilder::find(int id) {
    if (id < 1 || static_cast<size_t>(id) > records_.size()) return nullptr;
    return &records_[static_cast<size_t>(id) - 1];
}

FixRecord& FixReportBuilder::newRecord(const std::string& errorName, const ErrorInfo& info) {
    FixRecord r;
    r.id = static_cast<int>(records_.size()) + 1;
    r.errorName = errorName;
    r.difficulty = ErrorCollector::difficultyOf(errorName);
    r.info = info;
    r.title = makeTitle(errorName, info.file, info.message);
    records_.push_back(std::move(r));
    return records_.back();
}

void FixReportBuilder::observe(const std::vector<std::string>& errors, int causedBy) noexcept {
    try {
        for (const auto& name : errors) {
            auto it = std::find_if(seen_.begin(), seen_.end(), [&](const Seen& s) { return s.name == name; });
            if (it == seen_.end()) seen_.push_back({name, describe(name), 0});
            else it->resolvedBy = 0;   // present (again)
        }
        if (causedBy > 0) {
            for (const auto& name : lastObserved_) {
                if (contains(errors, name)) continue;
                for (auto& s : seen_) {
                    if (s.name == name) s.resolvedBy = causedBy;
                }
            }
        }
        lastObserved_ = errors;
    } catch (const std::exception& e) {
        logFailure("observe", &e);
    } catch (...) {
        logFailure("observe", nullptr);
    }
}

int FixReportBuilder::beginError(const std::string& errorName) noexcept {
    try {
        FixRecord& r = newRecord(errorName, describe(errorName));
        if (!r.info.file.empty() && !r.info.lines.empty()) {
            r.excerpt = FixSnippet::excerpt(readFile(r.info.file), r.info.lines, context_);
        }
        return r.id;
    } catch (const std::exception& e) {
        logFailure("beginError", &e);
    } catch (...) {
        logFailure("beginError", nullptr);
    }
    return 0;
}

int FixReportBuilder::skipError(const std::string& errorName, const std::string& reason) noexcept {
    const int id = beginError(errorName);
    if (FixRecord* r = find(id)) {
        r->status = "not_fixed";
        r->statusReason = reason;
    }
    return id;
}

void FixReportBuilder::addAttempt(int id, int number, const FixResult& result, const std::string& originalCode,
                                  bool applied, bool errorGone, const std::vector<std::string>& remainingErrors,
                                  const std::vector<std::string>& failedChecks, const std::string& applyError,
                                  const std::string& stillPresentReason) noexcept {
    try {
        FixRecord* r = find(id);
        if (!r) return;
        const bool de = language_ != "en";

        FixAttempt a;
        a.number = number;
        a.applied = applied;
        a.errorGone = applied && errorGone;
        a.explanation = result.explanation;
        a.review = result.review;
        a.verification = result.verification;
        a.verificationExpected = pipeline_;

        // project-relative path for the report
        if (!result.file_path.empty()) {
            fs::path p(result.file_path);
            std::error_code ec;
            if (!root_.empty() && (p.is_absolute() || fs::exists(p, ec))) {
                const fs::path rel = fs::weakly_canonical(p, ec).lexically_relative(fs::weakly_canonical(root_, ec));
                a.file = (!rel.empty() && rel.string().rfind("..", 0) != 0) ? rel.generic_string() : result.file_path;
            } else {
                a.file = result.file_path;
            }
        }

        if (applied) {
            a.outcome = a.errorGone ? "fixed" : "still_present";
            if (!a.errorGone) a.reason = !stillPresentReason.empty() ? stillPresentReason
                : (de ? "Fehler ist nach dem Re-Run weiterhin vorhanden" : "Error still present after the re-run");
            a.remainingErrors = remainingErrors;
            a.failedChecks = failedChecks;
        } else if (!applyError.empty()) {
            a.outcome = "apply_failed";
            a.reason = (de ? "Fix konnte nicht geschrieben werden: " : "Fix could not be written: ") + applyError;
        } else {
            a.outcome = result.fixed_code.empty() ? "no_fix" : "rejected";
            a.reason = !result.failure_reason.empty() ? result.failure_reason
                     : (de ? "Kein verwertbarer Fix geliefert" : "No usable fix returned");
        }

        if (!result.fixed_code.empty()) {
            const std::string original = !originalCode.empty() ? originalCode : readFile(result.file_path);
            if (!original.empty()) {
                const bool sameFile = !r->info.file.empty() && !a.file.empty()
                    && fs::path(r->info.file).lexically_normal() == fs::path(a.file).lexically_normal();
                const std::vector<int> errorLines = (sameFile && !r->info.lines.empty())
                    ? r->info.lines : result.explanation.affected_lines;
                a.snippet = FixSnippet::build(original, result.fixed_code, errorLines, context_);
                a.diff = unified_diff(original, result.fixed_code, a.file.empty() ? result.file_path : a.file);

                // location unknown from the tool output (e.g. runtime errors): take it from the fix
                if (r->info.file.empty() && !a.file.empty()) {
                    r->info.file = a.file;
                    r->info.lines = !result.explanation.affected_lines.empty()
                        ? result.explanation.affected_lines : FixSnippet::changedOldLines(original, result.fixed_code);
                    if (r->info.language.empty()) r->info.language = LanguageDetector::languageOf(a.file);
                    r->title = makeTitle(r->errorName, r->info.file, r->info.message);
                }
            }
        }
        r->attempts.push_back(std::move(a));
    } catch (const std::exception& e) {
        logFailure("addAttempt", &e);
    } catch (...) {
        logFailure("addAttempt", nullptr);
    }
}

void FixReportBuilder::finishError(int id, bool fixed, const std::string& reason) noexcept {
    try {
        FixRecord* r = find(id);
        if (!r) return;
        const bool de = language_ != "en";
        r->status = fixed ? "fixed" : "not_fixed";
        if (!reason.empty()) {
            r->statusReason = reason;
        } else if (!fixed) {
            const std::string last = r->attempts.empty() ? "" : r->attempts.back().reason;
            r->statusReason = (de ? "Nach " : "Not fixed after ") + std::to_string(r->attempts.size())
                + (de ? " Versuch(en) nicht behoben." : " attempt(s).");
            if (!last.empty()) r->statusReason += (de ? " Letzter Grund: " : " Last reason: ") + last;
        }
    } catch (const std::exception& e) {
        logFailure("finishError", &e);
    } catch (...) {
        logFailure("finishError", nullptr);
    }
}

void FixReportBuilder::finalize(const std::vector<std::string>& finalErrors, const std::string& unhandledReason) noexcept {
    try {
        const bool de = language_ != "en";
        auto resolvedText = [&](int by) {
            if (by <= 0) return std::string(de ? "Ohne eigenen Fix verschwunden (durch andere Fixes behoben)."
                                               : "Disappeared without a fix of its own (fixed by other fixes).");
            return (de ? "Mit dem Fix in Abschnitt " : "Fixed together with the fix in section ") + std::to_string(by)
                 + (de ? " behoben (dieselbe Ursache bzw. Meldung)." : " (same cause or message).");
        };

        // given up, but a later fix removed it
        for (auto& r : records_) {
            if (r.status == "fixed" || contains(finalErrors, r.errorName)) continue;
            const bool lastOfName = std::none_of(records_.begin() + r.id, records_.end(),
                                                 [&](const FixRecord& o) { return o.errorName == r.errorName; });
            auto it = std::find_if(seen_.begin(), seen_.end(), [&](const Seen& s) { return s.name == r.errorName; });
            if (!lastOfName || it == seen_.end() || it->resolvedBy <= 0 || it->resolvedBy == r.id) continue;
            r.status = "fixed";
            r.resolvedBy = it->resolvedBy;
            r.statusReason = (r.attempts.empty() ? "" : r.statusReason + " ") + resolvedText(it->resolvedBy);
        }

        // seen, but never worked on
        for (const auto& s : seen_) {
            const bool hasRecord = std::any_of(records_.begin(), records_.end(),
                                               [&](const FixRecord& r) { return r.errorName == s.name; });
            if (hasRecord) continue;
            FixRecord& r = newRecord(s.name, s.info);
            if (contains(finalErrors, s.name)) {
                r.status = "not_fixed";
                r.statusReason = !unhandledReason.empty() ? unhandledReason
                    : (de ? "Nicht bearbeitet (Rundenlimit des Fix-Loops erreicht)." : "Not handled (round limit of the fix loop reached).");
            } else {
                r.status = "fixed";
                r.resolvedBy = s.resolvedBy;
                r.statusReason = resolvedText(s.resolvedBy);
            }
        }
    } catch (const std::exception& e) {
        logFailure("finalize", &e);
    } catch (...) {
        logFailure("finalize", nullptr);
    }
}

json FixReportBuilder::recordToJson(const FixRecord& r) {
    json attempts = json::array();
    for (const auto& a : r.attempts) {
        json review = {{"ran", a.review.ran}, {"approved", a.review.approved}, {"verdict", a.review.verdict},
                       {"comment", a.review.comment}, {"findings", a.review.findings}, {"revised", a.review.revised}};
        json verification = {{"ran", a.verification.ran}, {"confirmed", a.verification.confirmed},
                             {"reason", a.verification.reason}, {"remaining_issues", a.verification.remaining_issues}};
        attempts.push_back({
            {"number", a.number}, {"file", a.file}, {"applied", a.applied}, {"error_gone", a.errorGone},
            {"outcome", a.outcome}, {"reason", a.reason}, {"explanation", explanationJson(a.explanation)},
            {"agent_pipeline", a.verificationExpected}, {"review", review}, {"verification", verification},
            {"remaining_errors", a.remainingErrors}, {"failed_checks", a.failedChecks},
            {"snippet", FixSnippet::toJson(a.snippet)}, {"diff", a.diff}
        });
    }
    return {
        {"id", r.id}, {"title", r.title}, {"error_name", r.errorName}, {"category", r.errorName},
        {"difficulty", r.difficulty}, {"file", r.info.file}, {"lines", r.info.lines},
        {"language", r.info.language}, {"tool", r.info.tool}, {"severity", r.info.severity},
        {"status", r.status}, {"status_reason", r.statusReason}, {"resolved_by", r.resolvedBy},
        {"message", r.info.message}, {"iterations", r.attempts.size()},
        {"excerpt", FixSnippet::toJson(r.excerpt)}, {"attempts", attempts}
    };
}

json FixReportBuilder::toJson(const json& meta) const noexcept {
    try {
        json out = {{"schema", 1}};
        if (meta.is_object()) {
            for (auto it = meta.begin(); it != meta.end(); ++it) out[it.key()] = it.value();
        }
        out["language"] = language_;
        out["context_lines"] = context_;

        size_t fixed = 0;
        json fixes = json::array();
        for (const auto& r : records_) {
            if (r.status == "fixed") ++fixed;
            fixes.push_back(recordToJson(r));
        }
        out["summary"] = {{"found", records_.size()}, {"fixed", fixed}, {"not_fixed", records_.size() - fixed}};
        out["fixes"] = fixes;
        // the JSON report is written as it is: no keys or passwords from code or messages in it
        return json(PdfReportExporter::maskSecrets(nlohmann::ordered_json(out)));
    } catch (const std::exception& e) {
        logFailure("toJson", &e);
    } catch (...) {
        logFailure("toJson", nullptr);
    }
    return json::object();
}
