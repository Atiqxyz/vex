#include "vex/ast.hpp"
#include "vex/backend.hpp"
#include "vex/diagnostic.hpp"
#include "vex/lexer.hpp"
#include "vex/ir.hpp"
#include "vex/lowering.hpp"
#include "vex/parser.hpp"
#include "vex/semantic.hpp"
#include "vex/source.hpp"
#include "vex/token.hpp"
#include "vex/toolchain.hpp"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#ifndef VEX_VERSION
#define VEX_VERSION "0.1.0"
#endif
namespace {
void print_help() {
    std::cout
        << "VEX compiler " << VEX_VERSION << "\n"
        << "Usage:\n"
        << "  vex lex <file.vex>      Tokenize a VEX source file\n"
        << "  vex parse <file.vex>    Parse a VEX source file and print its AST\n"
        << "  vex check <file.vex>    Resolve names and check static types\n"
        << "  vex ir <file.vex>       Lower a valid program to typed SSA IR\n"
        << "  vex emit-c <file.vex>   Emit portable C11 from verified VEX IR\n"
        << "  vex emit-llvm <file.vex> Emit LLVM IR from verified VEX IR\n"
        << "  vex build <file.vex>    Build a native executable using LLVM\n"
        << "  vex run <file.vex>      Build and execute using the bundled LLVM toolchain\n"
        << "  vex build-c <file.vex>  Build using the optional C11 backend\n"
        << "  vex run-c <file.vex>    Build and execute using the optional C11 backend\n"
        << "  vex build-llvm <file.vex> Build a native executable using LLVM\n"
        << "  vex build-llvm <file.vex> -o <output>  Choose executable path\n"
        << "  vex run-llvm <file.vex> Build and execute using LLVM\n"
        << "  vex --version           Show compiler version\n"
        << "  vex --help              Show this help message\n\n"
        << "emit-c and emit-llvm write generated source to standard output.\n"
        << "build/run use the bundled Zig/LLVM toolchain (or installed Clang); build-c/run-c require a C11 compiler.\n";
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
int run_check(const std::string& path) {
    std::string contents;
    if (!read_source_file(path, contents)) return 2;
    const vex::SourceFile source(path, std::move(contents));
    const vex::LexResult lexed = vex::Lexer(source).scan();
    if (lexed.has_errors()) {
        vex::print_diagnostics(std::cerr, source, lexed.diagnostics);
        return 1;
    }
    const vex::ParseResult parsed = vex::Parser(source, lexed.tokens).parse();
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

int run_ir(const std::string& path) {
    std::string contents;
    if (!read_source_file(path, contents)) return 2;
    const vex::SourceFile source(path, std::move(contents));
    const vex::LexResult lexed = vex::Lexer(source).scan();
    if (lexed.has_errors()) {
        vex::print_diagnostics(std::cerr, source, lexed.diagnostics);
        return 1;
    }
    const vex::ParseResult parsed = vex::Parser(source, lexed.tokens).parse();
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

int run_emit_backend(const std::string& path, const vex::BackendKind kind) {
    std::string contents;
    if (!read_source_file(path, contents)) return 2;
    const vex::SourceFile source(path, std::move(contents));
    const vex::LexResult lexed = vex::Lexer(source).scan();
    if (lexed.has_errors()) {
        vex::print_diagnostics(std::cerr, source, lexed.diagnostics);
        return 1;
    }
    const vex::ParseResult parsed = vex::Parser(source, lexed.tokens).parse();
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

int run_emit_c(const std::string& path) {
    return run_emit_backend(path, vex::BackendKind::C);
}

int run_emit_llvm(const std::string& path) {
    return run_emit_backend(path, vex::BackendKind::LLVM);
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
    if (argc == 3) {
        const std::string command = argv[1];
        if (command == "lex") return run_lexer(argv[2]);
        if (command == "parse") return run_parser(argv[2]);
        if (command == "check") return run_check(argv[2]);
        if (command == "ir") return run_ir(argv[2]);
        if (command == "emit-c") return run_emit_c(argv[2]);
        if (command == "emit-llvm") return run_emit_llvm(argv[2]);
        if (command == "build") return vex::build_llvm_program(argv[2]);
        if (command == "run") return vex::run_llvm_program(argv[2]);
        if (command == "build-c") return vex::build_program(argv[2]);
        if (command == "run-c") return vex::run_program(argv[2]);
        if (command == "build-llvm") return vex::build_llvm_program(argv[2]);
        if (command == "run-llvm") return vex::run_llvm_program(argv[2]);
    }
    if (argc == 5 && std::string(argv[3]) == "-o") {
        const std::string command = argv[1];
        if (command == "build") return vex::build_llvm_program(argv[2], argv[4]);
        if (command == "build-c") return vex::build_program(argv[2], argv[4]);
        if (command == "build-llvm") return vex::build_llvm_program(argv[2], argv[4]);
    }
    print_help();
    return 2;
}
