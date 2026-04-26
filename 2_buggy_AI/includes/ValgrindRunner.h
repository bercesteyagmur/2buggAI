#pragma once
#include "ProcessRunner.h"
#include <string>
#include <vector>

struct ValgrindResult {
    RunResult run;
    std::string xml;
};

ValgrindResult run_valgrind(const std::string& program, const std::vector<std::string>& args);
