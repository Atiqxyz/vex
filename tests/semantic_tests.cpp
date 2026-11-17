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
        "    let compound_text = \"text\"\n"
        "    compound_text -= \"more\"\n"
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

    const vex::SourceFile invalid_remainder{"invalid_remainder.vex",
        "make main() { let value = 7.5 % 2 }\n"};
    const vex::SemanticResult remainder_result = analyze(invalid_remainder);
    tests.check(has_code(remainder_result, "SEM012"),
                "remainder rejects non-integer operands");
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



void test_control_flow_return_analysis(TestContext& tests) {
    const vex::SourceFile valid{"complete_return_paths.vex",
        "make nested(flag: bool, other: bool) -> int {\n"
        "    if flag {\n"
        "        if other { return 1 } else { return 2 }\n"
        "    } else {\n"
        "        return 3\n"
        "    }\n"
        "}\n"
        "make early(flag: bool) -> int {\n"
        "    if flag { return 4 }\n"
        "    return 5\n"
        "}\n"
        "make constant_branch() -> int {\n"
        "    if true { return 6 }\n"
        "}\n"
        "make while_returns() -> int {\n"
        "    while true { return 7 }\n"
        "}\n"
        "make negated_while_returns() -> int {\n"
        "    while !false { return 8 }\n"
        "}\n"
        "make diverges() -> int {\n"
        "    while true { say(8) }\n"
        "}\n"
        "make repeat_returns() -> int {\n"
        "    repeat 2 times { return 9 }\n"
        "}\n"
        "make array_returns() -> int {\n"
        "    let values = [10,]\n"
        "    for value in values { return value }\n"
        "}\n"
        "make range_returns() -> int {\n"
        "    for value in 1..=1 { return value }\n"
        "}\n"
        "make nested_loop_diverges() -> int {\n"
        "    while true {\n"
        "        while true { break }\n"
        "    }\n"
        "}\n"};
    const vex::SemanticResult accepted = analyze(valid);
    tests.check(!accepted.has_errors(),
                "nested branches, early returns, literal conditions, proven non-empty loops, and non-fallthrough loops satisfy return analysis");

    const vex::SourceFile invalid{"incomplete_return_paths.vex",
        "make conditional(flag: bool) -> int {\n"
        "    if flag { return 1 } else { say(2) }\n"
        "}\n"
        "make conditional_after_break(flag: bool) -> int {\n"
        "    while true {\n"
        "        if flag { return 3 } else { break }\n"
        "    }\n"
        "}\n"
        "make maybe_zero_iterations(flag: bool) -> int {\n"
        "    while flag { return 4 }\n"
        "}\n"
        "make zero_repeat() -> int {\n"
        "    repeat 0 times { return 5 }\n"
        "}\n"
        "make negative_repeat() -> int {\n"
        "    repeat -2 times { return 6 }\n"
        "}\n"
        "make empty_range() -> int {\n"
        "    for value in 2..2 { return value }\n"
        "}\n"
        "make constant_false() -> int {\n"
        "    if false { return 7 }\n"
        "}\n"
        "make repeat_continue_falls_through() -> int {\n"
        "    repeat 2 times { continue }\n"
        "}\n"
        "make array_continue_falls_through() -> int {\n"
        "    for value in [1,] { continue }\n"
        "}\n"
        "make range_continue_falls_through() -> int {\n"
        "    for value in 1..=1 { continue }\n"
        "}\n"};
    const vex::SemanticResult rejected = analyze(invalid);
    std::size_t return_path_errors = 0;
    for (const vex::Diagnostic& diagnostic : rejected.diagnostics) {
        if (diagnostic.code == "SEM024") ++return_path_errors;
    }
    tests.check(return_path_errors == 10,
                "every incomplete function is rejected when a reachable path can reach function end");
}

