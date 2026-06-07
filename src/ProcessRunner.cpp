#include "ProcessRunner.h"
#include <array>
#include <cstdio>
#include <stdexcept>
#include <sys/wait.h>
#include <string>
#include <iostream>

RunResult run_capture(const std::string& cmd) {
    RunResult r;
    std::string full = cmd + " 2>&1";

    FILE* pipe = popen(full.c_str(), "r");
    if (!pipe) {
        throw std::runtime_error("popen failed");
    }

    std::array<char, 4096> buf{};
    while (fgets(buf.data(), (int)buf.size(), pipe)) {
        std::string chunk = buf.data();

        r.output += chunk;

        std::cout << chunk << std::flush;
    }

    int status = pclose(pipe);
    if (status == -1) {
        r.exit_code = -1;
        return r;
    }

    if (WIFEXITED(status)) {

        r.exit_code = WEXITSTATUS(status);

        if (r.exit_code == 124) {

            r.status =
                ExecutionStatus::TIMEOUT;
        }

        else if (containsTestFailure(r.output)) {

            r.status =
                ExecutionStatus::TEST_FAILURE;
        }

        else if (r.exit_code != 0) {

            r.status =
                ExecutionStatus::RUNTIME_ERROR;
        }

        else {

            r.status =
                ExecutionStatus::SUCCESS;
        }
    }

    else if (WIFSIGNALED(status)) {

        int sig = WTERMSIG(status);

        r.exit_code = 128 + sig;

        r.status =
            ExecutionStatus::SIGNAL_TERMINATED;
    }

    else {

        r.exit_code = -1;

        r.status =
            ExecutionStatus::RUNTIME_ERROR;
    }

    return r;
}



bool containsTestFailure(const std::string& output) {

    return output.find("FAILED (errors=")
           != std::string::npos ||

           output.find("FAILED (failures=")
           != std::string::npos ||

           output.find("Ran ")
           != std::string::npos;
}