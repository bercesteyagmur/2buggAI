#pragma once
#include <string>
#include "OpenAiClient.h"

class CodeChanger{
    public:
        CodeChanger() = default;
        explicit CodeChanger(std::string basePath) : basePath_(std::move(basePath)) {}
        bool apply_fix(FixResult fix_result);

        // Content of the file before the last apply_fix() (for the report).
        const std::string& previousContent() const { return previous_; }

    private:
        std::string basePath_;
        std::string previous_;
};