#include "vex/ast.hpp"
#include "vex/backend.hpp"
#include "vex/diagnostic.hpp"
#include "vex/lexer.hpp"
#include "vex/ir.hpp"
#include "vex/lowering.hpp"
#include "vex/module.hpp"
#include "vex/project.hpp"
#include "vex/parser.hpp"
#include "vex/semantic.hpp"
#include "vex/source.hpp"
#include "vex/token.hpp"
#include "vex/toolchain.hpp"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
#ifndef VEX_VERSION
#define VEX_VERSION "0.1.0"
#endif
namespace {
void print_help() {
    std::cout
        << "VEX compiler " << VEX_VERSION << "\n"
        << "Usage:\n"
        << "  vex init [name]        Create a project with vex.toml and src/main.vex\n"
        << "  vex lex <file.vex>     Tokenize a VEX source file\n"
        << "  vex parse <file.vex>   Parse a VEX source file and print its AST\n"
        << "  vex check [files...]   Check source files or the discovered project\n"
        << "  vex ir [files...]      Lower a valid program to typed SSA IR\n"
        << "  vex emit-c [files...]  Emit portable C11 from verified VEX IR\n"
        << "  vex emit-llvm [files...] Emit LLVM IR from verified VEX IR\n"
        << "  vex build [files...]   Build using the project backend or default LLVM\n"
        << "  vex run [files...]     Build and execute using the project backend or LLVM\n"
        << "  vex build-c [files...] Build using the optional C11 backend\n"
        << "  vex run-c [files...]   Build and execute using the optional C11 backend\n"
        << "  vex build-llvm [files...] Build using LLVM\n"
        << "  vex run-llvm [files...] Build and execute using LLVM\n"
        << "  vex --version          Show compiler version\n"
        << "  vex --help             Show this help message\n\n"
        << "With no source files, project commands discover vex.toml in the current directory or a parent.\n"
        << "Project settings: name, entry, backend (llvm or c), and output.\n"
        << "Imports are relative to the importing file; use 'as alias' and 'pub' to expose members.\n"
        << "build/run use the bundled Zig/LLVM toolchain by default; build-c/run-c require a C11 compiler.\n";
}
bool read_source_file(const std::string& path, std::string& contents) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::cerr << "error: could not open source file: " << path << '\n';
        return false;
    }
    contents.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    if (input.bad()) {
        std::cerr << "error: failed while reading source file: " << path << '\n';
        return false;
    }
    return true;
}
int run_lexer(const std::string& path) {
    std::string contents;
    if (!read_source_file(path, contents)) return 2;
    const vex::SourceFile source(path, std::move(contents));
    const vex::LexResult result = vex::Lexer(source).scan();
    for (const vex::Token& token : result.tokens) {
        const vex::SourcePosition position = source.position(token.span.begin);
        std::cout << position.line << ':' << position.column << "  "
                  << vex::token_kind_name(token.kind);
        if (vex::token_has_lexeme_value(token.kind))
            std::cout << "  " << std::quoted(std::string(token.text(source)));
        std::cout << '\n';
    }
    vex::print_diagnostics(std::cerr, source, result.diagnostics);
    return result.has_errors() ? 1 : 0;
}
int run_parser(const std::string& path) {
    std::string contents;
    if (!read_source_file(path, contents)) return 2;
    const vex::SourceFile source(path, std::move(contents));
    const vex::LexResult lexed = vex::Lexer(source).scan();
    if (lexed.has_errors()) {
        vex::print_diagnostics(std::cerr, source, lexed.diagnostics);
        return 1;
    }
    const vex::ParseResult parsed = vex::Parser(source, lexed.tokens).parse();
    vex::print_diagnostics(std::cerr, source, parsed.diagnostics);
    if (parsed.has_errors()) return 1;
    vex::print_ast(std::cout, source, parsed.program);
    return 0;
}
std::optional<vex::SourceFile> read_source_bundle(
    const std::vector<std::string>& paths) {
    return vex::load_source_bundle_with_imports(paths, std::cerr);
}

