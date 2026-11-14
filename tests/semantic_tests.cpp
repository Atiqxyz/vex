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
        "    let invalid_compound = \"text\" -= \"more\"\n"
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
    test_dynamic_array_semantics(tests);
    test_integer_range_semantics(tests);
    test_for_in_rejects_non_array_values(tests);
    test_structure_types_value_semantics_and_diagnostics(tests);
}
