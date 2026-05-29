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
        "You are a Senior Debugger. Analyze the JSON (debugger output, runtime errors, "
        "Valgrind output if available, detected errors). "
        "Structure your response in Markdown with the following sections:\n\n"
        "## Summary\n"
        "A brief summary of the main issue (max 2 sentences).\n\n"
        "## Severity\n"
        "critical, high, medium, or low\n\n"
        "## Bugs\n"
        "For each bug:\n\n"
        "### Bug N: [Title]\n"
        "- **File:** path/to/file.ext:line\n"
        "- **Category:** Choose the most specific category name "
        "(e.g., memory_leak, null_pointer, uninitialized_variable, "
        "off_by_one, integer_overflow, etc.). Use snake_case.\n"
        "- **Problem:** What is the problem (brief description)\n\n"
        "**Buggy code:**\n"
        "```\n"
        "// Original buggy code — use the language of the analyzed file "
        "(c, cpp, java, python)\n"
        "```\n\n"
        "**Fixed code:**\n"
        "```\n"
        "// Corrected code in the same language\n"
        "```\n\n"
        "**Explanation:** Brief explanation of why the fix works.\n\n"
        "## Recommendations (only if needed)\n"
        "OMIT THIS SECTION ENTIRELY unless there is at least one concrete, "
        "non-trivial systemic recommendation (tooling, build flags, testing "
        "strategy, language-specific best practices). "
        "Do NOT include generic advice like 'write tests' or 'use a linter'.\n\n"
        "Respond in English.";

    req["input"] =
        "Here is the report as JSON:\n\n" + report_json +
        "\n\nPlease analyze and explain the error, then propose a fix.";
        
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
        "You are a Senior Debugger. Analyze the error and the code. "
        "The source code may be in C, C++, Java, or Python — use the "
        "appropriate syntax and idioms for the language indicated by the "
        "'language' field and the source code itself.\n\n"
        "Respond ONLY with a JSON object, no text before or after:\n"
        "{\n"
        "  \"file_path\": \"...path to the fixed file...\",\n"
        "  \"fixed_code\": \"...only the content of this one file, in the original language...\",\n"
        "  \"new_errors\": [\"error1\", \"error2\"],\n"
        "  \"success\": true,\n"
        "  \"is_confident\": true\n"
        "}\n"
        "new_errors should only contain errors that are listed in the checklist. "
        "Respond in English.";

    std::string input_text =
        "Error name: " + req.error_name + "\n\n"
        "Language: " + req.language + "\n\n"
        "Error output:\n" + req.error_output + "\n\n"
        "Source code:\n" + req.source_code + "\n\n"
        "Checklist:\n" + req.checklist + "\n\n"
        "Please analyze and return ONLY the JSON object as specified.";

    fix_req["input"] = json::array({
        {
            {"type", "message"},
            {"role", "user"},
            {"content", input_text}
        }
    });
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
