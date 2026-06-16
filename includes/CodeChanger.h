#pragma once
#include <string>
#include "OpenAiClient.h"

class CodeChanger{
    public:
        CodeChanger() = default;
        explicit CodeChanger(std::string basePath) : basePath_(std::move(basePath)) {}
        bool apply_fix(FixResult fix_result);

    private:
        std::string basePath_;
};