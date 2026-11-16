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

void test_bitwise_precedence(TestContext& tests) {
    const vex::SourceFile source{"bitwise_precedence.vex",
        "let result = a | b ^ c & d == e && ready\n"};
    const vex::ParseResult parsed = parse(source);
    tests.check(!parsed.has_errors(), "bitwise expression precedence parses");
    const auto* variable = first_variable(parsed.program);
    if (!variable || !variable->initializer) return;
    const auto* logical = std::get_if<vex::BinaryExpr>(&variable->initializer->data);
    tests.check(logical && logical->op == vex::TokenKind::AndAnd,
                "logical AND remains lower precedence than bitwise and equality");
    if (!logical || !logical->left) return;
    const auto* bit_or = std::get_if<vex::BinaryExpr>(&logical->left->data);
    tests.check(bit_or && bit_or->op == vex::TokenKind::BitOr,
                "bitwise OR has the lowest bitwise precedence");
    if (!bit_or || !bit_or->right) return;
    const auto* bit_xor = std::get_if<vex::BinaryExpr>(&bit_or->right->data);
    tests.check(bit_xor && bit_xor->op == vex::TokenKind::BitXor,
                "bitwise XOR binds more tightly than OR");
    if (!bit_xor || !bit_xor->right) return;
    const auto* bit_and = std::get_if<vex::BinaryExpr>(&bit_xor->right->data);
    tests.check(bit_and && bit_and->op == vex::TokenKind::BitAnd,
                "bitwise AND binds more tightly than XOR");
    if (!bit_and || !bit_and->right) return;
    const auto* equality = std::get_if<vex::BinaryExpr>(&bit_and->right->data);
    tests.check(equality && equality->op == vex::TokenKind::EqualEqual,
                "equality binds more tightly than bitwise operators");
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

void test_unary_expression_in_call_argument(TestContext& tests) {
    const vex::SourceFile source{"unary_call_argument.vex",
        "make main() {\n"
        "    let magnitude = abs(-5.0)\n"
        "    say(magnitude)\n"
        "}\n"};
    const vex::ParseResult parsed = parse(source);
    tests.check(!parsed.has_errors(),
                "unary minus inside a function-call argument parses");
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
        "    count += 2\n"
        "    count -= 1\n"
        "    count *= 3\n"
        "    count /= 2\n"
        "    count %= 7\n"
        "    while count < 20 {\n"
        "        count -= 1\n"
        "        if count == 10 { continue }\n"
        "        if count == 5 { break }\n"
        "    }\n"
        "    for item in items {\n"
        "        say(item)\n"
        "    }\n"
        "}\n"};
    const vex::ParseResult parsed = parse(source);
    tests.check(!parsed.has_errors(),
                "if/else, compound assignment, repeat, while, loop control, and for statements parse");
}


void test_array_literal_indexing_and_assignment(TestContext& tests) {
    const vex::SourceFile source{"arrays.vex",
        "make main() {\n"
        "    let values = [10, 20, 30]\n"
        "    say(values[1])\n"
        "    values[2] = 42\n"
        "    values[1] += 5\n"
        "}\n"};
    const vex::ParseResult parsed = parse(source);
    tests.check(!parsed.has_errors(),
                "array literals, indexing, and indexed assignment parse");
    if (parsed.has_errors()) return;
    const auto* function =
        std::get_if<vex::FunctionDeclStmt>(&parsed.program.statements.front()->data);
    if (!function || !function->body) return;
    const auto* body = std::get_if<vex::BlockStmt>(&function->body->data);
    if (!body || body->statements.size() < 4) return;
    const auto* variable =
        std::get_if<vex::VariableDeclStmt>(&body->statements[0]->data);
    tests.check(variable && variable->initializer &&
                std::holds_alternative<vex::ArrayLiteralExpr>(variable->initializer->data),
                "comma-separated brackets produce an array literal AST node");
    const auto* output =
        std::get_if<vex::ExpressionStmt>(&body->statements[1]->data);
    const auto* call = output && output->expression
        ? std::get_if<vex::CallExpr>(&output->expression->data) : nullptr;
    tests.check(call && call->arguments.size() == 1 &&
                std::holds_alternative<vex::IndexExpr>(call->arguments[0]->data),
                "postfix brackets produce an array-index AST node");
    const auto* assignment_stmt =
        std::get_if<vex::ExpressionStmt>(&body->statements[2]->data);
    const auto* assignment = assignment_stmt && assignment_stmt->expression
        ? std::get_if<vex::AssignmentExpr>(&assignment_stmt->expression->data) : nullptr;
    tests.check(assignment && assignment->target &&
                std::holds_alternative<vex::IndexExpr>(assignment->target->data),
                "array indexing can be used as a simple assignment target");
    const auto* compound_stmt=std::get_if<vex::ExpressionStmt>(&body->statements[3]->data);
    const auto* compound=compound_stmt&&compound_stmt->expression
        ?std::get_if<vex::AssignmentExpr>(&compound_stmt->expression->data):nullptr;
    tests.check(compound&&compound->op==vex::TokenKind::PlusEqual&&compound->target&&
                std::holds_alternative<vex::IndexExpr>(compound->target->data),
                "indexed compound assignment keeps its operator and index target");
}

