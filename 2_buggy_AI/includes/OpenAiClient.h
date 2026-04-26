#pragma once
#include <string>

struct OpenAIResult {
    long http_status = 0;
    std::string raw_json;   // komplette API-Antwort (oder Fehler)
    std::string text;       // extrahierter Text (wenn vorhanden)
};

class OpenAIClient {
public:
    OpenAIClient();
    OpenAIResult debug_report(const std::string& report_json) const;

private:
    std::string api_key_;
    std::string base_url_;
    std::string model_;
};
