#include "test_support.hpp"
#include "vex/ast.hpp"
#include "vex/lexer.hpp"
#include "vex/parser.hpp"
#include "vex/source.hpp"
#include <variant>

namespace {

vex::ParseResult parse(const vex::SourceFile& source) {
    const vex::LexResult lexed = vex::Lexer(source).scan();
    if (lexed.has_errors()) return vex::ParseResult{};
    return vex::Parser(source, lexed.tokens).parse();
}

const vex::VariableDeclStmt* first_variable(const vex::Program& program) {
    if (program.statements.empty()) return nullptr;
    return std::get_if<vex::VariableDeclStmt>(&program.statements.front()->data);
}

void test_operator_precedence(TestContext& tests) {
    const vex::SourceFile source{
        "precedence.vex", "let result = a + b * c == d && ready\n"
    };
    const vex::ParseResult parsed = parse(source);
    tests.check(!parsed.has_errors(), "valid precedence expression parses");

    const auto* variable = first_variable(parsed.program);
    tests.check(variable != nullptr, "variable declaration appears in the AST");
    if (!variable || !variable->initializer) return;

    const auto* logical = std::get_if<vex::BinaryExpr>(&variable->initializer->data);
    tests.check(logical && logical->op == vex::TokenKind::AndAnd,
                "logical AND has lower precedence than equality");
    if (!logical || !logical->left) return;

    const auto* equality = std::get_if<vex::BinaryExpr>(&logical->left->data);
    tests.check(equality && equality->op == vex::TokenKind::EqualEqual,
                "equality has lower precedence than addition");
    if (!equality || !equality->left) return;

    const auto* addition = std::get_if<vex::BinaryExpr>(&equality->left->data);
    tests.check(addition && addition->op == vex::TokenKind::Plus,
                "addition is the left operand of equality");
    if (!addition || !addition->right) return;

    const auto* multiplication =
        std::get_if<vex::BinaryExpr>(&addition->right->data);
    tests.check(multiplication && multiplication->op == vex::TokenKind::Star,
                "multiplication binds more tightly than addition");
}

void test_functions_and_calls(TestContext& tests) {
    const vex::SourceFile source{"functions.vex",
        "make calculate_force(mass: float, acceleration: float) -> float {\n"
        "    return mass * acceleration\n"
        "}\n"
        "make main() {\n"
        "    let force = calculate_force(2.0, 9.81)\n"
        "    say(force)\n"
        "}\n"};
    const vex::ParseResult parsed = parse(source);
    tests.check(!parsed.has_errors(), "function declarations and calls parse");
    tests.check(parsed.program.statements.size() == 2,
                "both top-level function declarations are retained");
    if (parsed.program.statements.size() < 2) return;

    const auto* function =
        std::get_if<vex::FunctionDeclStmt>(&parsed.program.statements.front()->data);
    tests.check(function != nullptr, "first top-level node is a function");
    if (function) {
        tests.check(function->parameters.size() == 2,
                    "function parameter list is represented in the AST");
        tests.check(function->return_type.has_value(),
                    "function return type is represented in the AST");
    }

    const auto* main_function =
        std::get_if<vex::FunctionDeclStmt>(&parsed.program.statements[1]->data);
    tests.check(main_function != nullptr, "second top-level node is main");
    if (!main_function || !main_function->body) return;

    const auto* body = std::get_if<vex::BlockStmt>(&main_function->body->data);
    tests.check(body && body->statements.size() == 2,
                "function body contains its declaration and call statements");
}

void test_multiline_expression(TestContext& tests) {
    const vex::SourceFile source{"multiline.vex",
        "let force = [\n"
        "    mass *\n"
        "    acceleration\n"
        "]\n"};
    const vex::ParseResult parsed = parse(source);
    tests.check(!parsed.has_errors(), "bracketed multiline expression parses");

    const auto* variable = first_variable(parsed.program);
    tests.check(variable && variable->initializer,
                "multiline expression is retained as a variable initializer");
    if (!variable || !variable->initializer) return;

    const auto* grouping = std::get_if<vex::GroupExpr>(&variable->initializer->data);
    tests.check(grouping && grouping->multiline,
                "square brackets are represented as multiline grouping");
    if (grouping && grouping->expression) {
        const auto* multiplication =
            std::get_if<vex::BinaryExpr>(&grouping->expression->data);
        tests.check(multiplication && multiplication->op == vex::TokenKind::Star,
                    "multiline body contains the intended expression");
    }
}

void test_control_flow_and_assignment(TestContext& tests) {
    const vex::SourceFile source{"control.vex",
        "make main() {\n"
        "    let count = 0\n"
        "    if count < 5 {\n"
        "        count = count + 1\n"
        "    } else {\n"
        "        return\n"
        "    }\n"
        "    repeat 3 times {\n"
        "        say(\"x\")\n"
        "    }\n"
        "    for item in items {\n"
        "        say(item)\n"
        "    }\n"
        "}\n"};
    const vex::ParseResult parsed = parse(source);
    tests.check(!parsed.has_errors(),
                "if/else, assignment, repeat, and for statements parse");
}

void test_invalid_syntax_and_newline_rules(TestContext& tests) {
    const vex::SourceFile missing_expression{"missing.vex", "let value =\n"};
    const vex::LexResult missing_lex = vex::Lexer(missing_expression).scan();
    const vex::ParseResult missing =
        vex::Parser(missing_expression, missing_lex.tokens).parse();
    tests.check(missing.has_errors(), "missing expression produces a parser diagnostic");

    const vex::SourceFile missing_separator{
        "separator.vex", "let first = 1 let second = 2\n"
    };
    const vex::LexResult separator_lex = vex::Lexer(missing_separator).scan();
    const vex::ParseResult separator =
        vex::Parser(missing_separator, separator_lex.tokens).parse();
    tests.check(separator.has_errors(), "statements without newline are rejected");

    const vex::SourceFile unwrapped_newline{
        "continuation.vex", "let value = mass *\n    acceleration\n"
    };
    const vex::LexResult continuation_lex = vex::Lexer(unwrapped_newline).scan();
    const vex::ParseResult continuation =
        vex::Parser(unwrapped_newline, continuation_lex.tokens).parse();
    tests.check(continuation.has_errors(),
                "expression cannot continue across newline without brackets");
}

} // namespace

void run_parser_tests(TestContext& tests) {
    test_operator_precedence(tests);
    test_functions_and_calls(tests);
    test_multiline_expression(tests);
    test_control_flow_and_assignment(tests);
    test_invalid_syntax_and_newline_rules(tests);
}
