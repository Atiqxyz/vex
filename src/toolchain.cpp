#include "vex/toolchain.hpp"

#include "vex/ast.hpp"
#include "vex/backend.hpp"
#include "vex/diagnostic.hpp"
#include "vex/ir.hpp"
#include "vex/lexer.hpp"
#include "vex/lowering.hpp"
#include "vex/parser.hpp"
#include "vex/process.hpp"
#include "vex/semantic.hpp"
#include "vex/source.hpp"
#include "vex/detail/toolchain_discovery.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <random>
#include <string>
#include <system_error>
#include <vector>
#include <filesystem>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <cstdint>
#endif

#ifndef VEX_DEFAULT_C_COMPILER
#define VEX_DEFAULT_C_COMPILER ""
#endif
#ifndef VEX_DEFAULT_C_COMPILER_ID
#define VEX_DEFAULT_C_COMPILER_ID ""
#endif

namespace vex {
namespace fs = std::filesystem;

namespace detail {

fs::path bundled_llvm_compiler_path(const fs::path& executable_path) {
    if (executable_path.empty()) return {};

    const fs::path installation_root =
        executable_path.parent_path().parent_path();
#ifdef _WIN32
    return installation_root / "toolchain" / "bin" / "clang.exe";
#else
    return installation_root / "toolchain" / "bin" / "clang";
#endif
}

fs::path bundled_zig_compiler_path(const fs::path& executable_path) {
    if (executable_path.empty()) return {};

    const fs::path installation_root =
        executable_path.parent_path().parent_path();
#ifdef _WIN32
    return installation_root / "toolchain" / "zig.exe";
#else
    return installation_root / "toolchain" / "zig";
#endif
}

} // namespace detail

namespace {

fs::path current_executable_path() {
#ifdef _WIN32
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const DWORD length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) return {};
        if (length < buffer.size() - 1) {
            return fs::path(std::wstring(buffer.data(), length));
        }
        if (buffer.size() >= 32768) return {};
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    uint32_t size = 1024;
    std::vector<char> buffer(size);
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        buffer.resize(size);
        if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
    }
    return fs::path(buffer.data());
#else
    std::error_code error;
    const fs::path path = fs::read_symlink("/proc/self/exe", error);
    return error ? fs::path{} : path;
#endif
}

struct TemporaryDirectory {
    fs::path path;
    ~TemporaryDirectory() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

bool make_temporary_directory(TemporaryDirectory& temporary) {
    std::error_code error;
    const fs::path root = fs::temp_directory_path(error);
    if (error) {
        std::cerr << "error: cannot locate the temporary directory: "
                  << error.message() << '\n';
        return false;
    }

    std::random_device random;
    for (int attempt = 0; attempt < 32; ++attempt) {
        const fs::path candidate = root /
            ("vex-" + std::to_string(random()) + "-" + std::to_string(attempt));
        if (fs::create_directory(candidate, error)) {
            temporary.path = candidate;
            return true;
        }
        if (error && error != std::errc::file_exists) {
            std::cerr << "error: cannot create temporary build directory: "
                      << error.message() << '\n';
            return false;
        }
        error.clear();
    }
    std::cerr << "error: could not allocate a unique temporary build directory\n";
    return false;
}

bool generate_with_backend(const std::string& path, const BackendKind backend_kind,
                           std::string& generated_source) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::cerr << "error: could not open source file: " << path << '\n';
        return false;
    }
    std::string contents{
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (input.bad()) {
        std::cerr << "error: failed while reading source file: " << path << '\n';
        return false;
    }

    const SourceFile source(path, std::move(contents));
    const LexResult lexed = Lexer(source).scan();
    if (lexed.has_errors()) {
        print_diagnostics(std::cerr, source, lexed.diagnostics);
        return false;
    }

    const ParseResult parsed = Parser(source, lexed.tokens).parse();
    if (parsed.has_errors()) {
        print_diagnostics(std::cerr, source, parsed.diagnostics);
        return false;
    }

    const SemanticResult checked =
        SemanticAnalyzer(source, parsed.program).analyze();
    print_diagnostics(std::cerr, source, checked.diagnostics);
    if (checked.has_errors()) return false;

    const IRLoweringResult lowered =
        IRLowerer(source, parsed.program, checked.model).lower();
    print_diagnostics(std::cerr, source, lowered.diagnostics);
    if (lowered.has_errors()) return false;

    const IRVerificationResult verified = IRVerifier{}.verify(lowered.module);
    print_diagnostics(std::cerr, source, verified.diagnostics);
    if (verified.has_errors()) return false;

    const std::unique_ptr<Backend> backend = create_backend(backend_kind);
    if (!backend) {
        std::cerr << "error: the requested VEX backend is not available\n";
        return false;
    }

    const BackendResult generated = backend->emit(lowered.module);
    print_diagnostics(std::cerr, source, generated.diagnostics);
    if (generated.has_errors()) return false;

    generated_source = generated.output;
    return true;
}

