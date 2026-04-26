#include "GdbRunner.h"
#include "ShellQuote.h"

RunResult run_gdb(const std::string& program, const std::vector<std::string>& args) {
    std::string cmd =
        "gdb -q --batch "
        "-ex \"set pagination off\" "
        "-ex \"set confirm off\" "
        "-ex \"set print pretty on\" "
        "-ex \"set print frame-arguments all\" "
        "-ex \"set print elements 200\" "
        "-ex \"echo \\n===BUGGY_GDB:RUN===\\n\" "
        "-ex \"run\" "
        "-ex \"echo \\n===BUGGY_GDB:INFO_PROGRAM===\\n\" "
        "-ex \"info program\" "
        "-ex \"echo \\n===BUGGY_GDB:THREADS===\\n\" "
        "-ex \"info threads\" "
        "-ex \"echo \\n===BUGGY_GDB:BT_ALL===\\n\" "
        "-ex \"thread apply all bt full\" "
        "-ex \"echo \\n===BUGGY_GDB:FRAME0===\\n\" "
        "-ex \"frame 0\" "
        "-ex \"info args\" "
        "-ex \"info locals\" "
        "-ex \"echo \\n===BUGGY_GDB:REGS===\\n\" "
        "-ex \"info registers\" "
        "--args " + ShellQuote::quote(program);

    for (const auto& a : args) cmd += " " + ShellQuote::quote(a);
    return run_capture(cmd);
}
