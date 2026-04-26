#include "ProcessRunner.h"
#include <array>
#include <cstdio>
#include <stdexcept>
#include <sys/wait.h>
#include <string>

RunResult run_capture(const std::string& cmd) {
    RunResult r;
    std::string full = cmd + " 2>&1";

    FILE* pipe = popen(full.c_str(), "r");
    if (!pipe) {
        throw std::runtime_error("popen failed");
    }

    std::array<char, 4096> buf{};
    while (fgets(buf.data(), (int)buf.size(), pipe)) {
        r.output += buf.data();
    }

    int status = pclose(pipe);
    if (status == -1) {
        r.exit_code = -1;
        return r;
    }

    if (WIFEXITED(status)) {
        r.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        r.exit_code = 128 + sig;
    } else {
        r.exit_code = -1;
    }

    return r;
}