void test_recursive_function_signatures(TestContext& tests) {
    const vex::SourceFile source{"recursive_functions.vex",
        "make factorial(n: int) -> int {\n"
        "    if n <= 1 { return 1 }\n"
        "    return n * factorial(n - 1)\n"
        "}\n"
        "make is_even(n: int) -> bool {\n"
        "    if n == 0 { return true }\n"
        "    return is_odd(n - 1)\n"
        "}\n"
        "make is_odd(n: int) -> bool {\n"
        "    if n == 0 { return false }\n"
        "    return is_even(n - 1)\n"
        "}\n"
        "make main() {\n"
        "    say(factorial(5))\n"
        "    say(is_even(8))\n"
        "}\n"};
    const vex::SemanticResult result = analyze(source);
    tests.check(!result.has_errors(),
                "direct and mutual recursion resolve against pre-collected function signatures");
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

void test_loop_control_scope_and_while_condition(TestContext& tests) {
    const vex::SourceFile valid{"loop_control.vex",
        "make main() {\n"
        "    let count = 0\n"
        "    while count < 5 {\n"
        "        count += 1\n"
        "        if count == 2 { continue }\n"
        "        if count == 4 { break }\n"
        "    }\n"
        "    repeat 2 times {\n"
        "        count += 1\n"
        "        continue\n"
        "    }\n"
        "    repeat 2 times {\n"
        "        for index in 1..=4 {\n"
        "            if index == 2 { continue }\n"
        "            if index == 4 { break }\n"
        "            count += 1\n"
        "        }\n"
        "    }\n"
        "}\n"};
    const vex::SemanticResult accepted = analyze(valid);
    tests.check(!accepted.has_errors(),
                "while, break, continue, and compound assignment pass semantic analysis");

    const vex::SourceFile outside{"loop_control_outside.vex",
        "make main() {\n"
        "    break\n"
        "    continue\n"
        "}\n"};
    const vex::SemanticResult rejected = analyze(outside);
    tests.check(has_code(rejected, "SEM032"), "break outside a loop is diagnosed");
    tests.check(has_code(rejected, "SEM033"), "continue outside a loop is diagnosed");

    const vex::SourceFile bad_condition{"bad_while.vex",
        "make main() { while 1 { break } }\n"};
    const vex::SemanticResult bad = analyze(bad_condition);
    tests.check(has_code(bad, "SEM034"), "while requires a boolean condition");
}

void test_top_level_execution_scope(TestContext& tests) {
    const vex::SourceFile script{"top_level.vex",
        "let mass = 1200.0\n"
        "let acceleration = 9.81\n"
        "let force = mass * acceleration\n"
        "say(force)\n"};
    const vex::SemanticResult accepted = analyze(script);
    tests.check(!accepted.has_errors(),
                "top-level executable statements can reference earlier top-level bindings");

    const vex::SourceFile captured{"top_level_capture.vex",
        "let gravity = 9.81\n"
        "make calculate(mass: float) -> float {\n"
        "    return mass * gravity\n"
        "}\n"};
    const vex::SemanticResult rejected = analyze(captured);
    tests.check(has_code(rejected, "SEM001"),
                "ordinary functions cannot capture implicit-entry top-level variables");
}


void test_fixed_arrays_and_index_assignment(TestContext& tests) {
    const vex::SourceFile valid{"fixed_arrays.vex",
        "make main() {\n"
        "    let values = [1, 2, 3]\n"
        "    values[1] = 9\n"
        "    values[2] += 1\n"
        "    let mixed = [1, 2.5, 3]\n"
        "    say(values[1])\n"
        "    for value in values { say(value) }\n"
        "}\n"};
    const vex::SemanticResult accepted = analyze(valid);
    tests.check(!accepted.has_errors(),
                "fixed arrays infer primitive element types and support indexed mutation and iteration");

    const vex::SourceFile bad_index{"bad_array_index.vex",
        "make main() {\n"
        "    let values = [1, 2]\n"
        "    let bad = values[1.5]\n"
        "}\n"};
    const vex::SemanticResult bad = analyze(bad_index);
    tests.check(has_code(bad, "SEM042"), "array indices must have type int");

    const vex::SourceFile mixed{"mixed_array.vex",
        "make main() { let values = [1, true] }\n"};
    const vex::SemanticResult mixed_result = analyze(mixed);
    tests.check(has_code(mixed_result, "SEM041"),
                "arrays reject incompatible element types");

    const vex::SourceFile immutable{"immutable_array.vex",
        "make main() {\n"
        "    const values = [1, 2]\n"
        "    values[0] = 7\n"
        "}\n"};
    const vex::SemanticResult immutable_result = analyze(immutable);
    tests.check(has_code(immutable_result, "SEM017"),
                "elements of const arrays cannot be modified");

    const vex::SourceFile mismatch{"array_shape_mismatch.vex",
        "make main() {\n"
        "    let values = [1, 2]\n"
        "    values = [3, 4, 5]\n"
        "}\n"};
    const vex::SemanticResult mismatch_result = analyze(mismatch);
    tests.check(has_code(mismatch_result, "SEM045"),
                "array reassignment checks fixed length and element type");
}

void test_array_function_types_and_shapes(TestContext& tests) {
    const vex::SourceFile valid{"array_function_types.vex",
        "make identity(values: int[3]) -> int[3] { return values }\n"
        "make first(values: int[3]) -> int { return values[0] }\n"
        "make main() {\n"
        "    let values: int[3] = [1, 2, 3]\n"
        "    let copied: int[3] = identity(values)\n"
        "    say(first(copied))\n"
        "}\n"};
    const vex::SemanticResult accepted = analyze(valid);
    tests.check(!accepted.has_errors(),
                "fixed-array annotations, function parameters, and array returns are accepted");

    const vex::SourceFile invalid{"array_function_shape_errors.vex",
        "make expect_two(values: int[2]) -> int { return values[0] }\n"
        "make expect_float(values: float[2]) -> float { return values[0] }\n"
        "make wrong_return() -> int[2] { return [1, 2, 3] }\n"
        "make wrong_element_return() -> int[2] { return [1.0, 2.0] }\n"
        "make invalid_length(values: int[0]) -> int { return 0 }\n"
        "make oversized_length(values: bool[65537]) -> int { return 0 }\n"
        "make main() {\n"
        "    let wrong_binding: int[2] = [1, 2, 3]\n"
        "    let wrong_element_binding: int[2] = [1.0, 2.0]\n"
        "    let values = [1, 2, 3]\n"
        "    let integer_values = [4, 5]\n"
        "    expect_two(values)\n"
        "    expect_float(integer_values)\n"
        "}\n"};
    const vex::SemanticResult rejected = analyze(invalid);
    tests.check(has_code(rejected, "SEM021"),
                "array arguments must match the parameter's element type and length");
    tests.check(has_code(rejected, "SEM028"),
                "array returns must match the function's declared element type and length");
    tests.check(has_code(rejected, "SEM045"),
                "annotated array initializers must match their declared shape");
    tests.check(has_code(rejected, "SEM041"),
                "fixed-array annotations reject zero and over-limit lengths");
}

void test_nested_array_annotations_and_semantics(TestContext& tests) {
    const vex::SourceFile valid{"nested_array_semantics.vex",
        "make first(rows: int[][]) -> int[] { return rows[0] }\n"
        "make identity(rows: int[][3]) -> int[][3] { return rows }\n"
        "make main() {\n"
        "    let rows: int[][] = [[1, 2], [3, 4, 5]]\n"
        "    let fixed_rows: int[][3] = [[1, 2, 3], [4, 5, 6]]\n"
        "    let dynamic_columns: int[2][] = [[], [7, 8]]\n"
        "    push(rows, [9, 10])\n"
        "    push(rows[0], 11)\n"
        "    let removed = pop(rows)\n"
        "    let first_row = first(rows)\n"
        "    for row in rows {\n"
        "        for value in row { say(value) }\n"
        "    }\n"
        "    let count = length(rows[0])\n"
        "}\n"};
    const vex::SemanticResult accepted = analyze(valid);
    tests.check(!accepted.has_errors(),
                "dynamic, jagged, mixed fixed/dynamic nested arrays support literals, indexing, built-ins, functions, and nested iteration");

    const vex::SourceFile deep_jagged{"deep_jagged_nested_arrays.vex",
        "make main() {\n"
        "    let cubes: int[][][] = [[[1, 2], [3,]], [[4,],]]\n"
        "}\n"};
    const vex::SemanticResult deep_accepted = analyze(deep_jagged);
    tests.check(!deep_accepted.has_errors(),
                "contextual dynamic dimensions permit different lengths at deeper jagged levels");

    const vex::SourceFile fixed_nested{"fixed_nested_array_semantics.vex",
        "struct Matrix { cells: int[2][3] }\n"
        "make identity(values: int[2][3]) -> int[2][3] { return values }\n"
        "make main() {\n"
        "    let matrix: int[2][3] = [[1, 2, 3], [4, 5, 6]]\n"
        "    let copied = identity(matrix)\n"
        "    let inferred = [[7, 8], [9, 10]]\n"
        "    let box = Matrix { cells: copied }\n"
        "    box.cells[1][2] = inferred[1][0]\n"
        "    let dynamic_outer: int[][2][3] = [[[1, 2, 3], [4, 5, 6]],]\n"
        "}\n"};
    const vex::SemanticResult fixed_accepted = analyze(fixed_nested);
    tests.check(!fixed_accepted.has_errors(),
                "all-fixed nested arrays support contextual and inferred literals, structure fields, indexing, and function values");

    const vex::SourceFile oversized{"oversized_fixed_nested_array.vex",
        "make too_large(value: byte[513][512]) {}\n"
        "make too_large_dynamic(value: byte[][513][512]) {}\n"
        "make main() {}\n"};
    const vex::SemanticResult rejected = analyze(oversized);
    tests.check(has_code(rejected, "SEM060"),
                "nested fixed dimension runs whose minimum element storage exceeds the object budget are rejected");
}

void test_default_initialization_semantics(TestContext& tests) {
    const vex::SourceFile valid{"default_initialization.vex",
        "make main() {\n"
        "    let count: int\n"
        "    let ratio: float\n"
        "    let enabled: bool\n"
        "    let marker: byte\n"
        "    let title: string\n"
        "    say(count)\n"
        "    say(ratio)\n"
        "    say(enabled)\n"
        "    say(marker)\n"
        "    say(title)\n"
        "}\n"};
    const vex::SemanticResult accepted = analyze(valid);
    tests.check(!accepted.has_errors(),
                "explicitly typed mutable scalar bindings may use deterministic defaults");

    const vex::SourceFile rejected{"unsupported_default_initialization.vex",
        "struct Point { x: int }\n"
        "make main() {\n"
        "    let values: int[]\n"
        "    let point: Point\n"
        "    const fixed: int\n"
        "    let unknown\n"
        "}\n"};
    const vex::SemanticResult invalid = analyze(rejected);
    tests.check(has_code(invalid, "SEM057"), "arrays and structures still require explicit initialization");
    tests.check(has_code(invalid, "SEM058"), "immutable const declarations require explicit initialization");
    tests.check(has_code(invalid, "SEM059"), "uninitialized declarations require an explicit type annotation");
}

void test_dynamic_array_semantics(TestContext& tests) {
    const vex::SourceFile valid{"dynamic_arrays.vex",
        "make empty_values() -> int[] { return [] }\n"
        "make initial_values() -> int[] { return [4, 5] }\n"
        "make append(values: int[], value: int) -> int[] {\n"
        "    push(values, value)\n"
        "    return values\n"
        "}\n"
        "make sum_values(values: int[]) -> int {\n"
        "    let total = 0\n"
        "    for value in values { total += value }\n"
        "    return total\n"
        "}\n"
        "make main() {\n"
        "    let values: int[] = []\n"
        "    push(values, 1)\n"
        "    let shared: int[] = append(values, 2)\n"
        "    let direct = append([3, 4], 5)\n"
        "    values = [6, 7]\n"
        "    let last = pop(shared)\n"
        "    let count = length(values)\n"
        "    say(sum_values(values))\n"
        "    let decimals: float[] = [1.5, 2.5]\n"
        "    push(decimals, 3)\n"
        "    let labels: string[] = [\"VEX\",]\n"
        "    push(labels, \"runtime\")\n"
        "    let flags: bool[] = [true,]\n"
        "    push(flags, false)\n"
        "    let empty: int[] = empty_values()\n"
        "    let populated: int[] = initial_values()\n"
        "}\n"};
    const vex::SemanticResult accepted = analyze(valid);
    tests.check(!accepted.has_errors(),
                "dynamic arrays support typed empty literals, mutation, built-ins, iteration, aliases, and function signatures");

    const vex::SourceFile invalid{"dynamic_array_errors.vex",
        "make main() {\n"
        "    let fixed = [1, 2]\n"
        "    push(fixed, 3)\n"
        "    let values: int[] = []\n"
        "    push(values, \"wrong\")\n"
        "    const immutable: int[] = []\n"
        "    pop(immutable)\n"
        "    let untyped_empty = []\n"
        "    pop(fixed)\n"
        "}\n"};
    const vex::SemanticResult rejected = analyze(invalid);
    tests.check(has_code(rejected, "SEM021"),
                "push and pop reject fixed arrays and push rejects the wrong element type");
    tests.check(has_code(rejected, "SEM017"),
                "dynamic-array mutation rejects immutable bindings");
    tests.check(has_code(rejected, "SEM041"),
                "empty arrays require an explicit dynamic-array element type");
}

void test_dynamic_array_capacity_semantics(TestContext& tests) {
    const vex::SourceFile valid{"dynamic_array_capacity_semantics.vex",
        "make main() {\n"
        "    let values: int[] = []\n"
        "    let initial = capacity(values)\n"
        "    reserve(values, 16)\n"
        "    let enough = capacity(values) >= 16\n"
        "    let count = length(values)\n"
        "    const locked: int[] = []\n"
        "    let locked_capacity = capacity(locked)\n"
        "}\n"};
    const vex::SemanticResult accepted = analyze(valid);
    tests.check(!accepted.has_errors(),
                "capacity reads immutable dynamic arrays and reserve accepts a mutable dynamic array with an int minimum");

    const vex::SourceFile invalid{"invalid_dynamic_array_capacity.vex",
        "make main() {\n"
        "    let fixed = [1, 2]\n"
        "    capacity(fixed)\n"
        "    reserve(fixed, 4)\n"
        "    let values: int[] = []\n"
        "    reserve(values, 2.5)\n"
        "    const locked: int[] = []\n"
        "    reserve(locked, 8)\n"
        "    capacity(1)\n"
        "    reserve(values)\n"
        "}\n"};
    const vex::SemanticResult rejected = analyze(invalid);
    tests.check(has_code(rejected, "SEM021"),
                "capacity and reserve reject fixed arrays, non-arrays, and non-integer capacities");
    tests.check(has_code(rejected, "SEM017"),
                "reserve cannot mutate a dynamic array through an immutable binding");
    tests.check(has_code(rejected, "SEM020"),
                "reserve enforces its two-argument signature");
}

void test_integer_range_semantics(TestContext& tests) {
    const vex::SourceFile valid{"range_semantics.vex",
        "make main() {\n"
        "    let sum = 0\n"
        "    for index in 1 + 1..=5 - 1 {\n"
        "        sum += index\n"
        "    }\n"
        "    say(sum)\n"
        "}\n"};
    const vex::SemanticResult accepted = analyze(valid);
    tests.check(!accepted.has_errors(), "integer ranges accept integer bounds and infer an int iteration variable");
    const vex::SourceFile bad_bound{"range_float_bound.vex", "make main() { for index in 1.5..5 { say(index) } }\n"};
    const vex::SemanticResult bad = analyze(bad_bound);
    tests.check(has_code(bad, "SEM047"), "range bounds must be integers");
    const vex::SourceFile stored{"stored_range.vex", "make main() { let values = 0..5 }\n"};
    const vex::SemanticResult stored_result = analyze(stored);
    tests.check(has_code(stored_result, "SEM046"), "range expressions are only valid directly in a for-in loop");
}


void test_structure_types_value_semantics_and_diagnostics(TestContext& tests) {
    const vex::SourceFile valid{"structure_semantics.vex",
        "struct Point { x: int, y: int }\n"
        "struct Box { point: Point, width: int }\n"
        "make move(point: Point) -> Point {\n"
        "    let copied = point\n"
        "    copied.x = copied.x + 1\n"
        "    return copied\n"
        "}\n"
        "make main() {\n"
        "    let point = Point { x: 1, y: 2 }\n"
        "    let box = Box { point: point, width: 8 }\n"
        "    box.point.x = 9\n"
        "    let moved = move(point)\n"
        "    say(point.x)\n"
        "    say(moved.x)\n"
        "}\n"};
    const vex::SemanticResult valid_result = analyze(valid);
    tests.check(!valid_result.has_errors(),
                "nested structures, field mutation, and structure parameters/returns type-check");

    const vex::SourceFile composed{"composed_struct_arrays.vex",
        "struct Point { x: int, y: int }\n"
        "struct Group { fixed: Point[2], items: Point[] }\n"
        "struct Node { value: int, children: Node[] }\n"
        "make main() {\n"
        "    let points: Point[2] = [Point { x: 1, y: 2 }, Point { x: 3, y: 4 }]\n"
        "    let items: Point[] = []\n"
        "    push(items, points[0])\n"
        "    let group = Group { fixed: [Point { x: 1, y: 2 }, Point { x: 3, y: 4 }], items: [] }\n"
        "    group.fixed[1].x = 8\n"
        "    group.items[0].y = 9\n"
        "    let node = Node { value: 1, children: [] }\n"
        "    push(node.children, node)\n"
        "}\n"};
    const vex::SemanticResult composed_result = analyze(composed);
    tests.check(!composed_result.has_errors(),
                "fixed and dynamic structure arrays compose with records, nested indexing, and recursive pointer-backed collections");

    const vex::SourceFile invalid{"invalid_structures.vex",
        "struct Point { x: int, y: int }\n"
        "struct Other { x: int, y: int }\n"
        "struct Group { fixed: Point[2], items: Point[] }\n"
        "struct RecursiveValue { next: RecursiveValue }\n"
        "struct RecursiveFixed { values: RecursiveFixed[2] }\n"
        "struct ForwardList { values: Later[] }\n"
        "struct Later { value: int }\n"
        "make main() {\n"
        "    const fixed = Point { x: 1, y: 2 }\n"
        "    fixed.x = 3\n"
        "    let missing = Point { x: 1 }\n"
        "    let wrong = Point { x: true, y: 2 }\n"
        "    let incompatible: Point = Other { x: 1, y: 2 }\n"
        "    let unknown = Point { x: 1, y: 2, z: 3 }\n"
        "    let comparison = fixed == fixed\n"
        "    let unknown_array: Missing[] = []\n"
        "    let wrong_array: Point[] = [Other { x: 1, y: 2 }]\n"
        "    let points: Point[] = []\n"
        "    let others: Other[] = []\n"
        "    points = others\n"
        "    push(points, Other { x: 3, y: 4 })\n"
        "    say(fixed)\n"
        "}\n"};
    const vex::SemanticResult invalid_result = analyze(invalid);
    tests.check(has_code(invalid_result, "SEM017"),
                "immutable structure fields cannot be changed");

    const vex::SourceFile immutable_dynamic_field{"immutable_dynamic_field.vex",
        "struct Point { x: int }\n"
        "struct Group { items: Point[] }\n"
        "make main() {\n"
        "    const locked = Group { items: [] }\n"
        "    push(locked.items, Point { x: 1 })\n"
        "}\n"};
    const vex::SemanticResult immutable_dynamic_result =
        analyze(immutable_dynamic_field);
    tests.check(has_code(immutable_dynamic_result, "SEM017"),
                "dynamic arrays cannot be mutated through immutable structure bindings");
    tests.check(has_code(invalid_result, "SEM053"),
                "every structure field must be initialized");
    tests.check(has_code(invalid_result, "SEM052"),
                "structure field initializers are type-checked");
    tests.check(has_code(invalid_result, "SEM022"),
                "different structure types are not interchangeable");
    tests.check(has_code(invalid_result, "SEM051"),
                "unknown structure fields are rejected");
    tests.check(has_code(invalid_result, "SEM014"),
                "structure equality is rejected until defined explicitly");
    tests.check(has_code(invalid_result, "SEM041"),
                "array literals reject a different nominal structure element type");
    tests.check(has_code(invalid_result, "SEM045"),
                "array assignment rejects a different nominal structure element type");
    tests.check(has_code(invalid_result, "SEM021"),
                "push rejects a different nominal structure element type");
    tests.check(has_code(invalid_result, "SEM050"),
                "direct recursion, fixed-array recursion, and forward structure-array fields are rejected");
    tests.check(has_code(invalid_result, "SEM017"),
                "dynamic arrays cannot be mutated through immutable structure bindings");
    tests.check(has_code(invalid_result, "SEM056"),
                "structure values cannot be printed without selecting a field");
    tests.check(has_code(invalid_result, "SEM003"),
                "unknown structure names are rejected in array element annotations");
}

void test_enum_and_structure_type_composition(TestContext& tests) {
    const vex::SourceFile source{"enum_structure_composition.vex",
        "struct Packet { outcome: Outcome }\n"
        "enum Outcome { Success(int), Failure(string) }\n"
        "make main() {\n"
        "    let packet = Packet{outcome: Outcome.Success(5)}\n"
        "    match packet.outcome {\n"
        "        Outcome.Success(value) => { say(value) }\n"
        "        Outcome.Failure(reason) => { say(reason) }\n"
        "    }\n"
        "}\n"};
    const vex::SemanticResult result = analyze(source);
    tests.check(!result.has_errors(),
                "structures can contain enums declared later and expose them to exhaustive matching");
    tests.check(result.model.struct_types.size() == 1u &&
                result.model.struct_types.front().fields.size() == 1u &&
                result.model.struct_types.front().fields.front().type == vex::SemanticType::Enum &&
                result.model.struct_types.front().fields.front().enum_type_id == 1u,
                "structure fields preserve enum nominal identity");
}

void test_enum_match_semantics(TestContext& tests) {
    const vex::SourceFile valid{"valid_enum_match.vex",
        "enum Result { Success(int), Failure(string), Cancelled }\n"
        "make inspect(result: Result) {\n"
        "    match result {\n"
        "        Result.Success(value) => { say(value) }\n"
        "        Result.Failure(reason) => { say(reason) }\n"
        "        Result.Cancelled => { say(0) }\n"
        "    }\n"
        "}\n"};
    const vex::SemanticResult valid_result = analyze(valid);
    tests.check(!valid_result.has_errors() &&
                valid_result.model.match_statements.size() == 1u,
                "exhaustive enum matches resolve all variants and payload bindings");
    bool found_payload_binding = false;
    for (const auto& entry : valid_result.model.bindings) {
        if (entry.second.name == "value" && entry.second.type == vex::SemanticType::Int &&
            !entry.second.mutable_binding) found_payload_binding = true;
    }
    tests.check(found_payload_binding,
                "match payload bindings are immutable and retain their declared type");

    const vex::SourceFile invalid{"invalid_enum_match.vex",
        "enum Result { Success(int), Failure(string), Cancelled }\n"
        "make inspect(result: Result) {\n"
        "    match result {\n"
        "        Result.Success(value) => { say(value) }\n"
        "        Result.Success(other) => { say(other) }\n"
        "        Result.Cancelled => { say(0) }\n"
        "    }\n"
        "}\n"};
    const vex::SemanticResult invalid_result = analyze(invalid);
    tests.check(has_code(invalid_result, "SEM075"),
                "duplicate enum match variants are rejected");
    tests.check(has_code(invalid_result, "SEM077"),
                "non-exhaustive enum matches are rejected");

    const vex::SourceFile wrong_bindings{"wrong_match_bindings.vex",
        "enum Result { Success(int), Failure(string) }\n"
        "make inspect(result: Result) {\n"
        "    match result {\n"
        "        Result.Success => { say(0) }\n"
        "        Result.Failure(reason, extra) => { say(reason) }\n"
        "    }\n"
        "}\n"};
    tests.check(has_code(analyze(wrong_bindings), "SEM076"),
                "match patterns must bind exactly the payload fields declared by a variant");
}

void test_enum_declaration_semantics(TestContext& tests) {
    const vex::SourceFile valid{"valid_enum_declarations.vex",
        "struct Point { x: int, y: float }\n"
        "enum SolverResult { Converged(int, float), Failed(string), AtPoint(Point), Grid(Point[2]) }\n"
        "enum Direction { North, South, East, West }\n"};
    const vex::SemanticResult valid_result = analyze(valid);
    tests.check(!valid_result.has_errors() && valid_result.model.enum_types.size() == 2u,
                "enum declarations accept unit, primitive, structure and fixed-array payloads");
    if (valid_result.model.enum_types.size() == 2u) {
        tests.check(valid_result.model.enum_types[0].variants.size() == 4u &&
                    valid_result.model.enum_types[0].variants[0].fields.size() == 2u &&
                    valid_result.model.enum_types[0].variants[2].fields[0].struct_type_id == 1u,
                    "semantic enum model preserves nominal payload types and variant order");
    }

    const vex::SourceFile invalid{"invalid_enum_declarations.vex",
        "enum Empty { }\n"
        "enum Repeated { Same, Same }\n"
        "enum InvalidPayload { Nope(void), Unknown(Missing) }\n"
        "enum void { Builtin }\n"};
    const vex::SemanticResult invalid_result = analyze(invalid);
    tests.check(has_code(invalid_result, "SEM062"), "empty enums are rejected");
    tests.check(has_code(invalid_result, "SEM063"), "duplicate variants are rejected");
    tests.check(has_code(invalid_result, "SEM064"), "invalid enum payload types are rejected");
    tests.check(has_code(invalid_result, "SEM061"), "enum names cannot shadow built-in types");

    const vex::SourceFile duplicate_global{"duplicate_enum_global.vex",
        "enum Event { Started }\nstruct Event { value: int }\n"};
    const vex::SemanticResult duplicate_result = analyze(duplicate_global);
    tests.check(has_code(duplicate_result, "SEM002"),
                "enum declarations cannot share nominal names with structures");
}

void test_for_in_rejects_non_array_values(TestContext& tests) {
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
                "for-in rejects values that are neither arrays nor integer ranges");
}

