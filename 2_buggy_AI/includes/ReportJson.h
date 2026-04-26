#pragma once
#include "ProcessRunner.h"
#include "ValgrindRunner.h"
#include <string>
#include <vector>

std::string make_report_json(
    const std::string& targetPath,
    const std::string& fixDescription,
    bool recursive,
    bool verbose,
    const std::vector<std::string>& exts,
    const std::vector<std::string>& passthrough,
    const RunResult* gdb,
    const ValgrindResult* vg,
    const RunResult* plain_run,
    const std::string& sourceCode = ""
);