std::vector<std::string> compiler_candidates() {
    std::vector<std::string> candidates;
    const auto add = [&candidates](const char* value) {
        if (value == nullptr || *value == '\0') return;
        for (const std::string& existing : candidates) {
            if (existing == value) return;
        }
        candidates.emplace_back(value);
    };
    add(std::getenv("VEX_CC"));
    add(std::getenv("CC"));
    add(VEX_DEFAULT_C_COMPILER);
#ifdef _WIN32
    add("cl.exe");
    add("clang.exe");
    add("gcc.exe");
#else
    add("cc");
    add("clang");
    add("gcc");
#endif
    return candidates;
}

struct LLVMCompilerCandidate {
    std::string executable;
    bool is_zig{false};
};

std::vector<LLVMCompilerCandidate> llvm_compiler_candidates() {
    std::vector<LLVMCompilerCandidate> candidates;
    const auto add = [&candidates](
        const fs::path& executable, const bool is_zig) {
        if (executable.empty()) return;
        const std::string path = executable.string();
        if (path.empty()) return;
        for (const LLVMCompilerCandidate& existing : candidates) {
            if (existing.executable == path && existing.is_zig == is_zig) {
                return;
            }
        }
        candidates.push_back({path, is_zig});
    };
    const auto add_clang = [&add](const char* value) {
        if (value != nullptr && *value != '\0') add(fs::path(value), false);
    };
    const auto add_zig = [&add](const fs::path& value) {
        if (!value.empty()) add(value, true);
    };

    add_clang(std::getenv("VEX_LLVM_CC"));
    if (const char* zig = std::getenv("VEX_LLVM_ZIG");
        zig != nullptr && *zig != '\0') {
        add_zig(fs::path(zig));
    }
    add_clang(std::getenv("LLVM_CC"));

    if (const char* home = std::getenv("VEX_HOME");
        home != nullptr && *home != '\0') {
#ifdef _WIN32
        add_zig(fs::path(home) / "toolchain" / "zig.exe");
        add_clang((fs::path(home) / "toolchain" / "bin" / "clang.exe").string().c_str());
#else
        add_zig(fs::path(home) / "toolchain" / "zig");
        add_clang((fs::path(home) / "toolchain" / "bin" / "clang").string().c_str());
#endif
    }

    const fs::path executable = current_executable_path();
    if (!executable.empty()) {
        add_zig(detail::bundled_zig_compiler_path(executable));
        add_clang(detail::bundled_llvm_compiler_path(executable).string().c_str());
    }

#ifdef _WIN32
    add_clang("clang.exe");
    add_zig("zig.exe");
#else
    add_clang("clang");
    add_zig("zig");
#endif
    return candidates;
}


bool write_text_file(const fs::path& path, const std::string& contents) {
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        std::cerr << "error: cannot write temporary file: " << path.string() << '\n';
        return false;
    }
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!output) {
        std::cerr << "error: failed writing temporary file: " << path.string() << '\n';
        return false;
    }
    return true;
}

ProcessResult compile_with_msvc(
    const std::string& compiler,
    const fs::path& generated_c,
    const fs::path& output,
    const fs::path& temporary_directory) {
    fs::path installation_root = fs::path(compiler).parent_path();
    for (int level = 0; level < 7; ++level) {
        installation_root = installation_root.parent_path();
    }
    const fs::path developer_command =
        installation_root / "Common7" / "Tools" / "VsDevCmd.bat";
    if (!fs::exists(developer_command)) {
        return {false, -1,
            "could not locate VsDevCmd.bat for the configured MSVC compiler"};
    }

    const fs::path script = temporary_directory / "vex-compile.bat";
    std::string contents =
        "@echo off\r\n"
        "call \"" + developer_command.string() +
        "\" -arch=x64 -host_arch=x64 >nul\r\n"
        "if errorlevel 1 exit /b 1\r\n"
        "\"" + compiler + "\" /nologo /std:c11 /TC \"" +
        generated_c.string() + "\" \"/Fe:" + output.string() + "\" >nul\r\n"
        "exit /b %errorlevel%\r\n";
    if (!write_text_file(script, contents)) {
        return {false, -1, "could not create the MSVC compiler script"};
    }
    return run_process("cmd.exe", {"/D", "/C", script.string()});
}

