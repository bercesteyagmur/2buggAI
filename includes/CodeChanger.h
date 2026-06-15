#pragma once
#include <string>
#include "OpenAiClient.h"

class CodeChanger{
    public:
        bool apply_fix(FixResult fix_result, const std::string& projectPath = "");
};