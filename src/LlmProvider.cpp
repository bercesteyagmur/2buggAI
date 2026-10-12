#include "LlmProvider.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include <vector>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

using nlohmann::json;

extern char** environ;

namespace {

constexpr int MAX_HTTP_ATTEMPTS = 4;       // 1 Versuch + 3 Retries bei 429/5xx/Netzwerkfehler
constexpr long HTTP_TIMEOUT_SEC = 180;     // Thinking-Modelle brauchen teils deutlich > 60s

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

std::string env(const char* key) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : std::string();
}

std::string toLower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string stripTrailingSlash(std::string s) {
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

std::string truncate(const std::string& s, size_t max) {
    return s.size() <= max ? s : s.substr(0, max) + " ...";
}

// API-Key aus Texten entfernen, bevor sie geloggt werden
std::string redact(std::string s, const std::string& secret) {
    if (secret.size() < 4) return s;
    size_t pos = 0;
    while ((pos = s.find(secret, pos)) != std::string::npos) {
        s.replace(pos, secret.size(), "***");
        pos += 3;
    }
    return s;
}

// Fehlertext aus typischen Fehlerantworten (OpenAI, Anthropic, Gemini) holen
std::string apiErrorMessage(const std::string& body) {
    json j = json::parse(body, nullptr, false);
    if (!j.is_discarded()) {
        if (j.is_array() && !j.empty()) j = j[0];
        if (j.is_object() && j.contains("error")) {
            const json& e = j["error"];
            if (e.is_object() && e.contains("message") && e["message"].is_string())
                return e["message"].get<std::string>();
            if (e.is_string()) return e.get<std::string>();
        }
        if (j.is_object() && j.contains("message") && j["message"].is_string())
            return j["message"].get<std::string>();
    }
    return truncate(body, 500);
}

// ---------- Nutzungszähler ----------

struct Usage {
    long calls = 0;
    long failed = 0;
    long prompt_tokens = 0;
    long completion_tokens = 0;
};

std::map<std::string, Usage>& usageTable();

void printUsage() {
    const auto& table = usageTable();
    if (table.empty()) return;

    Usage total;
    std::cout << "\n========== LLM-NUTZUNG ==========\n";
    for (const auto& [key, u] : table) {
        std::cout << "  " << key << ": " << u.calls << " Aufruf(e)";
        if (u.failed) std::cout << " (" << u.failed << " fehlgeschlagen)";
        std::cout << ", Tokens: " << (u.prompt_tokens + u.completion_tokens)
                  << " (in " << u.prompt_tokens << " / out " << u.completion_tokens << ")\n";
        total.calls += u.calls;
        total.failed += u.failed;
        total.prompt_tokens += u.prompt_tokens;
        total.completion_tokens += u.completion_tokens;
    }
    std::cout << "  Gesamt: " << total.calls << " Aufruf(e), Tokens: "
              << (total.prompt_tokens + total.completion_tokens) << "\n";
    std::cout << "=================================\n";
}

std::map<std::string, Usage>& usageTable() {
    static std::map<std::string, Usage> table;
    static const bool registered = (std::atexit(printUsage) == 0);
    (void)registered;
    return table;
}

// ---------- Konfiguration ----------

struct ProviderSpec {
    const char* name;
    const char* key_var;
    const char* model_var;
    const char* default_model;   // nullptr = Pflicht
    const char* endpoint_var;
    const char* default_endpoint;
    bool max_completion_tokens;
};

const std::vector<ProviderSpec>& providerSpecs() {
    static const std::vector<ProviderSpec> specs = {
        {"gemini", "GEMINI_API_KEY", "GEMINI_MODEL", nullptr,
         "BUGGAI_GEMINI_ENDPOINT", "https://generativelanguage.googleapis.com/v1beta/openai/chat/completions", false},
        {"claude", "CLAUDE_API_KEY", "CLAUDE_MODEL", "claude-sonnet-5-5",
         "BUGGAI_CLAUDE_ENDPOINT", "https://api.anthropic.com/v1/chat/completions", false},
        {"openai", "OPENAI_API_KEY", "OPENAI_MODEL", "gpt-5.2",
         "BUGGAI_OPENAI_ENDPOINT", "https://api.openai.com/v1/chat/completions", true},
    };
    return specs;
}

struct RoleSpec {
    const char* role;
    const char* var;
    const char* default_provider;
};

const std::vector<RoleSpec>& roleSpecs() {
    static const std::vector<RoleSpec> specs = {
        {"Debugger", "BUGGAI_DEBUGGER", "gemini"},
        {"Reviewer", "BUGGAI_REVIEWER", "claude"},
        {"Verifier", "BUGGAI_VERIFIER", "openai"},
    };
    return specs;
}

// Liefert die Konfiguration oder eine Liste von Problemen (fehlende Variablen etc.)
LlmConfig resolveRole(const std::string& role, std::vector<std::string>& problems) {
    const RoleSpec* rs = nullptr;
    for (const auto& r : roleSpecs())
        if (role == r.role) rs = &r;
    if (!rs) {
        problems.push_back("unbekannte Rolle " + role);
        return {};
    }

    std::string providerName = toLower(env(rs->var));
    if (providerName.empty()) providerName = rs->default_provider;

    const ProviderSpec* ps = nullptr;
    for (const auto& p : providerSpecs())
        if (providerName == p.name) ps = &p;
    if (!ps) {
        problems.push_back(std::string(rs->var) + "=" + providerName +
                           " ist ungültig (erlaubt: gemini, claude, openai)");
        return {};
    }

    LlmConfig cfg;
    cfg.provider = ps->name;
    cfg.api_key = env(ps->key_var);
    cfg.model = env(ps->model_var);
    if (cfg.model.empty() && ps->default_model) cfg.model = ps->default_model;
    cfg.endpoint = env(ps->endpoint_var);
    if (cfg.endpoint.empty()) cfg.endpoint = ps->default_endpoint;
    cfg.use_max_completion_tokens = ps->max_completion_tokens;

    const std::string ctx = " ist nicht gesetzt (benötigt für " + role + " = " + providerName + ")";
    if (cfg.api_key.empty()) problems.push_back(std::string(ps->key_var) + ctx);
    if (cfg.model.empty()) problems.push_back(std::string(ps->model_var) + ctx);
    return cfg;
}

std::string joinProblems(const std::vector<std::string>& problems) {
    std::string msg;
    for (const auto& p : problems) msg += "\n  - " + p;
    return msg;
}

} // namespace