void test_string_byte_length_semantics(TestContext& tests) {
    const vex::SourceFile valid{"string_length.vex",
        "make main() {\n"
        "    let plain = length(\"hello\")\n"
        "    let embedded_nul = length(\"A\\0B\")\n"
        "    let empty = length(\"\")\n"
        "    let values = [1, 2, 3]\n"
        "    let array_count = length(values)\n"
        "    say(plain + embedded_nul + empty + array_count)\n"
        "}\n"};
    const vex::SemanticResult accepted = analyze(valid);
    tests.check(!accepted.has_errors(),
                "length accepts byte strings while preserving array length support");

    const vex::SourceFile invalid{"invalid_string_length.vex",
        "make main() { let count = length(42) }\n"};
    const vex::SemanticResult rejected = analyze(invalid);
    tests.check(has_code(rejected, "SEM021"),
                "length rejects values that are neither strings nor arrays");
}

void test_bitwise_operator_semantics(TestContext& tests) {
    const vex::SourceFile valid{"bitwise_valid.vex",
        "make main() {\n"
        "    let a = 12 & 10\n"
        "    let b = 12 | 10\n"
        "    let c = 12 ^ 10\n"
        "    let d = ~5\n"
        "    say(a + b + c + d)\n"
        "}\n"};
    const auto accepted = analyze(valid);
    tests.check(!accepted.has_errors(), "bitwise operators accept int operands");

    const vex::SourceFile invalid{"bitwise_invalid.vex",
        "make main() {\n"
        "    let a = true & false\n"
        "    let b = ~1.5\n"
        "}\n"};
    const auto rejected = analyze(invalid);
    tests.check(has_code(rejected, "SEM012"),
                "binary bitwise operators reject non-integer operands");
    tests.check(has_code(rejected, "SEM011"),
                "bitwise NOT rejects non-integer operands");
}

