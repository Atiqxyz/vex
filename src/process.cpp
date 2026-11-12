#include "vex/process.hpp"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace vex {

ProcessResult run_process(
    const std::string& executable,
    const std::vector<std::string>& arguments) {
    ProcessResult result;
#ifdef _WIN32
    std::vector<const char*> argv;
    argv.reserve(arguments.size() + 2);
    argv.push_back(executable.c_str());
    for (const std::string& argument : arguments) argv.push_back(argument.c_str());
    argv.push_back(nullptr);
    const std::intptr_t status = _spawnvp(
        _P_WAIT, executable.c_str(), argv.data());
    if (status == -1) {
        result.error = "could not start '" + executable + "'";
        return result;
    }
    result.started = true;
    result.exit_code = static_cast<int>(status);
#else
    pid_t process = 0;
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 2);
    argv.push_back(const_cast<char*>(executable.c_str()));
    for (const std::string& argument : arguments) {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    const int spawn_status = posix_spawnp(
        &process, executable.c_str(), nullptr, nullptr, argv.data(), ::environ);
    if (spawn_status != 0) {
        result.error = "could not start '" + executable + "': ";
        result.error += std::strerror(spawn_status);
        return result;
    }

    result.started = true;
    int status = 0;
    while (waitpid(process, &status, 0) == -1) {
        if (errno == EINTR) continue;
        result.error = "failed while waiting for '" + executable + "'";
        result.started = false;
        return result;
    }
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
    } else {
        result.exit_code = 1;
    }
#endif
    return result;
}

} // namespace vex