LlmProvider::LlmProvider(std::string role, LlmConfig cfg)
    : role_(std::move(role)), cfg_(std::move(cfg)) {}

LlmConfig LlmProvider::legacyConfig() {
    LlmConfig cfg;
    cfg.provider = "llm";

    cfg.api_key = env("LLM_API_KEY");
    if (cfg.api_key.empty()) cfg.api_key = env("OPENAI_API_KEY");
    if (cfg.api_key.empty()) {
        throw std::runtime_error("LLM_API_KEY / OPENAI_API_KEY is not set.");
    }

    cfg.model = env("LLM_MODEL");
    if (cfg.model.empty()) cfg.model = env("OPENAI_MODEL");
    if (cfg.model.empty()) cfg.model = "gpt-5.2";

    std::string endpoint = stripTrailingSlash(env("LLM_ENDPOINT"));
    if (!endpoint.empty()) {
        // volle URL oder Basis-URL (z.B. .../v1beta/openai)
        if (!endsWith(endpoint, "/chat/completions")) endpoint += "/chat/completions";
    } else {
        // OPENAI_BASE_URL ist wie bisher ohne /v1 gemeint
        std::string base = stripTrailingSlash(env("OPENAI_BASE_URL"));
        if (base.empty()) base = "https://api.openai.com";
        endpoint = endsWith(base, "/v1") ? base + "/chat/completions" : base + "/v1/chat/completions";
    }
    cfg.endpoint = endpoint;
    cfg.use_max_completion_tokens = endpoint.find("api.openai.com") != std::string::npos;
    return cfg;
}

LlmConfig LlmProvider::roleConfig(const std::string& role) {
    std::vector<std::string> problems;
    LlmConfig cfg = resolveRole(role, problems);
    if (!problems.empty()) {
        throw std::runtime_error("Pipeline-Konfiguration unvollständig:" + joinProblems(problems));
    }
    return cfg;
}

bool LlmProvider::pipelineEnabled() {
    const std::string mode = toLower(env("BUGGAI_PIPELINE"));
    if (mode == "off" || mode == "0" || mode == "false") return false;

    for (char** e = environ; e && *e; ++e) {
        const std::string entry = *e;
        const size_t eq = entry.find('=');
        if (entry.rfind("BUGGAI_", 0) == 0 && eq != std::string::npos && eq + 1 < entry.size())
            return true;
    }
    return false;
}

void LlmProvider::validateEnvironment() {
    if (!pipelineEnabled()) return;

    std::vector<std::string> problems;
    std::vector<std::pair<std::string, LlmConfig>> roles;
    for (const auto& r : roleSpecs()) {
        roles.emplace_back(r.role, resolveRole(r.role, problems));
    }
    // debug_report läuft weiterhin über die Einzelmodell-Konfiguration
    if (env("LLM_API_KEY").empty() && env("OPENAI_API_KEY").empty()) {
        problems.push_back("LLM_API_KEY / OPENAI_API_KEY ist nicht gesetzt (benötigt für den Debug-Report)");
    }
    if (!problems.empty()) {
        throw std::runtime_error("Pipeline-Konfiguration unvollständig:" + joinProblems(problems));
    }

    std::cout << "[PIPELINE]";
    for (const auto& [role, cfg] : roles) {
        std::cout << " " << role << "=" << cfg.provider << "/" << cfg.model;
    }
    std::cout << "\n";
}