void test_byte_type_and_checked_conversions(TestContext& tests) {
    const vex::SourceFile valid{"byte_valid.vex",
        "struct Packet {\n"
        "    tag: byte,\n"
        "    payload: byte[]\n"
        "}\n"
        "make echo(value: byte) -> byte { return value }\n"
        "make main() {\n"
        "    let maximum = to_byte(255)\n"
        "    let minimum: byte = to_byte(0)\n"
        "    let values: byte[] = [maximum, minimum]\n"
        "    let fixed: byte[2] = [minimum, maximum]\n"
        "    let packet = Packet{tag: echo(maximum), payload: values}\n"
        "    packet.tag = to_byte(7)\n"
        "    say(to_int(packet.tag))\n"
        "    say(to_int(fixed[1]))\n"
        "    say(~minimum)\n"
        "    say(maximum > minimum)\n"
        "}\n"};
    const vex::SemanticResult accepted = analyze(valid);
    tests.check(!accepted.has_errors(),
                "byte works in function signatures, structures, and fixed/dynamic arrays");

    const vex::SourceFile invalid{"byte_invalid.vex",
        "make main() {\n"
        "    let implicit: byte = 255\n"
        "    let mixed = to_byte(1) | 1\n"
        "    let wrong = to_byte(true)\n"
        "}\n"};
    const vex::SemanticResult rejected = analyze(invalid);
    tests.check(has_code(rejected, "SEM022"),
                "int literals cannot narrow implicitly to byte");
    tests.check(has_code(rejected, "SEM012"),
                "byte bitwise operations reject mixed int and byte operands");
    tests.check(has_code(rejected, "SEM021"),
                "checked conversion requires an int argument");
}

