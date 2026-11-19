#include "ErrorDetection.h"

#include "ErrorCollector.h"
#include "ErrorMatcher.h"

#include <algorithm>
#include <cctype>
#include <map>

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

} // namespace

namespace ErrorDetection {

std::vector<std::string> environmentProblems(const std::vector<ErrorCategory>& checklist,
                                             const std::string& output, const std::string& language) {
    std::vector<std::string> found;
    if (output.empty()) return found;
    const std::string text = lower(output);
    for (const auto& c : checklist) {
        if (c.detection_type != "environment") continue;
        if (!c.language.empty() && c.language != "general" && c.language != language) continue;
        for (const auto& k : c.keywords) {
            if (!k.empty() && text.find(lower(k)) != std::string::npos) {
                if (!contains(found, c.name)) found.push_back(c.name);
                break;
            }
        }
    }
    return found;
}

Result detect(const std::vector<ErrorCategory>& checklist, const std::string& language,
              const std::string& buildOutput, bool buildFailed,
              const std::string& runOutput, bool runFailed) {
    ErrorMatcher matcher(checklist);
    std::vector<std::string> buildErrors = matcher.match(buildOutput, language);
    std::vector<std::string> runErrors   = matcher.match(runOutput, language);

    // Unknown failure: never "no errors" when the build failed or the program crashed.
    if (buildFailed && buildErrors.empty()) buildErrors.push_back(kUnrecognizedBuild);
    if (!buildFailed && runFailed && runErrors.empty()) runErrors.push_back(kUnrecognizedRuntime);

    const std::vector<std::string> buildEnv = environmentProblems(checklist, buildOutput, language);
    const std::vector<std::string> runEnv   = environmentProblems(checklist, runOutput, language);

    Result r;
    std::vector<std::string> all = buildErrors;
    for (const auto& e : runErrors) if (!contains(all, e)) all.push_back(e);
    ErrorCollector collector;
    r.errors = collector.sortedErrors(all);

    r.environment = buildEnv;
    for (const auto& e : runEnv) if (!contains(r.environment, e)) r.environment.push_back(e);

    // An error is caused by the environment when every part of the output it shows up in
    // (build, run) also reports an environment problem.
    for (const auto& e : r.errors) {
        const bool inBuild = contains(buildErrors, e);
        const bool inRun   = contains(runErrors, e);
        if ((!inBuild || !buildEnv.empty()) && (!inRun || !runEnv.empty())) r.environmentCaused.insert(e);
    }
    return r;
}

std::string environmentHint(const std::string& name) {
    static const std::map<std::string, std::string> hints = {
        {"env_service_unreachable",
         "Ein Dienst (z. B. Datenbank) ist nicht erreichbar: starten und Host/Port prüfen, dann erneut ausführen."},
        {"env_db_authentication",
         "Anmeldung an der Datenbank fehlgeschlagen: Benutzer/Passwort/Datenbankname der Umgebung prüfen."},
        {"env_port_in_use",
         "Der Port ist schon belegt: den laufenden Prozess beenden oder einen anderen Port verwenden."},
        {"env_unknown_host",
         "Hostname nicht auflösbar bzw. kein Netzwerk: Netzwerk/Proxy/Hostnamen prüfen."},
        {"env_command_not_found",
         "Ein benötigtes Programm ist nicht installiert oder nicht im PATH."},
    };
    const auto it = hints.find(name);
    return it != hints.end() ? it->second : "Problem der Laufzeitumgebung, kein Fehler im Code.";
}

} // namespace ErrorDetection