int build_to_output(
    const std::string& source_path,
    const fs::path& output_path,
    const fs::path& temporary_directory,
    const bool announce = true) {
    std::string generated_source;
    if (!generate_with_backend(source_path, BackendKind::C, generated_source)) return 1;

    const fs::path generated_c = temporary_directory / "program.c";
    if (!write_text_file(generated_c, generated_source)) return 1;

    std::error_code error;
    const fs::path output_absolute = fs::absolute(output_path, error);
    if (error) {
        std::cerr << "error: cannot resolve output path: " << error.message() << '\n';
        return 1;
    }
    if (!fs::exists(output_absolute.parent_path())) {
        std::cerr << "error: output directory does not exist: "
                  << output_absolute.parent_path().string() << '\n';
        return 1;
    }

    const std::vector<std::string> compilers = compiler_candidates();
    std::string last_error = "no usable C11 compiler was found";
    for (const std::string& compiler : compilers) {
        ProcessResult result;
#ifdef _WIN32
        const bool configured_msvc =
            compiler == VEX_DEFAULT_C_COMPILER &&
            std::string(VEX_DEFAULT_C_COMPILER_ID) == "MSVC";
        const bool named_msvc =
            fs::path(compiler).filename().string() == "cl.exe";
        if (configured_msvc) {
            result = compile_with_msvc(
                compiler, generated_c, output_absolute, temporary_directory);
        } else if (named_msvc) {
            result = run_process(compiler, {
                "/nologo", "/std:c11", "/TC", generated_c.string(),
                "/Fe:" + output_absolute.string()
            });
        } else {
            result = run_process(compiler, {
                "-std=c11", generated_c.string(), "-o", output_absolute.string(), "-lm"
            });
        }
#else
        result = run_process(compiler, {
            "-std=c11", generated_c.string(), "-o", output_absolute.string(), "-lm"
        });
#endif

        if (!result.started) {
            last_error = result.error;
            continue;
        }
        if (result.exit_code != 0) {
            std::cerr << "error: C compiler exited with code "
                      << result.exit_code << '\n';
            return result.exit_code == 0 ? 1 : result.exit_code;
        }
        if (announce) {
            std::cout << "Built " << output_absolute.string() << '\n';
        }
        return 0;
    }

    std::cerr << "error: " << last_error
              << ". Install a C11 compiler or set VEX_CC to its executable path.\n";
    return 1;
}

fs::path default_output_path(const std::string& source_path) {
    fs::path output = fs::absolute(fs::path(source_path));
    output.replace_extension();
#ifdef _WIN32
    output += ".exe";
#endif
    return output;
}