void test_integer_range_expressions(TestContext& tests) {
    const vex::SourceFile source{"ranges.vex",
        "make main() {\n"
        "    for index in 1 + 2..10 - 1 { say(index) }\n"
        "    for index in 2..=5 { say(index) }\n"
        "}\n"};
    const vex::ParseResult parsed = parse(source);
    tests.check(!parsed.has_errors(), "half-open and inclusive for-in ranges parse");
    if (parsed.has_errors() || parsed.program.statements.empty()) return;
    const auto* function = std::get_if<vex::FunctionDeclStmt>(&parsed.program.statements.front()->data);
    const auto* body = function && function->body
        ? std::get_if<vex::BlockStmt>(&function->body->data) : nullptr;
    if (!body || body->statements.size() < 2) return;
    const auto* first = std::get_if<vex::ForStmt>(&body->statements[0]->data);
    const auto* second = std::get_if<vex::ForStmt>(&body->statements[1]->data);
    const auto* half_open = first && first->iterable ? std::get_if<vex::RangeExpr>(&first->iterable->data) : nullptr;
    const auto* inclusive = second && second->iterable ? std::get_if<vex::RangeExpr>(&second->iterable->data) : nullptr;
    tests.check(half_open && !half_open->inclusive, "start..end creates a half-open range node");
    tests.check(inclusive && inclusive->inclusive, "start..=end creates an inclusive range node");
    const auto* start = half_open && half_open->start ? std::get_if<vex::BinaryExpr>(&half_open->start->data) : nullptr;
    const auto* end = half_open && half_open->end ? std::get_if<vex::BinaryExpr>(&half_open->end->data) : nullptr;
    tests.check(start && start->op == vex::TokenKind::Plus && end && end->op == vex::TokenKind::Minus,
                "range endpoints retain normal arithmetic precedence");
}

void test_if_without_else_preserves_next_statement(TestContext& tests) {
    const vex::SourceFile source{"if_separator.vex",
        "make main() {\n"
        "    if true {\n"
        "        say(\"inside\")\n"
        "    }\n"
        "    say(\"after\")\n"
        "}\n"};
    const vex::ParseResult parsed = parse(source);
    tests.check(!parsed.has_errors(),
                "an if without else preserves the newline before the next statement");
}


void test_structure_declarations_literals_and_members(TestContext& tests) {
    const vex::SourceFile source{"structures.vex",
        "struct Point { x: int, y: int, }\n"
        "make main() {\n"
        "    let point = Point { x: 2, y: 3 }\n"
        "    point.x = 5\n"
        "    let points = [Point { x: 2, y: 3 }]\n"
        "    say(point.y)\n"
        "}\n"};
    const vex::ParseResult parsed = parse(source);
    tests.check(!parsed.has_errors(),
                "structure declarations, named literals, member reads, and writes parse");
    if (parsed.has_errors() || parsed.program.statements.size() < 2) return;
    const auto* structure =
        std::get_if<vex::StructDeclStmt>(&parsed.program.statements[0]->data);
    tests.check(structure && structure->fields.size() == 2,
                "structure declarations retain their ordered field list");
    const auto* function =
        std::get_if<vex::FunctionDeclStmt>(&parsed.program.statements[1]->data);
    const auto* body = function && function->body
        ? std::get_if<vex::BlockStmt>(&function->body->data) : nullptr;
    const auto* variable = body && !body->statements.empty()
        ? std::get_if<vex::VariableDeclStmt>(&body->statements[0]->data) : nullptr;
    tests.check(variable && variable->initializer &&
                std::holds_alternative<vex::StructLiteralExpr>(variable->initializer->data),
                "named structure construction produces a structure literal node");
}

