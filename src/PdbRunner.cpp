#include "PdbRunner.h"
#include "ShellQuote.h"
#include "ProcessRunner.h"

RunResult run_pdb(const std::string& script) {
    std::string cmd = "timeout 10s python3 -m pdb " + ShellQuote::quote(script);
    return run_capture(cmd);
}
