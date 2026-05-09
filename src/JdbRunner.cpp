#include "JdbRunner.h"
#include "ShellQuote.h"

RunResult run_jdb(const std::string& command) {
    std::string cmd = "timeout 10s " + command;
    return run_capture(cmd);
}