void test_multiline_structure_literal_with_array_fields(TestContext& tests) {
    const vex::SourceFile source{"multiline_structure_literal.vex",
        "struct Point { x: int, y: int }\n"
        "struct PointList { points: Point[2] }\n"
        "make main() {\n"
        "    let catalog = PointList {\n"
        "        points: [\n"
        "            Point { x: 1, y: 2 },\n"
        "            Point { x: 3, y: 4 }\n"
        "        ]\n"
        "    }\n"
        "}\n"};
    const vex::ParseResult parsed = parse(source);
    tests.check(!parsed.has_errors(),
                "multiline structure literals accept a newline after '{' and array-valued fields");
}

void test_multidimensional_type_annotation_spans(TestContext& tests) {
    const vex::SourceFile source{"nested_array_annotations.vex",
        "struct Matrix { values: float[2][3] }\n"
        "make reshape(rows: int[][4]) -> byte[2][] { return rows[0][0] }\n"
        "make main() { let values: bool[3][5] = [] }\n"};
    const vex::ParseResult parsed = parse(source);
    tests.check(!parsed.has_errors(),
                "type annotations consume every consecutive array dimension");
    if (parsed.has_errors() || parsed.program.statements.size() < 3u) return;

    const auto* structure =
        std::get_if<vex::StructDeclStmt>(&parsed.program.statements[0]->data);
    tests.check(structure && structure->fields.size() == 1u &&
                structure->fields[0].type_annotation &&
                source.slice(*structure->fields[0].type_annotation) == "float[2][3]",
                "structure field annotations preserve both fixed dimensions");

    const auto* function =
        std::get_if<vex::FunctionDeclStmt>(&parsed.program.statements[1]->data);
    tests.check(function && function->parameters.size() == 1u &&
                function->parameters[0].type_annotation &&
                source.slice(*function->parameters[0].type_annotation) == "int[][4]",
                "parameter annotations preserve mixed dynamic and fixed dimensions");
    tests.check(function && function->return_type &&
                source.slice(*function->return_type) == "byte[2][]",
                "return annotations preserve fixed and dynamic dimensions");

    const auto* main_function =
        std::get_if<vex::FunctionDeclStmt>(&parsed.program.statements[2]->data);
    const auto* body = main_function && main_function->body
        ? std::get_if<vex::BlockStmt>(&main_function->body->data) : nullptr;
    const auto* variable = body && !body->statements.empty()
        ? std::get_if<vex::VariableDeclStmt>(&body->statements[0]->data) : nullptr;
    tests.check(variable && variable->type_annotation &&
                source.slice(*variable->type_annotation) == "bool[3][5]",
                "local variable annotations preserve repeated fixed dimensions");
}

void test_typed_declarations_may_omit_initializer(TestContext& tests) {
    const vex::SourceFile source{"default_initialized_declaration.vex",
        "make main() {\n"
        "    let count: int\n"
        "    let title: string\n"
        "}\n"};
    const vex::ParseResult parsed = parse(source);
    tests.check(!parsed.has_errors(), "typed mutable declarations may omit an initializer");
    if (parsed.has_errors() || parsed.program.statements.empty()) return;
    const auto* function =
        std::get_if<vex::FunctionDeclStmt>(&parsed.program.statements.front()->data);
    const auto* body = function && function->body
        ? std::get_if<vex::BlockStmt>(&function->body->data) : nullptr;
    tests.check(body && body->statements.size() == 2u,
                "uninitialized declarations remain ordinary statement nodes");
    if (!body || body->statements.size() != 2u) return;
    const auto* first = std::get_if<vex::VariableDeclStmt>(&body->statements[0]->data);
    const auto* second = std::get_if<vex::VariableDeclStmt>(&body->statements[1]->data);
    tests.check(first && first->type_annotation && !first->initializer,
                "uninitialized typed binding preserves its annotation and absent initializer");
    tests.check(second && second->type_annotation && !second->initializer,
                "string declarations can also omit their initializer");
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
    test_bitwise_precedence(tests);
    test_functions_and_calls(tests);
    test_unary_expression_in_call_argument(tests);
    test_multiline_expression(tests);
    test_control_flow_and_assignment(tests);
    test_array_literal_indexing_and_assignment(tests);
    test_integer_range_expressions(tests);
    test_structure_declarations_literals_and_members(tests);
    test_multiline_structure_literal_with_array_fields(tests);
    test_multidimensional_type_annotation_spans(tests);
    test_typed_declarations_may_omit_initializer(tests);
    test_if_without_else_preserves_next_statement(tests);
    test_invalid_syntax_and_newline_rules(tests);
}