void test_string_join_semantics(TestContext& tests) {
    const vex::SourceFile valid{"string_join.vex",
        "make main() {\n"
        "    let chunks: string[] = [\"VEX\", \"\"]\n"
        "    let result = join(chunks)\n"
        "    const immutable: string[] = [\"safe\",]\n"
        "    say(join(immutable))\n"
        "    say(result)\n"
        "}\n"};
    const vex::SemanticResult accepted = analyze(valid);
    tests.check(!accepted.has_errors(),
                "join accepts mutable and immutable dynamic string arrays");

    const vex::SourceFile invalid{"invalid_string_join.vex",
        "make main() {\n"
        "    let numbers: int[] = [1, 2]\n"
        "    let fixed: string[2] = [\"a\", \"b\"]\n"
        "    let wrong = join(numbers)\n"
        "    let also_wrong = join(fixed)\n"
        "}\n"};
    const vex::SemanticResult rejected = analyze(invalid);
    tests.check(has_code(rejected, "SEM021"),
                "join rejects non-string and fixed-size arrays");
}

void test_import_declarations_are_rejected_until_module_resolution(TestContext& tests) {
    const vex::SourceFile source{"unsupported_import.vex",
        "import \"math.vex\"\n"
        "make main() { say(1) }\n"};
    const vex::SemanticResult result = analyze(source);
    tests.check(has_code(result, "SEM061"),
                "parsed import declarations are rejected until module resolution exists");
}