LlmResponse LlmProvider::chat(const std::string& system_prompt,
                              const std::string& user_prompt,
                              int max_tokens) const {
    static const bool curl_inited = (curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
    (void)curl_inited;

    LlmResponse out;
    Usage& usage = usageTable()[role_ + " " + label()];

    // Kein temperature/top_p: aktuelle Claude-Modelle antworten sonst mit HTTP 400
    json req;
    req["model"] = cfg_.model;
    req["messages"] = json::array({
        {{"role", "system"}, {"content", system_prompt}},
        {{"role", "user"},   {"content", user_prompt}}
    });
    req[cfg_.use_max_completion_tokens ? "max_completion_tokens" : "max_tokens"] = max_tokens;
    const std::string payload = req.dump();
    const std::string auth = "Authorization: Bearer " + cfg_.api_key;

    std::string body;
    for (int attempt = 1; attempt <= MAX_HTTP_ATTEMPTS; ++attempt) {
        CURL* curl = curl_easy_init();
        if (!curl) {
            out.error = "curl_easy_init failed";
            return out;
        }

        body.clear();
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, auth.c_str());
        headers = curl_slist_append(headers, "Content-Type: application/json");

        curl_easy_setopt(curl, CURLOPT_URL,           cfg_.endpoint.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    headers);
        curl_easy_setopt(curl, CURLOPT_POST,          1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    payload.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &body);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT,       HTTP_TIMEOUT_SEC);

        CURLcode code = curl_easy_perform(curl);
        long status = 0;
        curl_off_t retry_after = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_getinfo(curl, CURLINFO_RETRY_AFTER, &retry_after);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        usage.calls++;
        out.http_status = status;

        bool retryable = false;
        std::string reason;
        if (code != CURLE_OK) {
            retryable = true;
            reason = std::string("curl error: ") + curl_easy_strerror(code);
        } else if (status == 429 || status >= 500) {
            retryable = true;
            reason = "HTTP " + std::to_string(status) + ": " + redact(apiErrorMessage(body), cfg_.api_key);
        }

        if (!retryable) break;

        usage.failed++;
        out.error = reason;
        if (attempt == MAX_HTTP_ATTEMPTS) return out;

        long delay = 1L << (attempt - 1);  // 1s, 2s, 4s
        if (retry_after > 0) delay = std::min<long>(static_cast<long>(retry_after), 30);
        std::cerr << "  [" << role_ << "] " << reason << " – neuer Versuch in " << delay << "s ("
                  << (attempt + 1) << "/" << MAX_HTTP_ATTEMPTS << ")\n";
        std::this_thread::sleep_for(std::chrono::seconds(delay));
    }

    out.error.clear();
    out.raw_json = redact(body, cfg_.api_key);

    if (out.http_status < 200 || out.http_status >= 300) {
        usage.failed++;
        out.error = "HTTP " + std::to_string(out.http_status) + ": " + redact(apiErrorMessage(body), cfg_.api_key);
        return out;
    }

    json parsed = json::parse(body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        usage.failed++;
        out.error = "API-Antwort ist kein gültiges JSON: " + truncate(out.raw_json, 300);
        return out;
    }

    if (parsed.contains("usage") && parsed["usage"].is_object()) {
        const json& u = parsed["usage"];
        if (u.contains("prompt_tokens") && u["prompt_tokens"].is_number_integer())
            usage.prompt_tokens += u["prompt_tokens"].get<long>();
        if (u.contains("completion_tokens") && u["completion_tokens"].is_number_integer())
            usage.completion_tokens += u["completion_tokens"].get<long>();
    }

    std::string finish_reason;
    if (parsed.contains("choices") && parsed["choices"].is_array() && !parsed["choices"].empty()) {
        const json& choice = parsed["choices"][0];
        if (choice.contains("finish_reason") && choice["finish_reason"].is_string())
            finish_reason = choice["finish_reason"].get<std::string>();
        if (choice.contains("message") && choice["message"].is_object()) {
            const json& content = choice["message"].value("content", json());
            if (content.is_string()) {
                out.text = content.get<std::string>();
            } else if (content.is_array()) {
                for (const auto& part : content) {
                    if (part.is_object() && part.contains("text") && part["text"].is_string())
                        out.text += part["text"].get<std::string>();
                }
            }
        }
    }

    if (finish_reason == "length") {
        usage.failed++;
        out.error = "Antwort abgeschnitten – Token-Limit (max_tokens=" + std::to_string(max_tokens) + ")";
        return out;
    }
    if (out.text.find_first_not_of(" \t\r\n") == std::string::npos) {
        usage.failed++;
        out.error = "Leere Antwort (finish_reason=" + (finish_reason.empty() ? "?" : finish_reason) + ")";
        return out;
    }

    out.ok = true;
    return out;
}

std::string extract_json_object(const std::string& text) {
    const size_t start = text.find('{');
    const size_t end = text.rfind('}');
    if (start == std::string::npos || end == std::string::npos || end < start) return "";
    return text.substr(start, end - start + 1);
}
