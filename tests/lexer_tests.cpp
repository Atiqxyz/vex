#include "test_support.hpp"

#include "vex/lexer.hpp"
#include "vex/source.hpp"
#include "vex/token.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace {

std::vector<vex::TokenKind> token_kinds(const vex::LexResult& result) {
    std::vector<vex::TokenKind> output;
    output.reserve(result.tokens.size());
    for (const vex::Token& token : result.tokens) {
        output.push_back(token.kind);
    }
    return output;
}

std::size_t count_kind(const vex::LexResult& result, const vex::TokenKind kind) {
    return static_cast<std::size_t>(std::count_if(
        result.tokens.begin(), result.tokens.end(),
        [kind](const vex::Token& token) { return token.kind == kind; }));
}

bool has_diagnostic(const vex::LexResult& result, const std::string& code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [&code](const vex::Diagnostic& diagnostic) {
            return diagnostic.code == code;
        });
}

void test_keywords_and_identifiers(TestContext& tests) {
    const vex::SourceFile source{"keywords.vex", "make main() {}"};
    const vex::LexResult result = vex::Lexer{source}.scan();
    const std::vector<vex::TokenKind> expected{
        vex::TokenKind::KwMake, vex::TokenKind::Identifier,
        vex::TokenKind::LeftParen, vex::TokenKind::RightParen,
        vex::TokenKind::LeftBrace, vex::TokenKind::RightBrace,
        vex::TokenKind::EndOfFile
    };

    tests.check(token_kinds(result) == expected,
                "function declaration keywords and punctuation tokenize in order");
    tests.check(!result.has_errors(), "valid declaration has no diagnostics");
    tests.check(result.tokens[1].text(source) == "main",
                "token text is resolved from its source span");
}

void test_numeric_literals(TestContext& tests) {
    const vex::SourceFile source{"numbers.vex", "0 42 1.5 1e4 1.25e-2"};
    const vex::LexResult result = vex::Lexer{source}.scan();

    tests.check(count_kind(result, vex::TokenKind::Integer) == 2,
                "integer literals are recognized");
    tests.check(count_kind(result, vex::TokenKind::Float) == 3,
                "decimal and exponent float literals are recognized");
    tests.check(!result.has_errors(), "valid numeric literals have no diagnostics");

    const vex::SourceFile malformed{"bad-number.vex", "12e+"};
    const vex::LexResult malformed_result = vex::Lexer{malformed}.scan();
    tests.check(has_diagnostic(malformed_result, "LEX005"),
                "incomplete exponent receives a stable diagnostic code");
    tests.check(count_kind(malformed_result, vex::TokenKind::Invalid) == 1,
                "malformed numeric literal becomes an invalid token");
}

void test_newlines_and_comments(TestContext& tests) {
    const vex::SourceFile source{
        "comments.vex",
        "let x = 1 // line comment\r\n/* block\ncomment */let y = 2"
    };
    const vex::LexResult result = vex::Lexer{source}.scan();

    tests.check(count_kind(result, vex::TokenKind::Newline) == 2,
                "newlines survive line and block comments");
    tests.check(!result.has_errors(), "closed comments have no diagnostics");

    const vex::SourceFile unterminated{"comment.vex", "/* never closed"};
    const vex::LexResult bad_comment = vex::Lexer{unterminated}.scan();
    tests.check(has_diagnostic(bad_comment, "LEX009"),
                "unterminated block comment is diagnosed");
}

void test_operators(TestContext& tests) {
    const vex::SourceFile source{
        "operators.vex",
        "a -> b == 2 && c != 3 || d <= 4 && 1.25e-2 > 0"
    };
    const vex::LexResult result = vex::Lexer{source}.scan();

    tests.check(count_kind(result, vex::TokenKind::Arrow) == 1,
                "arrow token is recognized");
    tests.check(count_kind(result, vex::TokenKind::AndAnd) == 2,
                "logical AND token is recognized");
    tests.check(count_kind(result, vex::TokenKind::OrOr) == 1,
                "logical OR token is recognized");
    tests.check(count_kind(result, vex::TokenKind::EqualEqual) == 1,
                "equality token is recognized");
    tests.check(count_kind(result, vex::TokenKind::BangEqual) == 1,
                "inequality token is recognized");
    tests.check(count_kind(result, vex::TokenKind::LessEqual) == 1,
                "less-than-or-equal token is recognized");
    tests.check(!result.has_errors(), "supported operators tokenize cleanly");
}

void test_strings_and_invalid_input(TestContext& tests) {
    const vex::SourceFile valid{
        "strings.vex", "say(\"hello\\nVEX\\t\")"
    };
    const vex::LexResult valid_result = vex::Lexer{valid}.scan();
    tests.check(count_kind(valid_result, vex::TokenKind::String) == 1,
                "supported string escapes are accepted");
    tests.check(!valid_result.has_errors(), "valid escaped string has no diagnostic");

    const vex::SourceFile bad_escape{"escape.vex", "say(\"bad\\q\")"};
    const vex::LexResult escape_result = vex::Lexer{bad_escape}.scan();
    tests.check(has_diagnostic(escape_result, "LEX002"),
                "unsupported string escape is diagnosed");

    const vex::SourceFile semicolon{"semicolon.vex", "let value = 1;"};
    const vex::LexResult semicolon_result = vex::Lexer{semicolon}.scan();
    tests.check(has_diagnostic(semicolon_result, "LEX008"),
                "semicolon gets an actionable statement-separator diagnostic");

    const vex::SourceFile newline_string{"newline.vex", "say(\"first\nsecond\")"};
    const vex::LexResult newline_result = vex::Lexer{newline_string}.scan();
    tests.check(has_diagnostic(newline_result, "LEX004"),
                "raw newline inside a string is diagnosed");

    const vex::SourceFile lone_operator{"operator.vex", "a & b | c"};
    const vex::LexResult operator_result = vex::Lexer{lone_operator}.scan();
    tests.check(has_diagnostic(operator_result, "LEX006") &&
                has_diagnostic(operator_result, "LEX007"),
                "single ampersand and vertical bar are diagnosed");

    const vex::SourceFile unexpected{"unexpected.vex", "@"};
    const vex::LexResult unexpected_result = vex::Lexer{unexpected}.scan();
    tests.check(has_diagnostic(unexpected_result, "LEX001"),
                "unknown character has a stable diagnostic code");
}

void test_diagnostic_positions(TestContext& tests) {
    const vex::SourceFile source{"position.vex", "let ok = 1\r\n@"};
    const vex::LexResult result = vex::Lexer{source}.scan();

    tests.check(result.diagnostics.size() == 1,
                "one invalid character creates one diagnostic");
    if (!result.diagnostics.empty()) {
        const vex::SourcePosition position =
            source.position(result.diagnostics.front().span.begin);
        tests.check(position.line == 2 && position.column == 1,
                    "diagnostic source span maps to the right line and column");
    }
}

} // namespace

void run_lexer_tests(TestContext& tests) {
    test_keywords_and_identifiers(tests);
    test_numeric_literals(tests);
    test_newlines_and_comments(tests);
    test_operators(tests);
    test_strings_and_invalid_input(tests);
    test_diagnostic_positions(tests);
}