int build_llvm_to_output(
    const std::string& source_path,
    const fs::path& output_path,
    const fs::path& temporary_directory,
    const bool announce = true) {
    std::string generated_ir;
    if (!generate_with_backend(source_path, BackendKind::LLVM, generated_ir)) return 1;

    const fs::path generated_llvm = temporary_directory / "program.ll";
    if (!write_text_file(generated_llvm, generated_ir)) return 1;

    std::error_code error;
    const fs::path output_absolute = fs::absolute(output_path, error);
    if (error) {
        std::cerr << "error: cannot resolve output path: " << error.message() << '\n';
        return 1;
    }
    if (!fs::exists(output_absolute.parent_path())) {
        std::cerr << "error: output directory does not exist: "
                  << output_absolute.parent_path().string() << '\n';
        return 1;
    }

    std::string last_error = "no bundled Zig or Clang executable was found";
    for (const LLVMCompilerCandidate& compiler : llvm_compiler_candidates()) {
        ProcessResult result;
        if (compiler.is_zig) {
            // Compile textual LLVM IR to a native object through Zig's embedded
            // Clang, then link it with Zig's bundled linker and target runtime.
#ifdef _WIN32
            const fs::path object_file = temporary_directory / "program.obj";
#else
            const fs::path object_file = temporary_directory / "program.o";
#endif
            std::vector<std::string> compile_arguments{
                "clang", "-O2"
            };
#ifdef __linux__
            // Produce Linux programs against bundled musl rather than the
            // build host's glibc version.
            compile_arguments.insert(compile_arguments.end(), {
                "-target", "x86_64-linux-musl"
            });
#endif
            compile_arguments.insert(compile_arguments.end(), {
                "-c", generated_llvm.string(), "-o", object_file.string()
            });
            const ProcessResult compile_result =
                run_process(compiler.executable, compile_arguments);
            if (!compile_result.started) {
                last_error = compile_result.error;
                continue;
            }
            if (compile_result.exit_code != 0) {
                std::cerr << "error: bundled Zig Clang exited with code "
                          << compile_result.exit_code << '\n';
                return compile_result.exit_code;
            }

            std::vector<std::string> link_arguments{
                "build-exe", object_file.string(), "-O", "ReleaseFast"
            };
#ifdef __linux__
            link_arguments.insert(link_arguments.end(), {
                "-target", "x86_64-linux-musl"
            });
#endif
            link_arguments.emplace_back("-lc");
#ifndef _WIN32
            link_arguments.emplace_back("-lm");
#endif
            link_arguments.emplace_back("-femit-bin=" + output_absolute.string());
            result = run_process(compiler.executable, link_arguments);
        } else {
            std::vector<std::string> arguments{
                "-O2", generated_llvm.string(), "-o", output_absolute.string()
            };
#ifndef _WIN32
            arguments.emplace_back("-lm");
#endif
            result = run_process(compiler.executable, arguments);
        }
        if (!result.started) {
            last_error = result.error;
            continue;
        }
        if (result.exit_code != 0) {
            std::cerr << "error: LLVM toolchain exited with code "
                      << result.exit_code << '\n';
            return result.exit_code;
        }
        if (announce) {
            std::cout << "Built " << output_absolute.string() << '\n';
        }
        return 0;
    }

    std::cerr << "error: " << last_error
              << ". Install VEX with its bundled toolchain, place Zig in the VEX "
              << "installation's toolchain directory, install Clang, or set "
              << "VEX_LLVM_ZIG or VEX_LLVM_CC to an executable path.\n";
    return 1;
}


} // namespace

int build_program(
    const std::string& source_path,
    const std::string& output_path) {
    TemporaryDirectory temporary;
    if (!make_temporary_directory(temporary)) return 1;
    const fs::path output = output_path.empty()
        ? default_output_path(source_path)
        : fs::path(output_path);
    return build_to_output(source_path, output, temporary.path);
}

int run_program(const std::string& source_path) {
    TemporaryDirectory temporary;
    if (!make_temporary_directory(temporary)) return 1;
    fs::path executable = temporary.path / "vex-program";
#ifdef _WIN32
    executable += ".exe";
#endif
    const int build_status =
        build_to_output(source_path, executable, temporary.path, false);
    if (build_status != 0) return build_status;

    const ProcessResult result = run_process(executable.string(), {});
    if (!result.started) {
        std::cerr << "error: could not start compiled VEX program: "
                  << result.error << '\n';
        return 1;
    }
    return result.exit_code;
}


int build_llvm_program(
    const std::string& source_path,
    const std::string& output_path) {
    TemporaryDirectory temporary;
    if (!make_temporary_directory(temporary)) return 1;
    const fs::path output = output_path.empty()
        ? default_output_path(source_path)
        : fs::path(output_path);
    return build_llvm_to_output(source_path, output, temporary.path);
}

int run_llvm_program(const std::string& source_path) {
    TemporaryDirectory temporary;
    if (!make_temporary_directory(temporary)) return 1;
    fs::path executable = temporary.path / "vex-llvm-program";
#ifdef _WIN32
    executable += ".exe";
#endif
    const int build_status =
        build_llvm_to_output(source_path, executable, temporary.path, false);
    if (build_status != 0) return build_status;

    const ProcessResult result = run_process(executable.string(), {});
    if (!result.started) {
        std::cerr << "error: could not start compiled VEX LLVM program: "
                  << result.error << '\n';
        return 1;
    }
    return result.exit_code;
}

} // namespace vex