int run_check(const std::vector<std::string>& paths) {
    auto source_bundle = read_source_bundle(paths);
    if (!source_bundle) return 2;
    const vex::SourceFile& source = *source_bundle;
    const vex::LexResult lexed = vex::Lexer(source).scan();
    if (lexed.has_errors()) {
        vex::print_diagnostics(std::cerr, source, lexed.diagnostics);
        return 1;
    }
    vex::ParseResult parsed = vex::Parser(source, lexed.tokens).parse();
    if (parsed.has_errors()) {
        vex::print_diagnostics(std::cerr, source, parsed.diagnostics);
        return 1;
    }
    const vex::SemanticResult checked =
        vex::SemanticAnalyzer(source, parsed.program).analyze();
    vex::print_diagnostics(std::cerr, source, checked.diagnostics);
    if (checked.has_errors()) return 1;
    std::cout << "Semantic analysis passed.\n";
    return 0;
}

int run_ir(const std::vector<std::string>& paths) {
    auto source_bundle = read_source_bundle(paths);
    if (!source_bundle) return 2;
    const vex::SourceFile& source = *source_bundle;
    const vex::LexResult lexed = vex::Lexer(source).scan();
    if (lexed.has_errors()) {
        vex::print_diagnostics(std::cerr, source, lexed.diagnostics);
        return 1;
    }
    vex::ParseResult parsed = vex::Parser(source, lexed.tokens).parse();
    if (parsed.has_errors()) {
        vex::print_diagnostics(std::cerr, source, parsed.diagnostics);
        return 1;
    }
    const vex::SemanticResult checked =
        vex::SemanticAnalyzer(source, parsed.program).analyze();
    vex::print_diagnostics(std::cerr, source, checked.diagnostics);
    if (checked.has_errors()) return 1;

    const vex::IRLoweringResult lowered =
        vex::IRLowerer(source, parsed.program, checked.model).lower();
    vex::print_diagnostics(std::cerr, source, lowered.diagnostics);
    if (lowered.has_errors()) return 1;

    const vex::IRVerificationResult verified = vex::IRVerifier{}.verify(lowered.module);
    vex::print_diagnostics(std::cerr, source, verified.diagnostics);
    if (verified.has_errors()) return 1;

    vex::print_ir(std::cout, lowered.module);
    return 0;
}

int run_emit_backend(const std::vector<std::string>& paths, const vex::BackendKind kind) {
    auto source_bundle = read_source_bundle(paths);
    if (!source_bundle) return 2;
    const vex::SourceFile& source = *source_bundle;
    const vex::LexResult lexed = vex::Lexer(source).scan();
    if (lexed.has_errors()) {
        vex::print_diagnostics(std::cerr, source, lexed.diagnostics);
        return 1;
    }
    vex::ParseResult parsed = vex::Parser(source, lexed.tokens).parse();
    if (parsed.has_errors()) {
        vex::print_diagnostics(std::cerr, source, parsed.diagnostics);
        return 1;
    }
    const vex::SemanticResult checked =
        vex::SemanticAnalyzer(source, parsed.program).analyze();
    vex::print_diagnostics(std::cerr, source, checked.diagnostics);
    if (checked.has_errors()) return 1;

    const vex::IRLoweringResult lowered =
        vex::IRLowerer(source, parsed.program, checked.model).lower();
    vex::print_diagnostics(std::cerr, source, lowered.diagnostics);
    if (lowered.has_errors()) return 1;

    const std::unique_ptr<vex::Backend> backend =
        vex::create_backend(kind);
    if (!backend) {
        std::cerr << "error: the requested VEX backend is not available\n";
        return 1;
    }

    const vex::BackendResult generated = backend->emit(lowered.module);
    vex::print_diagnostics(std::cerr, source, generated.diagnostics);
    if (generated.has_errors()) return 1;

    std::cout << generated.output;
    return 0;
}