void test_typed_pointer_semantics(TestContext& tests) {
    const vex::SourceFile valid{"typed_pointers.vex",
        "make bump(value: int) -> int {\n"
        "    let pointer: *int = &value\n"
        "    *pointer = *pointer + 1\n"
        "    return value\n"
        "}\n"
        "make main() {\n"
        "    let value = 41\n"
        "    let pointer = &value\n"
        "    *pointer = 42\n"
        "    say(value)\n"
        "    say(bump(value))\n"
        "}\n"};
    const vex::SemanticResult accepted = analyze(valid);
    tests.check(!accepted.has_errors(),
                "typed pointers support mutable locals, parameters, reads, and writes");

    const vex::SourceFile pointer_parameters{"pointer_parameters.vex", R"VEX(
make increment(pointer: *int) -> int {
    *pointer += 1
    return *pointer
}
make main() {
    let value = 4
    let pointer: *int = &value
    say(increment(pointer))
    say(value)
}
)VEX"};
    const vex::SemanticResult pointer_parameters_result = analyze(pointer_parameters);
    tests.check(!pointer_parameters_result.has_errors(),
                "typed pointers may be passed to functions as typed pointer parameters");

    const vex::SourceFile pointer_returns{"pointer_returns.vex", R"VEX(
make identity(pointer: *int) -> *int {
    return pointer
}
make main() {
    let value = 3
    let pointer: *int = &value
    let returned: *int = identity(pointer)
    *returned += 1
    say(value)
}
)VEX"};
    const vex::SemanticResult pointer_returns_result = analyze(pointer_returns);
    tests.check(!pointer_returns_result.has_errors(),
                "a function may return a typed pointer borrowed from its pointer parameter");

    const vex::SourceFile dangling_return{"dangling_pointer_return.vex", R"VEX(
make dangling() -> *int {
    let local = 3
    let pointer: *int = &local
    return pointer
}
make main() {}
)VEX"};
    const vex::SemanticResult dangling_return_result = analyze(dangling_return);
    tests.check(has_code(dangling_return_result, "SEM072"),
                "a function cannot return a pointer into its own local stack storage");

    const vex::SourceFile immutable{"pointer_to_const.vex",
        "make main() {\n"
        "    const value = 1\n"
        "    let pointer = &value\n"
        "}\n"};
    const vex::SemanticResult immutable_result = analyze(immutable);
    tests.check(has_code(immutable_result, "SEM070"),
                "address-of rejects immutable bindings");

    const vex::SourceFile escaping{"pointer_escape.vex",
        "make main() {\n"
        "    let outer = 1\n"
        "    let pointer = &outer\n"
        "    if (true) {\n"
        "        let inner = 2\n"
        "        pointer = &inner\n"
        "    }\n"
        "}\n"};
    const vex::SemanticResult escaping_result = analyze(escaping);
    tests.check(has_code(escaping_result, "SEM072"),
                "pointer assignment rejects a pointer to a shorter-lived local");

    const vex::SourceFile retargeted{"pointer_retarget.vex",
        "make main() {\n"
        "    let outer = 1\n"
        "    let outer_pointer = &outer\n"
        "    if (true) {\n"
        "        let inner = 2\n"
        "        let inner_pointer = &inner\n"
        "        inner_pointer = &outer\n"
        "        outer_pointer = inner_pointer\n"
        "    }\n"
        "}\n"};
    const vex::SemanticResult retargeted_result = analyze(retargeted);
    tests.check(!retargeted_result.has_errors(),
                "pointer reassignment refreshes the target origin before alias propagation");

    const vex::SourceFile aggregate{"aggregate_pointers.vex", R"VEX(
struct Inner { value: int }
struct Outer { inner: Inner, values: int[2] }
make update(point: Outer) -> int {
    let pointer: *Outer = &point
    (*pointer).inner.value = 7
    (*pointer).values[1] = 9
    return point.inner.value + point.values[1]
}
make main() {
    let outer = Outer { inner: Inner { value: 1 }, values: [3, 4] }
    let pointer: *Outer = &outer
    (*pointer).inner.value = 10
    (*pointer).values[0] = 8
    say(outer.inner.value)
    say(update(outer))
}
)VEX"};
    const vex::SemanticResult aggregate_result = analyze(aggregate);
    tests.check(!aggregate_result.has_errors(),
                "typed pointers may address mutable structures and project nested fields and arrays");
}

} // namespace

