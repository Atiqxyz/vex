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
#ifdef _WIN32
namespace {

// _spawnvp combines argv elements into a Windows command line. Quote each
// argument using the Microsoft CRT parsing rules so paths containing spaces
// (including the Clang installation path) remain one argument.
std::string quote_windows_argument(const std::string& argument) {
    const bool needs_quotes = argument.empty() ||
        argument.find_first_of(" \t\n\v\f\r\"") != std::string::npos;
    if (!needs_quotes) return argument;

    std::string quoted;
    quoted.reserve(argument.size() + 2);
    quoted.push_back('"');
    std::size_t backslashes = 0;
    for (const char character : argument) {
        if (character == '\\') {
            ++backslashes;
            continue;
        }
        if (character == '"') {
            quoted.append(backslashes * 2 + 1, '\\');
            quoted.push_back('"');
            backslashes = 0;
            continue;
        }
        quoted.append(backslashes, '\\');
        backslashes = 0;
        quoted.push_back(character);
    }
    quoted.append(backslashes * 2, '\\');
    quoted.push_back('"');
    return quoted;
}

} // namespace
#endif

ProcessResult run_process(
    const std::string& executable,
    const std::vector<std::string>& arguments) {
    ProcessResult result;
#ifdef _WIN32
    std::vector<std::string> quoted_arguments;
    quoted_arguments.reserve(arguments.size() + 1);
    quoted_arguments.push_back(quote_windows_argument(executable));
    for (const std::string& argument : arguments) {
        quoted_arguments.push_back(quote_windows_argument(argument));
    }

    std::vector<const char*> argv;
    argv.reserve(quoted_arguments.size() + 1);
    for (const std::string& argument : quoted_arguments) {
        argv.push_back(argument.c_str());
    }
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
