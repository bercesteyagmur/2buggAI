#pragma once
#include "ProcessRunner.h"
#include <string>
#include <vector>

RunResult run_gdb(const std::string& program, const std::vector<std::string>& args);
