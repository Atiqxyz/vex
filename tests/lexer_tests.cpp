#include "vex/lexer.hpp"
#include "vex/token.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

std::vector<vex::TokenKind> kinds(const vex::LexResult& result) {
    std::vector<vex::TokenKind> output;
    output.reserve(result.tokens.size());
    for (const vex::Token& token : result.tokens) {
        output.push_back(token.kind);
    }
    return output;
}

void test_keywords_and_identifiers() {
    vex::Lexer lexer{"make main() { let score = 12 const ratio: float = 1.5 }"};
    const vex::LexResult result = lexer.scan();
    const auto actual = kinds(result);
    const std::vector<vex::TokenKind> expected{
        vex::TokenKind::KwMake, vex::TokenKind::Identifier,
        vex::TokenKind::LeftParen, vex::TokenKind::RightParen,
        vex::TokenKind::LeftBrace, vex::TokenKind::KwLet,
        vex::TokenKind::Identifier, vex::TokenKind::Equal,
        vex::TokenKind::Integer, vex::TokenKind::KwConst,
        vex::TokenKind::Identifier, vex::TokenKind::Colon,
        vex::TokenKind::KwFloat, vex::TokenKind::Equal,
        vex::TokenKind::Float, vex::TokenKind::RightBrace,
        vex::TokenKind::EndOfFile
    };
    check(actual == expected, "keywords, identifiers, and numeric literals");
    check(result.diagnostics.empty(), "valid keyword sample has no diagnostics");
}

void test_newlines_and_comments() {
    vex::Lexer lexer{"let x = 1 // line comment\n/* block\ncomment */let y = 2"};
    const vex::LexResult result = lexer.scan();
    const auto actual = kinds(result);
    std::size_t newline_count = 0;
    for (const auto kind : actual) {
        if (kind == vex::TokenKind::Newline) ++newline_count;
    }
    check(newline_count == 2, "newlines survive line and block comments");
    check(result.diagnostics.empty(), "closed comments have no diagnostics");
}

void test_operators_and_exponents() {
    vex::Lexer lexer{"a -> b == 2 && c != 3 || d <= 4 && 1.25e-2 > 0"};
    const vex::LexResult result = lexer.scan();
    const auto actual = kinds(result);
    check(actual.size() > 10, "operator sample produces tokens");
    check(result.diagnostics.empty(), "operators and exponent notation tokenize cleanly");
    bool found_arrow = false;
    bool found_and = false;
    bool found_float = false;
    for (const auto kind : actual) {
        found_arrow = found_arrow || kind == vex::TokenKind::Arrow;
        found_and = found_and || kind == vex::TokenKind::AndAnd;
        found_float = found_float || kind == vex::TokenKind::Float;
    }
    check(found_arrow, "arrow operator is recognized");
    check(found_and, "logical AND is recognized");
    check(found_float, "exponent-form float is recognized");
}

void test_strings_and_errors() {
    vex::Lexer valid{"say(\"hello\\nVEX\")"};
    const vex::LexResult valid_result = valid.scan();
    check(valid_result.diagnostics.empty(), "supported string escapes are accepted");

    vex::Lexer invalid{"let value = 1;"};
    const vex::LexResult invalid_result = invalid.scan();
    check(!invalid_result.diagnostics.empty(), "semicolon produces a diagnostic");
    check(invalid_result.diagnostics.front().message.find("semicolons") != std::string::npos,
          "semicolon diagnostic explains newline statement separation");

    vex::Lexer unterminated{"say(\"unfinished"};
    const vex::LexResult unterminated_result = unterminated.scan();
    check(!unterminated_result.diagnostics.empty(), "unterminated string is diagnosed");
}

} // namespace

int main() {
    test_keywords_and_identifiers();
    test_newlines_and_comments();
    test_operators_and_exponents();
    test_strings_and_errors();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }

    std::cout << "All VEX lexer tests passed.\n";
    return 0;
}
