#pragma once

#include <string>
#include <vector>

namespace vex {

struct ProcessResult {
    bool started = false;
    int exit_code = -1;
    std::string error;
};

// Runs a process without passing arguments through a shell.
[[nodiscard]] ProcessResult run_process(
    const std::string& executable,
    const std::vector<std::string>& arguments);

} // namespace vex