int run_emit_c(const std::vector<std::string>& paths) {
    return run_emit_backend(paths, vex::BackendKind::C);
}

int run_emit_llvm(const std::vector<std::string>& paths) {
    return run_emit_backend(paths, vex::BackendKind::LLVM);
}
} // namespace
int main(const int argc, char* argv[]) {
    if (argc == 2) {
        const std::string argument = argv[1];
        if (argument == "--help" || argument == "-h") {
            print_help();
            return 0;
        }
        if (argument == "--version" || argument == "-v") {
            std::cout << "VEX compiler " << VEX_VERSION << '\n';
            return 0;
        }
    }
    if (argc < 2) {
        print_help();
        return 2;
    }

    const std::string command = argv[1];
    if (command == "init" && argc <= 3)
        return vex::initialize_project(argc == 3 ? argv[2] : "", std::cerr) ? 0 : 1;

    const bool build_command = command == "build" ||
        command == "build-c" || command == "build-llvm";
    const bool known_command = command == "lex" || command == "parse" ||
        command == "check" || command == "ir" || command == "emit-c" ||
        command == "emit-llvm" || build_command || command == "run" ||
        command == "run-c" || command == "run-llvm";
    if (!known_command) {
        print_help();
        return 2;
    }

    if (command == "lex" || command == "parse") {
        if (argc != 3) {
            print_help();
            return 2;
        }
        return command == "lex" ? run_lexer(argv[2]) : run_parser(argv[2]);
    }

    std::vector<std::string> paths;
    std::string output_argument;
    for (int index = 2; index < argc; ++index) {
        const std::string argument = argv[index];
        if (build_command && argument == "-o") {
            if (index + 1 >= argc || !output_argument.empty()) {
                std::cerr << "error: -o requires exactly one output path\n";
                return 2;
            }
            output_argument = argv[++index];
        } else {
            paths.push_back(argument);
        }
    }

    std::optional<vex::ProjectConfig> project;
    const bool project_mode = paths.empty();
    if (project_mode) {
        project = vex::discover_project_config(std::cerr);
        if (!project) return 2;
        paths.push_back(project->entry.string());
    }
    if (paths.empty()) return 2;

    if (command == "check") return run_check(paths);
    if (command == "ir") return run_ir(paths);
    if (command == "emit-c") return run_emit_c(paths);
    if (command == "emit-llvm") return run_emit_llvm(paths);

    const bool force_c = command == "build-c" || command == "run-c";
    const bool force_llvm = command == "build-llvm" || command == "run-llvm";
    const bool use_c = force_c ||
        (!force_llvm && command == "build" && project_mode &&
         project->backend == "c") ||
        (!force_llvm && command == "run" && project_mode &&
         project->backend == "c");

    if (command == "build" || command == "build-c" || command == "build-llvm") {
        std::string output_path = output_argument;
        if (output_path.empty() && project_mode) output_path = project->output.string();
        if (project_mode && !output_path.empty()) {
            std::filesystem::path resolved_output(output_path);
            if (!resolved_output.is_absolute())
                resolved_output = project->root / resolved_output;
            const std::filesystem::path normalized_output =
                resolved_output.lexically_normal();
            if (normalized_output == project->entry.lexically_normal() ||
                normalized_output == project->manifest.lexically_normal()) {
                std::cerr << "error: build output must not overwrite the project manifest or entry source\\n";
                return 2;
            }
            std::error_code error;
            std::filesystem::create_directories(resolved_output.parent_path(), error);
            if (error) {
                std::cerr << "error: cannot create project build directory: "
                          << error.message() << '\n';
                return 1;
            }
            output_path = resolved_output.string();
        }
        return use_c
            ? vex::build_program(paths, output_path)
            : vex::build_llvm_program(paths, output_path);
    }

    if (command == "run" || command == "run-c" || command == "run-llvm")
        return use_c ? vex::run_program(paths) : vex::run_llvm_program(paths);

    print_help();
    return 2;
}
