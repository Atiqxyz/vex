#pragma once

#include <string>
#include <vector>

namespace vex {

[[nodiscard]] int build_program(const std::vector<std::string>& source_paths,
                                 const std::string& output_path = {});
[[nodiscard]] int run_program(const std::vector<std::string>& source_paths);
[[nodiscard]] int build_llvm_program(const std::vector<std::string>& source_paths,
                                     const std::string& output_path = {});
[[nodiscard]] int run_llvm_program(const std::vector<std::string>& source_paths);


// Compiles a VEX source file to a native executable using an installed C11
// compiler. An empty output path selects a name based on the source file.
[[nodiscard]] int build_program(
    const std::string& source_path,
    const std::string& output_path = {});

// Builds a VEX source file in a temporary directory, executes it, and removes
// the temporary build artifacts.
[[nodiscard]] int run_program(const std::string& source_path);

// Compiles VEX IR through the LLVM backend and an installed Clang driver.
[[nodiscard]] int build_llvm_program(
    const std::string& source_path,
    const std::string& output_path = {});

// Builds and executes a VEX source file through the LLVM backend.
[[nodiscard]] int run_llvm_program(const std::string& source_path);

} // namespace vex