void run_semantic_tests(TestContext& tests) {
    test_valid_program_and_forward_calls(tests);
    test_scopes_shadowing_and_duplicates(tests);
    test_name_resolution_and_mutability(tests);
    test_operator_and_condition_types(tests);
    test_function_signatures_and_returns(tests);
    test_control_flow_return_analysis(tests);
    test_recursive_function_signatures(tests);
    test_semantic_model_exposes_typed_resolutions(tests);
    test_math_standard_library_signatures(tests);
    test_loop_control_scope_and_while_condition(tests);
    test_top_level_execution_scope(tests);
    test_fixed_arrays_and_index_assignment(tests);
    test_array_function_types_and_shapes(tests);
    test_nested_array_annotations_and_semantics(tests);
    test_import_declarations_are_rejected_until_module_resolution(tests);
    test_default_initialization_semantics(tests);
    test_dynamic_array_semantics(tests);
    test_dynamic_array_capacity_semantics(tests);
    test_integer_range_semantics(tests);
    test_for_in_rejects_non_array_values(tests);
    test_structure_types_value_semantics_and_diagnostics(tests);
    test_enum_declaration_semantics(tests);
    test_enum_and_structure_type_composition(tests);
    test_enum_match_semantics(tests);
    test_string_byte_length_semantics(tests);
    test_string_join_semantics(tests);
    test_bitwise_operator_semantics(tests);
    test_byte_type_and_checked_conversions(tests);
    test_typed_pointer_semantics(tests);
}
