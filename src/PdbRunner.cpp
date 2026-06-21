#include "PdbRunner.h"
#include "ShellQuote.h"
#include "ProcessRunner.h"

RunResult run_pdb(const std::string& script) {
    std::string cmd = "yes \"\" | timeout 10s python3 -m pdb -c continue " + ShellQuote::quote(script);
    return run_capture(cmd);
}
