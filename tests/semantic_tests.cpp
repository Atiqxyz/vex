#include "test_support.hpp"

#include "vex/lexer.hpp"
#include "vex/parser.hpp"
#include "vex/semantic.hpp"

#include <string_view>

namespace {

vex::SemanticResult analyze(const vex::SourceFile& source) {
    const vex::LexResult lexed = vex::Lexer(source).scan();
    if (lexed.has_errors()) return vex::SemanticResult{lexed.diagnostics};
    const vex::ParseResult parsed = vex::Parser(source, lexed.tokens).parse();
    if (parsed.has_errors()) return vex::SemanticResult{parsed.diagnostics};
    return vex::SemanticAnalyzer(source, parsed.program).analyze();
}

bool has_code(const vex::SemanticResult& result, const std::string_view code) {
    for (const vex::Diagnostic& diagnostic : result.diagnostics) {
        if (diagnostic.code == code) return true;
    }
    return false;
}

void test_valid_program_and_forward_calls(TestContext& tests) {
    const vex::SourceFile source{"valid_semantics.vex",
        "make main() {\n"
        "    let total = add(2, 3)\n"
        "    let widened: float = total\n"
        "    if widened > 4.0 {\n"
        "        say(widened)\n"
        "    }\n"
        "}\n"
        "make add(left: int, right: int) -> int {\n"
        "    return left + right\n"
        "}\n"};
    const vex::SemanticResult result = analyze(source);
    tests.check(!result.has_errors(),
                "typed functions, forward calls, inference, widening, and say pass");
}

void test_scopes_shadowing_and_duplicates(TestContext& tests) {
    const vex::SourceFile source{"scopes.vex",
        "make main() {\n"
        "    let value = 1\n"
        "    {\n"
        "        let value = \"inner\"\n"
        "        say(value)\n"
        "    }\n"
        "    value = value + 1\n"
        "}\n"};
    const vex::SemanticResult valid = analyze(source);
    tests.check(!valid.has_errors(), "nested lexical shadowing is valid");

    const vex::SourceFile duplicate{"duplicate.vex",
        "make main() {\n"
        "    let value = 1\n"
        "    let value = 2\n"
        "}\n"};
    const vex::SemanticResult invalid = analyze(duplicate);
    tests.check(has_code(invalid, "SEM002"),
                "duplicate declarations in one scope are diagnosed");
}

void test_name_resolution_and_mutability(TestContext& tests) {
    const vex::SourceFile source{"names.vex",
        "make main() {\n"
        "    const fixed = 1\n"
        "    fixed = 2\n"
        "    let other = missing\n"
        "}\n"};
    const vex::SemanticResult result = analyze(source);
    tests.check(has_code(result, "SEM001"), "undefined names are diagnosed");
    tests.check(has_code(result, "SEM017"), "assignment to const is diagnosed");
}

void test_operator_and_condition_types(TestContext& tests) {
    const vex::SourceFile source{"operator_types.vex",
        "make main() {\n"
        "    let invalid = \"text\" - 2\n"
        "    if 1 {\n"
        "        say(invalid)\n"
        "    }\n"
        "    repeat true times {\n"
        "        say(1)\n"
        "    }\n"
        "}\n"};
    const vex::SemanticResult result = analyze(source);
    tests.check(has_code(result, "SEM012"),
                "arithmetic with non-numeric operands is rejected");
    tests.check(has_code(result, "SEM029"), "non-boolean if conditions are rejected");
    tests.check(has_code(result, "SEM030"), "non-integer repeat counts are rejected");
}

void test_function_signatures_and_returns(TestContext& tests) {
    const vex::SourceFile source{"signatures.vex",
        "make sum(left: int, right: int) -> int {\n"
        "    return left + right\n"
        "}\n"
        "make main() {\n"
        "    let wrong = sum(\"one\", 2)\n"
        "}\n"};
    const vex::SemanticResult result = analyze(source);
    tests.check(has_code(result, "SEM021"),
                "function argument types are checked against parameter types");

    const vex::SourceFile missing_return{"missing_return.vex",
        "make get_value(flag: bool) -> int {\n"
        "    if flag {\n"
        "        return 1\n"
        "    }\n"
        "}\n"};
    const vex::SemanticResult incomplete = analyze(missing_return);
    tests.check(has_code(incomplete, "SEM024"),
                "non-void functions must return on every control-flow path");
}


void test_semantic_model_exposes_typed_resolutions(TestContext& tests) {
    const vex::SourceFile source{"semantic_model.vex",
        "make add(left: int, right: int) -> int {\n"
        "    return left + right\n"
        "}\n"
        "make main() {\n"
        "    let total = add(2, 3)\n"
        "    total = total + 1\n"
        "}\n"};
    const vex::LexResult lexed = vex::Lexer(source).scan();
    const vex::ParseResult parsed = vex::Parser(source, lexed.tokens).parse();
    const vex::SemanticResult result =
        vex::SemanticAnalyzer(source, parsed.program).analyze();

    tests.check(!result.has_errors(), "semantic model test program is valid");
    tests.check(!result.model.expression_types.empty(),
                "semantic analysis records expression types");
    tests.check(!result.model.variable_bindings.empty(),
                "semantic analysis assigns stable binding identifiers");
    tests.check(!result.model.resolved_names.empty(),
                "semantic analysis records resolved variable references");
    tests.check(!result.model.resolved_calls.empty(),
                "semantic analysis records resolved function calls");
    tests.check(!result.model.functions.empty(),
                "semantic analysis exposes typed function signatures");

    if (parsed.program.statements.size() > 1) {
        const auto* main_function =
            std::get_if<vex::FunctionDeclStmt>(&parsed.program.statements[1]->data);
        if (main_function && main_function->body) {
            const auto* body = std::get_if<vex::BlockStmt>(&main_function->body->data);
            if (body && !body->statements.empty()) {
                const auto* variable =
                    std::get_if<vex::VariableDeclStmt>(&body->statements[0]->data);
                if (variable && variable->initializer) {
                    const auto* call =
                        std::get_if<vex::CallExpr>(&variable->initializer->data);
                    if (call) {
                        const auto type = result.model.expression_types.find(variable->initializer.get());
                        tests.check(type != result.model.expression_types.end() &&
                                    type->second == vex::SemanticType::Int,
                                    "function-call expression carries its inferred return type");
                        const auto target = result.model.resolved_calls.find(call);
                        tests.check(target != result.model.resolved_calls.end() &&
                                    target->second == "add",
                                    "call expression retains its resolved callee name");
                    }
                }
            }
        }
    }
}

void test_math_standard_library_signatures(TestContext& tests) {
    const vex::SourceFile source{"math_builtins.vex",
        "make main() {\n"
        "    let root = sqrt(144)\n"
        "    let wave = sin(0.0) + cos(0.0)\n"
        "    let scaled = pow(2.0, 3.0)\n"
        "    let magnitude = abs(-5.0)\n"
        "    say(root + wave + scaled + magnitude)\n"
        "}\n"};
    const vex::SemanticResult valid = analyze(source);
    tests.check(!valid.has_errors(),
                "math builtins accept numeric arguments and return float");

    const vex::SourceFile invalid{"bad_math_builtins.vex",
        "make main() {\n"
        "    let invalid = sqrt(true)\n"
        "}\n"};
    const vex::SemanticResult rejected = analyze(invalid);
    tests.check(has_code(rejected, "SEM021"),
                "math builtins reject arguments with incompatible types");
}

void test_unimplemented_iteration_is_explicit(TestContext& tests) {
    const vex::SourceFile source{"iteration.vex",
        "make main() {\n"
        "    let items = 1\n"
        "    for item in items {\n"
        "        say(item)\n"
        "    }\n"
        "}\n"};
    const vex::SemanticResult result = analyze(source);
    tests.check(!has_code(result, "SEM001"),
                "the declared iteration source resolves by name");
    tests.check(has_code(result, "SEM031"),
                "for-in type checking is deferred until collection types exist");
}

} // namespace

void run_semantic_tests(TestContext& tests) {
    test_valid_program_and_forward_calls(tests);
    test_scopes_shadowing_and_duplicates(tests);
    test_name_resolution_and_mutability(tests);
    test_operator_and_condition_types(tests);
    test_function_signatures_and_returns(tests);
    test_semantic_model_exposes_typed_resolutions(tests);
    test_math_standard_library_signatures(tests);
    test_unimplemented_iteration_is_explicit(tests);
}
