#pragma once
#include <string>

// Ein Anbieter mit OpenAI-kompatiblem Chat-Completions-Endpoint
// (OpenAI, Gemini, Claude oder ein beliebiger LLM_ENDPOINT).
struct LlmConfig {
    std::string provider;   // "gemini" | "claude" | "openai" | "llm"
    std::string endpoint;   // volle URL bis /chat/completions
    std::string api_key;
    std::string model;
    bool use_max_completion_tokens = false;  // OpenAI erwartet max_completion_tokens
};

struct LlmResponse {
    bool ok = false;          // HTTP 2xx, gültiges JSON, nicht abgeschnitten, nicht leer
    long http_status = 0;
    std::string text;         // choices[0].message.content
    std::string raw_json;     // komplette API-Antwort
    std::string error;        // lesbare Ursache, wenn !ok
};

class LlmProvider {
public:
    LlmProvider() = default;
    LlmProvider(std::string role, LlmConfig cfg);

    // Einzelmodell-Konfiguration: LLM_ENDPOINT / LLM_API_KEY / LLM_MODEL,
    // Fallback auf OPENAI_BASE_URL / OPENAI_API_KEY / OPENAI_MODEL.
    static LlmConfig legacyConfig();

    // Konfiguration einer Pipeline-Rolle ("Debugger", "Reviewer", "Verifier")
    // anhand von BUGGAI_DEBUGGER / BUGGAI_REVIEWER / BUGGAI_VERIFIER.
    static LlmConfig roleConfig(const std::string& role);

    // true, sobald eine BUGGAI_*-Variable gesetzt ist und BUGGAI_PIPELINE != off
    static bool pipelineEnabled();

    // Prüft beim Programmstart alle benötigten Variablen (nur im Pipeline-Modus)
    // und wirft mit den Namen der fehlenden Variablen.
    static void validateEnvironment();

    // Ein Chat-Completions-Aufruf mit Retry bei 429/5xx. Wirft nie.
    LlmResponse chat(const std::string& system_prompt,
                     const std::string& user_prompt,
                     int max_tokens) const;

    const std::string& role() const { return role_; }
    std::string label() const { return cfg_.provider + "/" + cfg_.model; }

private:
    std::string role_;
    LlmConfig cfg_;
};

// Schneidet das äußerste {...} aus einer Modellantwort (z.B. aus ```json Blöcken).
// Leerer String, wenn kein Objekt gefunden wird.
std::string extract_json_object(const std::string& text);
