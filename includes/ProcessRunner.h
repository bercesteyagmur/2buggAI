#pragma once
#include <string>

enum class ExecutionStatus {
    SUCCESS,
    TEST_FAILURE,
    TIMEOUT,
    SIGNAL_TERMINATED,
    RUNTIME_ERROR
};

struct RunResult {
    int exit_code = -1;
    std::string output;
    ExecutionStatus status =ExecutionStatus::RUNTIME_ERROR;
};

RunResult run_capture(const std::string& cmd);

bool containsTestFailure(const std::string& output);
