#include "OpenAiClient.h"
#include <cstdlib>
#include <stdexcept>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

using nlohmann::json;

static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

static std::string getenv_or(const char* key, const char* def) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : std::string(def);
}

static std::string extract_text_best_effort(const json& resp) {
    // falls die API ein output_text Feld liefert
    if (resp.contains("output_text") && resp["output_text"].is_string()) {
        return resp["output_text"].get<std::string>();
    }

    // durchs "output" Array laufen und Text einsammeln
    std::string out;
    if (resp.contains("output") && resp["output"].is_array()) {
        for (const auto& item : resp["output"]) {
            if (!item.is_object()) continue;
            if (!item.contains("content") || !item["content"].is_array()) continue;

            for (const auto& part : item["content"]) {
                if (!part.is_object()) continue;

                // häufig: { "type": "...", "text": "..." }
                if (part.contains("text") && part["text"].is_string()) {
                    if (!out.empty()) out += "\n";
                    out += part["text"].get<std::string>();
                }
            }
        }
    }
    return out;
}

OpenAIClient::OpenAIClient() {
    static bool curl_inited = false;
    if (!curl_inited) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        curl_inited = true;
    }

    api_key_  = getenv_or("OPENAI_API_KEY", "");
    base_url_ = getenv_or("OPENAI_BASE_URL", "https://api.openai.com");
    model_    = getenv_or("OPENAI_MODEL", "gpt-5.2");

    if (api_key_.empty()) {
        throw std::runtime_error("OPENAI_API_KEY is not set.");
    }
}

OpenAIResult OpenAIClient::debug_report(const std::string& report_json) const {
    // Request für POST /v1/responses
    json req;
    req["model"] = model_;
    req["instructions"] =
        "Du bist ein Senior Debugger. Analysiere das JSON (GDB/Valgrind/Detected Errors). "
        "Strukturiere deine Antwort in Markdown mit folgenden Abschnitten:\n\n"
        "## Zusammenfassung\n"
        "Eine kurze Zusammenfassung des Hauptproblems (max 2 Sätze).\n\n"
        "## Severity\n"
        "critical, high, medium, oder low\n\n"
        "## Bugs\n"
        "Für jeden Bug:\n\n"
        "### Bug N: [Titel]\n"
        "- **Datei:** Pfad/zur/Datei.cpp:Zeile\n"
        "- **Kategorie:** memory_leak / null_pointer / race_condition / buffer_overflow / resource_leak / other\n"
        "- **Problem:** Was ist das Problem (kurze Beschreibung)\n\n"
        "**Fehlerhafter Code:**\n"
        "```cpp\n"
        "// Original buggy code\n"
        "```\n\n"
        "**Gefixter Code:**\n"
        "```cpp\n"
        "// Corrected code\n"
        "```\n\n"
        "**Erklärung:** Kurze Erklärung warum der Fix funktioniert.\n\n"
        "## Empfehlungen\n"
        "Allgemeine Empfehlungen als nummerierte Liste.\n\n"
        "Antwort auf Deutsch.";
    req["input"] =
        "Hier ist der Report als JSON:\n\n" + report_json +
        "\n\nBitte analysieren und den Fehler erklären + Fix vorschlagen.";
    req["max_output_tokens"] = 4000;

    const std::string url = base_url_ + "/v1/responses";
    const std::string payload = req.dump();

    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("curl_easy_init failed");

    std::string resp_body;
    struct curl_slist* headers = nullptr;

    const std::string auth = "Authorization: Bearer " + api_key_;
    headers = curl_slist_append(headers, auth.c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp_body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);

    CURLcode code = curl_easy_perform(curl);

    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (code != CURLE_OK) {
        throw std::runtime_error(std::string("curl error: ") + curl_easy_strerror(code));
    }

    OpenAIResult out;
    out.http_status = http_status;
    out.raw_json = resp_body;

    json parsed = json::parse(resp_body, nullptr, false);
    if (!parsed.is_discarded()) {
        out.text = extract_text_best_effort(parsed);
    }
    return out;
}

FixResult OpenAIClient::fix_code(const FixRequest& req) const {
    FixResult result;
    
    // 1. Prompt bauen
    json fix_req;
    fix_req["model"] = model_;
    fix_req["instructions"] =
        "Du bist ein Senior Debugger. Analysiere den Fehler und den Code. "
        "Antworte NUR mit einem JSON Objekt, kein Text davor oder danach:\n"
        "{\n"
        "  \"file_path\": \"...Pfad zur gefixten Datei...\",\n"
        "  \"fixed_code\": \"...nur der Inhalt dieser einen Datei...\",\n"
        "  \"new_errors\": [\"fehler1\", \"fehler2\"],\n"
        "  \"success\": true,\n"
        "  \"is_confident\": true\n"
        "}\n"
        "new_errors soll nur Fehler enthalten die in der Checkliste stehen. "
        "Antworte auf Deutsch.";

    fix_req["input"] = {
        {"error_name",   req.error_name},
        {"error_output", req.error_output},
        {"language",     req.language},
        {"source_code",  req.source_code},
        {"checklist",    req.checklist}
    };
    fix_req["max_output_tokens"] = 4000;
    
    // 2. HTTP Request schicken (gleiche curl Logik wie debug_report)

    const std::string url = base_url_ + "/v1/responses";
    const std::string payload = fix_req.dump();

    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("curl_easy_init failed");

    std::string resp_body;
    struct curl_slist* headers = nullptr;

    const std::string auth = "Authorization: Bearer " + api_key_;
    headers = curl_slist_append(headers, auth.c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL,           url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    headers);
    curl_easy_setopt(curl, CURLOPT_POST,          1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    payload.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &resp_body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       60L);

    CURLcode code = curl_easy_perform(curl);
    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (code != CURLE_OK)
        throw std::runtime_error(std::string("curl error: ") + curl_easy_strerror(code));

    
    // 3. Antwort parsen → FixResult befüllen

    json parsed = json::parse(resp_body, nullptr, false);
    if (parsed.is_discarded()) return result; // leeres FixResult bei ungültigem JSON
    
    // Text aus API Antwort extrahieren (gleich wie debug_report)
    std::string text = extract_text_best_effort(parsed);

    // Text nochmal als JSON parsen
    json fix_result = json::parse(text, nullptr, false);
    if (fix_result.is_discarded()) return result;

    if (fix_result.contains("fixed_code"))
        result.fixed_code = fix_result["fixed_code"].get<std::string>();

    if (fix_result.contains("new_errors") && fix_result["new_errors"].is_array())
        for (const auto& e : fix_result["new_errors"])
            result.new_errors.push_back(e.get<std::string>());

    if (fix_result.contains("success"))
        result.success = fix_result["success"].get<bool>();

    if (fix_result.contains("is_confident"))
        result.is_confident = fix_result["is_confident"].get<bool>();

    if (fix_result.contains("file_path"))
        result.file_path = fix_result["file_path"].get<std::string>();

    return result;
}
