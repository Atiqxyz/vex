#include "vex/diagnostic.hpp"
#include "vex/lexer.hpp"
#include "vex/source.hpp"
#include "vex/token.hpp"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
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
        << "  vex lex <file.vex>   Tokenize a VEX source file\n"
        << "  vex --version        Show compiler version\n"
        << "  vex --help           Show this help message\n\n"
        << "The current development build provides lexical analysis only.\n"
        << "Parsing and code generation are not implemented yet.\n";
}

int run_lexer(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::cerr << "error: could not open source file: " << path << '\n';
        return 2;
    }

    std::string contents{std::istreambuf_iterator<char>(input),
                         std::istreambuf_iterator<char>()};
    if (input.bad()) {
        std::cerr << "error: failed while reading source file: " << path << '\n';
        return 2;
    }

    const vex::SourceFile source(path, std::move(contents));
    vex::Lexer lexer(source);
    const vex::LexResult result = lexer.scan();

    for (const vex::Token& token : result.tokens) {
        const vex::SourcePosition position = source.position(token.span.begin);
        std::cout << position.line << ':' << position.column << "  "
                  << vex::token_kind_name(token.kind);

        if (vex::token_has_lexeme_value(token.kind)) {
            std::cout << "  " << std::quoted(std::string(token.text(source)));
        }
        std::cout << '\n';
    }

    vex::print_diagnostics(std::cerr, source, result.diagnostics);
    return result.has_errors() ? 1 : 0;
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

    if (argc == 3 && std::string{argv[1]} == "lex") {
        return run_lexer(argv[2]);
    }

    print_help();
    return 2;
}
