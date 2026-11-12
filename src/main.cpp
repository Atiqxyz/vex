#include "vex/lexer.hpp"
#include "vex/token.hpp"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <string>

namespace {

constexpr const char* version = "0.1.0-dev";

void print_help() {
    std::cout << "VEX compiler " << version << "\n"
              << "Usage:\n"
              << "  vex lex <file.vex>   Tokenize a VEX source file\n"
              << "  vex --version        Show compiler version\n"
              << "  vex --help           Show this help message\n\n"
              << "The 0.1 development build provides lexer inspection only;\n"
              << "parsing and code generation are not implemented yet.\n";
}

int run_lexer(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::cerr << "error: could not open source file: " << path << '\n';
        return 2;
    }

    const std::string source{std::istreambuf_iterator<char>(input),
                             std::istreambuf_iterator<char>()};
    vex::Lexer lexer(source);
    vex::LexResult result = lexer.scan();

    for (const vex::Token& token : result.tokens) {
        std::cout << token.line << ':' << token.column << "  "
                  << vex::token_kind_name(token.kind);
        if (vex::token_has_lexeme_value(token.kind)) {
            std::cout << "  " << std::quoted(token.lexeme);
        }
        std::cout << '\n';
    }

    for (const vex::Diagnostic& diagnostic : result.diagnostics) {
        std::cerr << path << ':' << diagnostic.line << ':' << diagnostic.column
                  << ": error: " << diagnostic.message << '\n';
    }

    return result.diagnostics.empty() ? 0 : 1;
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
            std::cout << "VEX compiler " << version << '\n';
            return 0;
        }
    }

    if (argc == 3 && std::string{argv[1]} == "lex") {
        return run_lexer(argv[2]);
    }

    print_help();
    return 2;
}
