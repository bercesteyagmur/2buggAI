#pragma once
#include <string>

struct RunResult {
    int exit_code = -1;
    std::string output;
};

RunResult run_capture(const std::string& cmd);
