#include "test_support.hpp"

#include "vex/ir.hpp"
#include "vex/c_backend.hpp"
#include "vex/llvm_backend.hpp"
#include "vex/lexer.hpp"
#include "vex/lowering.hpp"
#include "vex/parser.hpp"
#include "vex/semantic.hpp"
#include "vex/target_layout.hpp"

#include <sstream>
#include <string_view>
#include <variant>

namespace {

bool has_code(const vex::IRVerificationResult& result,
              const std::string_view code) {
    for (const vex::Diagnostic& diagnostic : result.diagnostics) {
        if (diagnostic.code == code) return true;
    }
    return false;
}

bool has_code(const vex::IRLoweringResult& result,
              const std::string_view code) {
    for (const vex::Diagnostic& diagnostic : result.diagnostics) {
        if (diagnostic.code == code) return true;
    }
    return false;
}

vex::IRValue value(const vex::IRValueId id, const vex::IRType type) {
    return vex::IRValue{id, type, {}};
}

vex::IRInstruction constant(const vex::IRValueId id, const vex::IRType type,
                            const char* spelling) {
    vex::IRInstruction instruction;
    instruction.op = vex::IROp::Constant;
    instruction.result = value(id, type);
    instruction.immediate = spelling;
    return instruction;
}

vex::IRFunction simple_integer_function() {
    vex::IRBlock entry;
    entry.id = 0;
    entry.name = "entry";
    entry.instructions.push_back(constant(1, vex::IRType::Int, "2"));
    entry.instructions.push_back(constant(2, vex::IRType::Int, "3"));

    vex::IRInstruction add;
    add.op = vex::IROp::Add;
    add.result = value(3, vex::IRType::Int);
    add.operands = {1, 2};
    entry.instructions.push_back(add);
    entry.terminator = vex::IRTerminator{vex::IRReturn{3}, {}};

    vex::IRFunction function;
    function.name = "sum";
    function.return_type = vex::IRType::Int;
    function.entry_block = 0;
    function.blocks.push_back(entry);
    return function;
}


void test_source_to_verified_ir_lowering(TestContext& tests) {
    const vex::SourceFile source{"lowering.vex",
        "make scale(value: float) -> float {\n"
        "    return value * 2.0\n"
        "}\n"
        "make main() {\n"
        "    let result = scale(3)\n"
        "    if result > 1.0 {\n"
        "        result = result + 1.0\n"
        "    } else {\n"
        "        result = result - 1.0\n"
        "    }\n"
        "    repeat 2 times {\n"
        "        result = result + 1.0\n"
        "    }\n"
        "    let iterations = 0\n"
        "    repeat 4 times {\n"
        "        iterations += 1\n"
        "        if iterations == 2 { continue }\n"
        "        if iterations == 3 { break }\n"
        "    }\n"
        "    let ticks = 0\n"
        "    while ticks < 6 {\n"
        "        ticks += 1\n"
        "        if ticks == 2 { continue }\n"
        "        if ticks == 5 { break }\n"
        "    }\n"
        "    say(result + ticks)\n"
        "}\n"};
    const vex::LexResult lexed = vex::Lexer(source).scan();
    const vex::ParseResult parsed = vex::Parser(source, lexed.tokens).parse();
    const vex::SemanticResult semantic =
        vex::SemanticAnalyzer(source, parsed.program).analyze();
    tests.check(!lexed.has_errors() && !parsed.has_errors() &&
                !semantic.has_errors(),
                "lowering fixture passes lexing, parsing, and semantic analysis");
    if (lexed.has_errors() || parsed.has_errors() || semantic.has_errors()) return;

    const vex::IRLoweringResult lowered =
        vex::IRLowerer(source, parsed.program, semantic.model).lower();
    tests.check(!lowered.has_errors(),
                "valid VEX functions lower into typed SSA IR");
    if (lowered.has_errors()) return;

    const vex::IRVerificationResult verified =
        vex::IRVerifier{}.verify(lowered.module);
    tests.check(!verified.has_errors(),
                "lowered functions pass the independent IR verifier");

    const vex::CBackendResult generated_c =
        vex::CBackend{}.generate(lowered.module);
    tests.check(!generated_c.has_errors() && !generated_c.source.empty(),
                "C backend accepts and emits the new loop-control IR");

    const vex::BackendResult generated_llvm =
        vex::LLVMBackend{}.emit(lowered.module);
    tests.check(!generated_llvm.has_errors() && !generated_llvm.output.empty(),
                "LLVM backend accepts and emits the new loop-control IR");
    tests.check(generated_llvm.output.find("br i1") != std::string::npos,
                "LLVM backend preserves while-loop conditional branches");

    tests.check(lowered.module.functions.size() == 2,
                "IR module contains every top-level function");

    bool has_conversion = false;
    bool has_conditional_branch = false;
    bool has_loop_backedge = false;
    for (const vex::IRFunction& function : lowered.module.functions) {
        for (const vex::IRBlock& block : function.blocks) {
            for (const vex::IRInstruction& instruction : block.instructions) {
                if (instruction.op == vex::IROp::IntToFloat) has_conversion = true;
            }
            if (block.terminator) {
                if (std::holds_alternative<vex::IRConditionalBranch>(
                        block.terminator->data)) {
                    has_conditional_branch = true;
                }
                if (const auto* branch =
                        std::get_if<vex::IRBranch>(&block.terminator->data)) {
                    if (branch->target == block.id ||
                        branch->target < block.id) {
                        has_loop_backedge = true;
                    }
                }
            }
        }
    }
    tests.check(has_conversion,
                "lowering inserts explicit int-to-float widening conversions");
    tests.check(has_conditional_branch,
                "if and repeat statements lower to conditional control flow");
    tests.check(has_loop_backedge,
                "repeat and while lower to loops with explicit back edges");
}

void test_implicit_and_explicit_entry_points(TestContext& tests) {
    const auto lower_source = [](const vex::SourceFile& source) {
        const vex::LexResult lexed = vex::Lexer(source).scan();
        const vex::ParseResult parsed = vex::Parser(source, lexed.tokens).parse();
        const vex::SemanticResult semantic =
            vex::SemanticAnalyzer(source, parsed.program).analyze();
        if (lexed.has_errors()) {
            return vex::IRLoweringResult{{}, lexed.diagnostics};
        }
        if (parsed.has_errors()) {
            return vex::IRLoweringResult{{}, parsed.diagnostics};
        }
        if (semantic.has_errors()) {
            return vex::IRLoweringResult{{}, semantic.diagnostics};
        }
        return vex::IRLowerer(source, parsed.program, semantic.model).lower();
    };

    const vex::SourceFile script{"implicit_main.vex",
        "let velocity = 7.5\n"
        "let time = 12.0\n"
        "say(velocity * time)\n"};
    const vex::IRLoweringResult implicit = lower_source(script);
    tests.check(!implicit.has_errors(),
                "top-level code lowers without an explicit main function");
    if (!implicit.has_errors()) {
        tests.check(implicit.module.functions.size() == 1 &&
                    implicit.module.functions.front().name == "main",
                    "compiler synthesizes a main entry point for top-level code");
        const vex::CBackendResult c = vex::CBackend{}.generate(implicit.module);
        const vex::BackendResult llvm = vex::LLVMBackend{}.emit(implicit.module);
        tests.check(!c.has_errors() && !c.source.empty(),
                    "C backend accepts the synthesized main function");
        tests.check(!llvm.has_errors() && !llvm.output.empty(),
                    "LLVM backend accepts the synthesized main function");
    }

    const vex::SourceFile explicit_main{"explicit_main.vex",
        "make main() {\n"
        "    say(\"controlled\")\n"
        "}\n"};
    const vex::IRLoweringResult explicit_result = lower_source(explicit_main);
    tests.check(!explicit_result.has_errors(),
                "an explicit main function remains valid");

    const vex::SourceFile mixed{"mixed_entry.vex",
        "say(\"top level\")\n"
        "make main() {\n"
        "    say(\"explicit\")\n"
        "}\n"};
    const vex::IRLoweringResult mixed_result = lower_source(mixed);
    tests.check(has_code(mixed_result, "IR140"),
                "top-level executable statements are rejected when explicit main exists");
}

void test_valid_typed_function(TestContext& tests) {
    vex::IRModule module;
    module.functions.push_back(simple_integer_function());

    const vex::IRVerificationResult result = vex::IRVerifier{}.verify(module);
    tests.check(!result.has_errors(),
                "well-typed SSA function with constants, arithmetic, and return verifies");

    std::ostringstream printed;
    vex::print_ir(printed, module);
    tests.check(printed.str().find("func sum") != std::string::npos &&
                printed.str().find("add") != std::string::npos,
                "IR printer emits readable function and instruction names");
}

void test_block_arguments_and_control_flow(TestContext& tests) {
    vex::IRBlock entry;
    entry.id = 0;
    entry.name = "entry";
    entry.instructions.push_back(constant(1, vex::IRType::Bool, "true"));
    vex::IRConditionalBranch conditional;
    conditional.condition = 1;
    conditional.true_target = 1;
    conditional.false_target = 2;
    entry.terminator = vex::IRTerminator{conditional, {}};

    vex::IRBlock left;
    left.id = 1;
    left.name = "left";
    left.instructions.push_back(constant(2, vex::IRType::Int, "10"));
    left.terminator = vex::IRTerminator{vex::IRBranch{3, {2}}, {}};

    vex::IRBlock right;
    right.id = 2;
    right.name = "right";
    right.instructions.push_back(constant(3, vex::IRType::Int, "20"));
    right.terminator = vex::IRTerminator{vex::IRBranch{3, {3}}, {}};

    vex::IRBlock merge;
    merge.id = 3;
    merge.name = "merge";
    merge.parameters.push_back(value(4, vex::IRType::Int));
    merge.terminator = vex::IRTerminator{vex::IRReturn{4}, {}};

    vex::IRFunction function;
    function.name = "choose";
    function.return_type = vex::IRType::Int;
    function.entry_block = 0;
    function.blocks = {entry, left, right, merge};

    vex::IRModule module;
    module.functions.push_back(function);
    const vex::IRVerificationResult result = vex::IRVerifier{}.verify(module);
    tests.check(!result.has_errors(),
                "branch arguments correctly populate typed merge-block parameters");
}

void test_invalid_ir_is_rejected(TestContext& tests) {
    vex::IRFunction missing_terminator;
    missing_terminator.name = "unfinished";
    missing_terminator.entry_block = 0;
    vex::IRBlock empty;
    empty.id = 0;
    missing_terminator.blocks.push_back(empty);

    vex::IRModule module;
    module.functions.push_back(missing_terminator);
    const vex::IRVerificationResult missing =
        vex::IRVerifier{}.verify(module);
    tests.check(has_code(missing, "IR028"),
                "verifier rejects a basic block without a terminator");

    vex::IRBlock entry;
    entry.id = 0;
    entry.instructions.push_back(constant(1, vex::IRType::Bool, "true"));
    entry.instructions.push_back(constant(2, vex::IRType::Bool, "false"));
    vex::IRInstruction invalid_add;
    invalid_add.op = vex::IROp::Add;
    invalid_add.result = value(3, vex::IRType::Bool);
    invalid_add.operands = {1, 2};
    entry.instructions.push_back(invalid_add);
    vex::IRInstruction invalid_string_concat;
    invalid_string_concat.op = vex::IROp::StringConcat;
    invalid_string_concat.result = value(4, vex::IRType::String);
    invalid_string_concat.operands = {1, 2};
    entry.instructions.push_back(invalid_string_concat);
    entry.terminator = vex::IRTerminator{vex::IRReturn{3}, {}};

    vex::IRFunction invalid_types;
    invalid_types.name = "bad_add";
    invalid_types.return_type = vex::IRType::Bool;
    invalid_types.entry_block = 0;
    invalid_types.blocks.push_back(entry);
    module.functions.clear();
    module.functions.push_back(invalid_types);

    const vex::IRVerificationResult bad_types =
        vex::IRVerifier{}.verify(module);
    tests.check(has_code(bad_types, "IR013"),
                "verifier rejects arithmetic on non-numeric values");
    tests.check(has_code(bad_types, "IR072"),
                "verifier rejects StringConcat with non-string operands");
}

void test_dominance_and_call_signatures(TestContext& tests) {
    vex::IRBlock entry;
    entry.id = 0;
    entry.instructions.push_back(constant(1, vex::IRType::Bool, "true"));
    entry.terminator = vex::IRTerminator{
        vex::IRConditionalBranch{1, 1, {}, 2, {}}, {}
    };

    vex::IRBlock left;
    left.id = 1;
    left.instructions.push_back(constant(2, vex::IRType::Int, "7"));
    left.terminator = vex::IRTerminator{vex::IRBranch{3, {}}, {}};

    vex::IRBlock right;
    right.id = 2;
    right.terminator = vex::IRTerminator{vex::IRBranch{3, {}}, {}};

    vex::IRBlock merge;
    merge.id = 3;
    merge.terminator = vex::IRTerminator{vex::IRReturn{2}, {}};

    vex::IRFunction non_dominating;
    non_dominating.name = "bad_merge";
    non_dominating.return_type = vex::IRType::Int;
    non_dominating.entry_block = 0;
    non_dominating.blocks = {entry, left, right, merge};

    vex::IRModule module;
    module.functions.push_back(non_dominating);
    const vex::IRVerificationResult dominance =
        vex::IRVerifier{}.verify(module);
    tests.check(has_code(dominance, "IR035"),
                "verifier rejects values that do not dominate their uses");

    vex::IRFunction caller;
    caller.name = "caller";
    caller.return_type = vex::IRType::Int;
    caller.entry_block = 0;
    vex::IRBlock caller_entry;
    caller_entry.id = 0;
    caller_entry.instructions.push_back(constant(10, vex::IRType::Int, "4"));
    vex::IRInstruction call;
    call.op = vex::IROp::Call;
    call.result = value(11, vex::IRType::Int);
    call.operands = {10};
    call.callee = "increment";
    caller_entry.instructions.push_back(call);
    caller_entry.terminator = vex::IRTerminator{vex::IRReturn{11}, {}};
    caller.blocks.push_back(caller_entry);

    vex::IRFunction callee;
    callee.name = "increment";
    callee.return_type = vex::IRType::Int;
    callee.entry_block = 0;
    callee.parameters.push_back({"number", value(20, vex::IRType::Int)});
    vex::IRBlock callee_entry;
    callee_entry.id = 0;
    callee_entry.terminator = vex::IRTerminator{vex::IRReturn{20}, {}};
    callee.blocks.push_back(callee_entry);

    module.functions = {caller, callee};
    const vex::IRVerificationResult call_result =
        vex::IRVerifier{}.verify(module);
    tests.check(!call_result.has_errors(),
                "direct calls validate against signatures even when the callee is declared later");
}



void test_struct_lowering_and_backend_aggregates(TestContext& tests) {
    const vex::SourceFile source{"struct_ir.vex",
        "struct Point { x: int, y: int }\n"
        "make change(point: Point) -> Point {\n"
        "    let copy = point\n"
        "    copy.x = copy.x + 4\n"
        "    return copy\n"
        "}\n"
        "make main() {\n"
        "    let point = Point { x: 1, y: 2 }\n"
        "    let changed = change(point)\n"
        "    let branch_value = point\n"
        "    if true {\n"
        "        branch_value.x = branch_value.x + 1\n"
        "    } else {\n"
        "        branch_value.y = branch_value.y + 1\n"
        "    }\n"
        "    say(point.x)\n"
        "    say(changed.x)\n"
        "    say(branch_value.x)\n"
        "}\n"};
    const vex::LexResult lexed = vex::Lexer(source).scan();
    const vex::ParseResult parsed = vex::Parser(source, lexed.tokens).parse();
    const vex::SemanticResult semantic =
        vex::SemanticAnalyzer(source, parsed.program).analyze();
    tests.check(!lexed.has_errors() && !parsed.has_errors() && !semantic.has_errors(),
                "structure fixture passes front-end stages");
    if (lexed.has_errors() || parsed.has_errors() || semantic.has_errors()) return;
    const vex::IRLoweringResult lowered =
        vex::IRLowerer(source, parsed.program, semantic.model).lower();
    tests.check(!lowered.has_errors(), "structure values lower to typed IR");
    if (lowered.has_errors()) return;
    const vex::IRVerificationResult verified = vex::IRVerifier{}.verify(lowered.module);
    tests.check(!verified.has_errors(), "structure aggregate IR passes the independent verifier");
    if (verified.has_errors()) return;
    bool make = false, get = false, set = false;
    for (const auto& function : lowered.module.functions)
        for (const auto& block : function.blocks)
            for (const auto& instruction : block.instructions) {
                make |= instruction.op == vex::IROp::StructMake;
                get |= instruction.op == vex::IROp::StructGet;
                set |= instruction.op == vex::IROp::StructSet;
            }
    tests.check(lowered.module.struct_types.size() == 1 && make && get && set,
                "IR carries the named layout and all three structure operations");
    const vex::CBackendResult c = vex::CBackend{}.generate(lowered.module);
    tests.check(!c.has_errors() &&
                c.source.find("typedef struct vex_struct_1") != std::string::npos &&
                c.source.find(".vex_field_0 =") != std::string::npos,
                "C backend emits native structure values and field updates");
    const vex::BackendResult llvm = vex::LLVMBackend{}.emit(lowered.module);
    tests.check(!llvm.has_errors() &&
                llvm.output.find("%vex.struct.1 = type") != std::string::npos &&
                llvm.output.find("insertvalue") != std::string::npos &&
                llvm.output.find("extractvalue") != std::string::npos,
                "LLVM backend emits named aggregate types and field operations");
}

void test_composed_structure_arrays_and_recursive_dynamic_fields(TestContext& tests) {
    const vex::SourceFile source{"composed_struct_arrays_ir.vex",
        "struct Point { x: int, y: int }\n"
        "struct Group { fixed: Point[2], items: Point[] }\n"
        "struct Node { value: int, children: Node[] }\n"
        "make identity(points: Point[]) -> Point[] { return points }\n"
        "make fixed_identity(points: Point[2]) -> Point[2] { return points }\n"
        "make main() {\n"
        "    let points: Point[2] = [Point { x: 1, y: 2 }, Point { x: 3, y: 4 }]\n"
        "    let items: Point[] = []\n"
        "    push(items, points[0])\n"
        "    let group = Group { fixed: fixed_identity(points), items: identity(items) }\n"
        "    group.fixed[1].x = 8\n"
        "    group.items[0].y = 9\n"
        "    for item in group.items { say(item.x) }\n"
        "    let node = Node { value: 1, children: [] }\n"
        "    push(node.children, node)\n"
        "}\n"};
    const vex::LexResult lexed = vex::Lexer(source).scan();
    const vex::ParseResult parsed = vex::Parser(source, lexed.tokens).parse();
    const vex::SemanticResult semantic =
        vex::SemanticAnalyzer(source, parsed.program).analyze();
    tests.check(!lexed.has_errors() && !parsed.has_errors() &&
                !semantic.has_errors(),
                "structure arrays, array fields, and recursive dynamic fields pass semantic analysis");
    if (lexed.has_errors() || parsed.has_errors() || semantic.has_errors()) return;

    const vex::IRLoweringResult lowered =
        vex::IRLowerer(source, parsed.program, semantic.model).lower();
    tests.check(!lowered.has_errors(),
                "composed structure arrays lower through calls, nested writes, and iteration");
    if (lowered.has_errors()) return;

    const vex::IRVerificationResult verified = vex::IRVerifier{}.verify(lowered.module);
    tests.check(!verified.has_errors(),
                "nominal array element identity and recursive pointer-backed fields verify in IR");
    if (verified.has_errors()) return;

    vex::IRModule invalid_nominal_identity = lowered.module;
    bool changed_structure_array = false;
    for (vex::IRFunction& function : invalid_nominal_identity.functions) {
        for (vex::IRBlock& block : function.blocks) {
            for (vex::IRInstruction& instruction : block.instructions) {
                if (instruction.op == vex::IROp::ArrayMake && instruction.result &&
                    instruction.result->element_type == vex::IRType::Struct) {
                    instruction.result->element_struct_type_id = 2;
                    changed_structure_array = true;
                    break;
                }
            }
            if (changed_structure_array) break;
        }
        if (changed_structure_array) break;
    }
    const vex::IRVerificationResult invalid_nominal_result =
        vex::IRVerifier{}.verify(invalid_nominal_identity);
    tests.check(changed_structure_array && invalid_nominal_result.has_errors(),
                "IR verifier rejects an array construction with a mismatched nominal structure identity");

    const vex::CBackendResult c = vex::CBackend{}.generate(lowered.module);
    tests.check(!c.has_errors() &&
                c.source.find("vex_array_struct_1_2") != std::string::npos &&
                c.source.find("vex_array_push") != std::string::npos,
                "C backend emits fixed structure-array wrappers and dynamic structure storage");
    const vex::BackendResult llvm = vex::LLVMBackend{}.emit(lowered.module);
    tests.check(!llvm.has_errors() &&
                llvm.output.find("[2 x %vex.struct.1]") != std::string::npos &&
                llvm.output.find("getelementptr %vex.struct.1, ptr null, i64 1") != std::string::npos,
                "LLVM backend emits nominal structure arrays and derives their target allocation size");
}

void test_fixed_array_lowering_and_backends(TestContext& tests) {
    const vex::SourceFile source{"arrays_ir.vex",
        "make main() {\n"
        "    let values = [1, 2, 3]\n"
        "    values[1] = 9\n"
        "    let selected = values[1]\n"
        "    for item in values { say(item) }\n"
        "    say(selected)\n"
        "}\n"};
    const vex::LexResult lexed = vex::Lexer(source).scan();
    const vex::ParseResult parsed = vex::Parser(source, lexed.tokens).parse();
    const vex::SemanticResult semantic =
        vex::SemanticAnalyzer(source, parsed.program).analyze();
    tests.check(!lexed.has_errors() && !parsed.has_errors() &&
                !semantic.has_errors(),
                "fixed array fixture passes lexing, parsing, and semantic analysis");
    if (lexed.has_errors() || parsed.has_errors() || semantic.has_errors()) return;

    const vex::IRLoweringResult lowered =
        vex::IRLowerer(source, parsed.program, semantic.model).lower();
    tests.check(!lowered.has_errors(),
                "fixed arrays, indexed writes, and for-in lower to SSA");
    if (lowered.has_errors()) return;

    const vex::IRVerificationResult verified =
        vex::IRVerifier{}.verify(lowered.module);
    tests.check(!verified.has_errors(),
                "array SSA values and loop-carried aggregate values verify");
    if (verified.has_errors()) return;

    bool has_array_make = false;
    bool has_array_get = false;
    bool has_array_set = false;
    for (const vex::IRFunction& function : lowered.module.functions) {
        for (const vex::IRBlock& block : function.blocks) {
            for (const vex::IRInstruction& instruction : block.instructions) {
                has_array_make |= instruction.op == vex::IROp::ArrayMake;
                has_array_get |= instruction.op == vex::IROp::ArrayGet;
                has_array_set |= instruction.op == vex::IROp::ArraySet;
            }
        }
    }
    tests.check(has_array_make && has_array_get && has_array_set,
                "IR contains array construction, reads, and updates");

    const vex::CBackendResult c = vex::CBackend{}.generate(lowered.module);
    tests.check(!c.has_errors() && c.source.find("vex_array_int_3") != std::string::npos &&
                c.source.find("vex_checked_array_index") != std::string::npos,
                "C backend emits fixed array storage and bounds checks");

    const vex::BackendResult llvm = vex::LLVMBackend{}.emit(lowered.module);
    tests.check(!llvm.has_errors() && llvm.output.find("[3 x i64]") != std::string::npos &&
                llvm.output.find("@vex_checked_array_index") != std::string::npos,
                "LLVM backend emits aggregate arrays and bounds checks");
}

void test_dynamic_array_capacity_lowering_and_backends(TestContext& tests) {
    const vex::SourceFile source{"dynamic_array_capacity_ir.vex",
        "struct Sample { marker: bool, value: float, samples: float[2] }\n"
        "make main() {\n"
        "    let fixed = [1, 2]\n"
        "    let values: int[] = []\n"
        "    let label = \"bad\"\n"
        "    let before = capacity(values)\n"
        "    reserve(values, 12)\n"
        "    let records: Sample[] = []\n"
        "    reserve(records, 4)\n"
        "    push(records, Sample { marker: true, value: 1.5, samples: [2.5, 3.5] })\n"
        "    say(before)\n"
        "}\n"};
    const vex::LexResult lexed = vex::Lexer(source).scan();
    const vex::ParseResult parsed = vex::Parser(source, lexed.tokens).parse();
    const vex::SemanticResult semantic =
        vex::SemanticAnalyzer(source, parsed.program).analyze();
    tests.check(!lexed.has_errors() && !parsed.has_errors() && !semantic.has_errors(),
                "capacity and reserve fixture passes frontend analysis");
    if (lexed.has_errors() || parsed.has_errors() || semantic.has_errors()) return;

    const vex::IRLoweringResult lowered =
        vex::IRLowerer(source, parsed.program, semantic.model).lower();
    tests.check(!lowered.has_errors(),
                "capacity and reserve lower into dedicated typed IR operations");
    if (lowered.has_errors()) return;

    std::size_t capacity_count = 0;
    std::size_t reserve_count = 0;
    vex::IRValueId fixed_array_id = 0;
    vex::IRValueId string_id = 0;
    for (const vex::IRFunction& function : lowered.module.functions) {
        for (const vex::IRBlock& block : function.blocks) {
            for (const vex::IRInstruction& instruction : block.instructions) {
                if (instruction.op == vex::IROp::ArrayCapacity) ++capacity_count;
                if (instruction.op == vex::IROp::ArrayReserve) ++reserve_count;
                if (instruction.op == vex::IROp::ArrayMake && instruction.result &&
                    instruction.result->array_length != 0)
                    fixed_array_id = instruction.result->id;
                if (instruction.op == vex::IROp::Constant && instruction.result &&
                    instruction.result->type == vex::IRType::String)
                    string_id = instruction.result->id;
            }
        }
    }
    tests.check(capacity_count == 1 && reserve_count == 2,
                "capacity and reserve each lower to the expected dedicated IR operations");
    const auto verified = vex::IRVerifier{}.verify(lowered.module);
    tests.check(!verified.has_errors(),
                "dynamic-array capacity and reservation pass independent IR verification");

    vex::IRModule invalid_capacity = lowered.module;
    bool changed_capacity = false;
    for (vex::IRFunction& function : invalid_capacity.functions) {
        for (vex::IRBlock& block : function.blocks) {
            for (vex::IRInstruction& instruction : block.instructions) {
                if (instruction.op == vex::IROp::ArrayCapacity && fixed_array_id != 0) {
                    instruction.operands[0] = fixed_array_id;
                    changed_capacity = true;
                    break;
                }
            }
            if (changed_capacity) break;
        }
        if (changed_capacity) break;
    }
    const auto rejected_capacity = vex::IRVerifier{}.verify(invalid_capacity);
    tests.check(changed_capacity && has_code(rejected_capacity, "IR075"),
                "IR verifier rejects capacity inspection on fixed arrays");

    vex::IRModule invalid_reserve = lowered.module;
    bool changed_reserve = false;
    for (vex::IRFunction& function : invalid_reserve.functions) {
        for (vex::IRBlock& block : function.blocks) {
            for (vex::IRInstruction& instruction : block.instructions) {
                if (instruction.op == vex::IROp::ArrayReserve && string_id != 0) {
                    instruction.operands[1] = string_id;
                    changed_reserve = true;
                    break;
                }
            }
            if (changed_reserve) break;
        }
        if (changed_reserve) break;
    }
    const auto rejected_reserve = vex::IRVerifier{}.verify(invalid_reserve);
    tests.check(changed_reserve && has_code(rejected_reserve, "IR077"),
                "IR verifier rejects a non-integer reservation capacity");

    const vex::CBackendResult c = vex::CBackend{}.generate(lowered.module);
    tests.check(!c.has_errors() &&
                c.source.find("vex_array_capacity(") != std::string::npos &&
                c.source.find("vex_array_reserve(") != std::string::npos,
                "C backend emits checked dynamic-array capacity and reservation helpers");
    const vex::BackendResult llvm = vex::LLVMBackend{}.emit(lowered.module);
    tests.check(!llvm.has_errors() &&
                llvm.output.find("define internal i64 @vex_array_capacity") != std::string::npos &&
                llvm.output.find("define internal void @vex_array_reserve") != std::string::npos &&
                llvm.output.find("call void @vex_array_reserve") != std::string::npos,
                "LLVM backend emits target-sized dynamic-array capacity and reservation helpers");
}

void test_string_byte_length_lowering_and_backends(TestContext& tests) {
    const vex::SourceFile source{"string_length_ir.vex",
        "make main() {\n"
        "    let unused_integer = 42\n"
        "    let embedded_nul_count = length(\"A\\0B\")\n"
        "    let empty_count = length(\"\")\n"
        "    say(embedded_nul_count)\n"
        "    say(empty_count)\n"
        "}\n"};
    const vex::LexResult lexed = vex::Lexer(source).scan();
    const vex::ParseResult parsed = vex::Parser(source, lexed.tokens).parse();
    const vex::SemanticResult semantic =
        vex::SemanticAnalyzer(source, parsed.program).analyze();
    tests.check(!lexed.has_errors() && !parsed.has_errors() && !semantic.has_errors(),
                "byte-length fixture passes lexical, syntactic, and semantic analysis");
    if (lexed.has_errors() || parsed.has_errors() || semantic.has_errors()) return;

    const vex::IRLoweringResult lowered =
        vex::IRLowerer(source, parsed.program, semantic.model).lower();
    tests.check(!lowered.has_errors(),
                "string byte length lowers into typed IR");
    if (lowered.has_errors()) return;

    std::size_t byte_length_operations = 0;
    vex::IRModule invalid_operand = lowered.module;
    vex::IRValueId integer_id = 0;
    for (const vex::IRFunction& function : lowered.module.functions) {
        for (const vex::IRBlock& block : function.blocks) {
            for (const vex::IRInstruction& instruction : block.instructions) {
                if (instruction.op == vex::IROp::Constant && instruction.result &&
                    instruction.result->type == vex::IRType::Int) {
                    integer_id = instruction.result->id;
                }
                if (instruction.op == vex::IROp::StringLength)
                    ++byte_length_operations;
            }
        }
    }
    tests.check(byte_length_operations == 2,
                "both non-empty and empty strings lower to string_length operations");

    const vex::IRVerificationResult verified = vex::IRVerifier{}.verify(lowered.module);
    tests.check(!verified.has_errors(),
                "string-length operation passes independent IR verification");

    bool corrupted = false;
    for (vex::IRFunction& function : invalid_operand.functions) {
        for (vex::IRBlock& block : function.blocks) {
            for (vex::IRInstruction& instruction : block.instructions) {
                if (instruction.op == vex::IROp::StringLength) {
                    instruction.operands[0] = integer_id;
                    corrupted = true;
                    break;
                }
            }
            if (corrupted) break;
        }
        if (corrupted) break;
    }
    const vex::IRVerificationResult rejected = vex::IRVerifier{}.verify(invalid_operand);
    tests.check(corrupted && integer_id != 0 && has_code(rejected, "IR073"),
                "IR verifier rejects string length applied to a non-string value");

    const vex::CBackendResult c = vex::CBackend{}.generate(lowered.module);
    tests.check(!c.has_errors() && c.source.find(".length;") != std::string::npos,
                "C backend reads the length-aware string descriptor");
    const vex::BackendResult llvm = vex::LLVMBackend{}.emit(lowered.module);
    tests.check(!llvm.has_errors() &&
                llvm.output.find("extractvalue { ptr, i64 }") != std::string::npos,
                "LLVM backend extracts the string descriptor byte count");
}

void test_string_join_lowering_and_backends(TestContext& tests) {
    const vex::SourceFile source{"string_join_ir.vex",
        "make main() {\n"
        "    let marker = 7\n"
        "    let chunks: string[] = [\"A\", \"\\0B\", \"\"]\n"
        "    let joined = join(chunks)\n"
        "    say(length(joined))\n"
        "}\n"};
    const vex::LexResult lexed = vex::Lexer(source).scan();
    const vex::ParseResult parsed = vex::Parser(source, lexed.tokens).parse();
    const vex::SemanticResult semantic =
        vex::SemanticAnalyzer(source, parsed.program).analyze();
    tests.check(!lexed.has_errors() && !parsed.has_errors() && !semantic.has_errors(),
                "string join fixture passes frontend analysis");
    if (lexed.has_errors() || parsed.has_errors() || semantic.has_errors()) return;

    const vex::IRLoweringResult lowered =
        vex::IRLowerer(source, parsed.program, semantic.model).lower();
    tests.check(!lowered.has_errors(), "string join lowers into typed IR");
    if (lowered.has_errors()) return;

    vex::IRModule malformed = lowered.module;
    vex::IRValueId integer_id = 0;
    std::size_t join_count = 0;
    for (const vex::IRFunction& function : lowered.module.functions) {
        for (const vex::IRBlock& block : function.blocks) {
            for (const vex::IRInstruction& instruction : block.instructions) {
                if (instruction.op == vex::IROp::Constant && instruction.result &&
                    instruction.result->type == vex::IRType::Int)
                    integer_id = instruction.result->id;
                if (instruction.op == vex::IROp::StringJoin) ++join_count;
            }
        }
    }
    tests.check(join_count == 1, "join lowers to one dedicated string_join IR operation");
    tests.check(!vex::IRVerifier{}.verify(lowered.module).has_errors(),
                "string_join passes independent IR verification");

    bool corrupted = false;
    for (vex::IRFunction& function : malformed.functions) {
        for (vex::IRBlock& block : function.blocks) {
            for (vex::IRInstruction& instruction : block.instructions) {
                if (instruction.op == vex::IROp::StringJoin && integer_id != 0) {
                    instruction.operands[0] = integer_id;
                    corrupted = true;
                    break;
                }
            }
            if (corrupted) break;
        }
        if (corrupted) break;
    }
    const auto rejected = vex::IRVerifier{}.verify(malformed);
    tests.check(corrupted && has_code(rejected, "IR074"),
                "IR verifier rejects joining a non-string-array value");

    const vex::CBackendResult c = vex::CBackend{}.generate(lowered.module);
    tests.check(!c.has_errors() &&
                c.source.find("vex_string_join(vex_v_") != std::string::npos &&
                c.source.find("static vex_string vex_string_join(vex_array *parts)") != std::string::npos,
                "C backend emits and calls the checked string-join runtime helper");
    const vex::BackendResult llvm = vex::LLVMBackend{}.emit(lowered.module);
    tests.check(!llvm.has_errors() &&
                llvm.output.find("call { ptr, i64 } @vex_string_join(ptr ") != std::string::npos &&
                llvm.output.find("define internal { ptr, i64 } @vex_string_join(ptr %parts)") != std::string::npos,
                "LLVM backend emits and calls the checked string-join runtime helper");

    const auto c_join_start = c.source.find("static vex_string vex_string_join");
    const auto c_join_end = c.source.find("static void vex_string_cleanup", c_join_start);
    const bool has_c_join_helper = c_join_start != std::string::npos &&
                                   c_join_end != std::string::npos;
    bool c_join_uses_one_allocation = false;
    if (has_c_join_helper) {
        const std::string_view helper{c.source.data() + c_join_start,
                                      c_join_end - c_join_start};
        const auto allocation = helper.find("malloc(");
        c_join_uses_one_allocation = allocation != std::string_view::npos &&
            helper.find("malloc(", allocation + 1) == std::string_view::npos &&
            helper.find("if (length == 0u) return") < allocation;
    }
    tests.check(c_join_uses_one_allocation,
                "C string join allocates once and returns empty output before allocation");

    const auto llvm_join_start = llvm.output.find(
        "define internal { ptr, i64 } @vex_string_join");
    const auto llvm_join_end = llvm.output.find(
        "define internal void @vex_string_cleanup", llvm_join_start);
    const bool has_llvm_join_helper = llvm_join_start != std::string::npos &&
                                      llvm_join_end != std::string::npos;
    bool llvm_join_uses_one_allocation = false;
    if (has_llvm_join_helper) {
        const std::string_view helper{llvm.output.data() + llvm_join_start,
                                      llvm_join_end - llvm_join_start};
        const auto empty_return = helper.find("ret { ptr, i64 } %empty_result");
        const auto allocation = helper.find("call ptr @malloc(i64");
        llvm_join_uses_one_allocation = allocation != std::string_view::npos &&
            helper.find("call ptr @malloc(i64", allocation + 1) == std::string_view::npos &&
            empty_return < allocation;
    }
    tests.check(llvm_join_uses_one_allocation,
                "LLVM string join allocates once and returns empty output before allocation");
}

void test_bitwise_ir_lowering_and_backends(TestContext& tests) {
    const vex::SourceFile source{"bitwise_ir.vex",
        "make main() {\n"
        "    let a = 12 & 10\n"
        "    let b = 12 | 10\n"
        "    let c = 12 ^ 10\n"
        "    let d = ~5\n"
        "    say(a + b + c + d)\n"
        "}\n"};
    const auto lexed = vex::Lexer(source).scan();
    const auto parsed = vex::Parser(source, lexed.tokens).parse();
    const auto semantic = vex::SemanticAnalyzer(source, parsed.program).analyze();
    tests.check(!lexed.has_errors() && !parsed.has_errors() && !semantic.has_errors(),
                "bitwise source passes lexing, parsing, and semantic analysis");
    if (lexed.has_errors() || parsed.has_errors() || semantic.has_errors()) return;
    const auto lowered = vex::IRLowerer(source, parsed.program, semantic.model).lower();
    tests.check(!lowered.has_errors(), "bitwise operators lower into typed IR");
    if (lowered.has_errors()) return;
    std::size_t and_count = 0, or_count = 0, xor_count = 0, not_count = 0;
    for (const auto& function : lowered.module.functions)
        for (const auto& block : function.blocks)
            for (const auto& instruction : block.instructions) {
                and_count += instruction.op == vex::IROp::BitwiseAnd ? 1u : 0u;
                or_count += instruction.op == vex::IROp::BitwiseOr ? 1u : 0u;
                xor_count += instruction.op == vex::IROp::BitwiseXor ? 1u : 0u;
                not_count += instruction.op == vex::IROp::BitwiseNot ? 1u : 0u;
            }
    tests.check(and_count == 1 && or_count == 1 && xor_count == 1 && not_count == 1,
                "IR contains each of the four bitwise operations exactly once");
    tests.check(!vex::IRVerifier{}.verify(lowered.module).has_errors(),
                "bitwise operations pass independent IR verification");
    const auto c = vex::CBackend{}.generate(lowered.module);
    tests.check(!c.has_errors() &&
                c.source.find("vex_i64_bit_and(") != std::string::npos &&
                c.source.find("vex_i64_bit_not(") != std::string::npos,
                "C backend emits defined 64-bit bit-pattern helpers");
    const auto llvm = vex::LLVMBackend{}.emit(lowered.module);
    tests.check(!llvm.has_errors() &&
                llvm.output.find(" = and i64 ") != std::string::npos &&
                llvm.output.find(" = or i64 ") != std::string::npos &&
                llvm.output.find(" = xor i64 ") != std::string::npos,
                "LLVM backend emits native i64 bitwise instructions");
}

void test_default_initialized_scalar_lowering_and_backends(TestContext& tests) {
    const vex::SourceFile source{"default_initialized_scalars_ir.vex",
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
    const auto lexed = vex::Lexer(source).scan();
    const auto parsed = vex::Parser(source, lexed.tokens).parse();
    const auto semantic = vex::SemanticAnalyzer(source, parsed.program).analyze();
    tests.check(!lexed.has_errors() && !parsed.has_errors() && !semantic.has_errors(),
                "default-initialized scalar declarations pass front-end analysis");
    if (lexed.has_errors() || parsed.has_errors() || semantic.has_errors()) return;
    const auto lowered = vex::IRLowerer(source, parsed.program, semantic.model).lower();
    tests.check(!lowered.has_errors(), "default scalar values lower to explicit typed IR constants");
    if (lowered.has_errors()) return;
    tests.check(!vex::IRVerifier{}.verify(lowered.module).has_errors(),
                "default-initialization IR passes independent verification");

    std::size_t default_constants = 0;
    for (const auto& function : lowered.module.functions)
        for (const auto& block : function.blocks)
            for (const auto& instruction : block.instructions)
                if (instruction.op == vex::IROp::Constant && instruction.result &&
                    (instruction.result->type == vex::IRType::Int ||
                     instruction.result->type == vex::IRType::Byte ||
                     instruction.result->type == vex::IRType::Float ||
                     instruction.result->type == vex::IRType::Bool ||
                     instruction.result->type == vex::IRType::String))
                    ++default_constants;
    tests.check(default_constants == 5u,
                "IR contains deterministic defaults for int, float, bool, byte, and string");

    const auto c = vex::CBackend{}.generate(lowered.module);
    tests.check(!c.has_errors() && c.source.find("INT64_C(0)") != std::string::npos &&
                c.source.find("0.0") != std::string::npos &&
                c.source.find("false") != std::string::npos &&
                c.source.find("(uint8_t)0u") != std::string::npos,
                "C backend emits deterministic native scalar defaults");
    const auto llvm = vex::LLVMBackend{}.emit(lowered.module);
    tests.check(!llvm.has_errors() && llvm.output.find("add i64 0, 0") != std::string::npos &&
                llvm.output.find("fadd double 0.0, 0x0000000000000000") != std::string::npos &&
                llvm.output.find("xor i1 false, false") != std::string::npos,
                "LLVM backend emits matching deterministic scalar defaults");
}

void test_recursive_array_ir_shapes_and_backends(TestContext& tests) {
    vex::IRModule module;

    const auto make_array = [](const vex::IRValue& result,
                               const std::vector<vex::IRValueId>& elements) {
        vex::IRInstruction instruction;
        instruction.op = vex::IROp::ArrayMake;
        instruction.result = result;
        instruction.operands = elements;
        return instruction;
    };

    vex::IRBlock fixed_entry;
    fixed_entry.id = 0;
    fixed_entry.name = "entry";
    for (std::uint32_t index = 1; index <= 6; ++index) {
        const std::string spelling = std::to_string(index);
        fixed_entry.instructions.push_back(
            constant(index, vex::IRType::Int, spelling.c_str()));
    }
    vex::IRValue fixed_row_a = vex::ir_make_array_value(
        vex::IRType::Int, {3u});
    fixed_row_a.id = 7;
    vex::IRValue fixed_row_b = vex::ir_make_array_value(
        vex::IRType::Int, {3u});
    fixed_row_b.id = 8;
    vex::IRValue fixed_grid = vex::ir_make_array_value(
        vex::IRType::Int, {2u, 3u});
    fixed_grid.id = 9;
    fixed_entry.instructions.push_back(make_array(fixed_row_a, {1, 2, 3}));
    fixed_entry.instructions.push_back(make_array(fixed_row_b, {4, 5, 6}));
    fixed_entry.instructions.push_back(make_array(fixed_grid, {7, 8}));
    fixed_entry.terminator = vex::IRTerminator{vex::IRReturn{9}, {}};

    vex::IRFunction fixed_function;
    fixed_function.name = "fixed_grid";
    fixed_function.return_type = vex::IRType::Array;
    fixed_function.return_element_type = vex::IRType::Int;
    fixed_function.return_array_length = 2u;
    fixed_function.return_array_dimensions = {2u, 3u};
    fixed_function.entry_block = 0;
    fixed_function.blocks.push_back(std::move(fixed_entry));
    module.functions.push_back(std::move(fixed_function));

    vex::IRBlock dynamic_entry;
    dynamic_entry.id = 0;
    dynamic_entry.name = "entry";
    for (std::uint32_t index = 1; index <= 6; ++index) {
        const std::string spelling = std::to_string(index + 10u);
        dynamic_entry.instructions.push_back(
            constant(index, vex::IRType::Int, spelling.c_str()));
    }
    vex::IRValue dynamic_row_a = vex::ir_make_array_value(
        vex::IRType::Int, {3u});
    dynamic_row_a.id = 7;
    vex::IRValue dynamic_row_b = vex::ir_make_array_value(
        vex::IRType::Int, {3u});
    dynamic_row_b.id = 8;
    vex::IRValue dynamic_rows = vex::ir_make_array_value(
        vex::IRType::Int, {0u, 3u});
    dynamic_rows.id = 9;
    dynamic_entry.instructions.push_back(make_array(dynamic_row_a, {1, 2, 3}));
    dynamic_entry.instructions.push_back(make_array(dynamic_row_b, {4, 5, 6}));
    dynamic_entry.instructions.push_back(make_array(dynamic_rows, {7, 8}));
    dynamic_entry.terminator = vex::IRTerminator{vex::IRReturn{9}, {}};

    vex::IRFunction dynamic_function;
    dynamic_function.name = "dynamic_rows";
    dynamic_function.return_type = vex::IRType::Array;
    dynamic_function.return_element_type = vex::IRType::Int;
    dynamic_function.return_array_length = 0u;
    dynamic_function.return_array_dimensions = {0u, 3u};
    dynamic_function.entry_block = 0;
    dynamic_function.blocks.push_back(std::move(dynamic_entry));
    module.functions.push_back(std::move(dynamic_function));

    const auto verified = vex::IRVerifier{}.verify(module);
    tests.check(!verified.has_errors(),
                "IR verifier accepts fixed nested arrays and dynamic outer arrays of fixed rows");
    if (verified.has_errors()) return;

    const auto c = vex::CBackend{}.generate(module);
    tests.check(!c.has_errors() &&
                c.source.find("vex_array_int_3") != std::string::npos &&
                c.source.find("vex_array_int_2_3") != std::string::npos &&
                c.source.find("vex_array * vex_fn_dynamic_rows") != std::string::npos,
                "C backend emits recursive fixed wrappers and pointer-backed dynamic outer arrays");

    const auto llvm = vex::LLVMBackend{}.emit(module);
    tests.check(!llvm.has_errors() &&
                llvm.output.find("[2 x [3 x i64]]") != std::string::npos &&
                llvm.output.find("define internal ptr @vex_fn_dynamic_rows") != std::string::npos &&
                llvm.output.find("[3 x i64]") != std::string::npos,
                "LLVM backend emits nested fixed aggregates and dynamic outer pointer representation");
}

void test_byte_lowering_and_backends(TestContext& tests) {
    const vex::SourceFile source{"byte_ir.vex",
        "make flip(value: byte) -> byte { return ~value }\n"
        "make main() {\n"
        "    let a = to_byte(170)\n"
        "    let b = to_byte(255)\n"
        "    let x = a & b\n"
        "    let y = a | b\n"
        "    let z = a ^ b\n"
        "    let inverted = flip(a)\n"
        "    say(to_int(x))\n"
        "    say(to_int(y))\n"
        "    say(to_int(z))\n"
        "    say(to_int(inverted))\n"
        "    say(a < b)\n"
        "}\n"};
    const auto lexed = vex::Lexer(source).scan();
    const auto parsed = vex::Parser(source, lexed.tokens).parse();
    const auto semantic = vex::SemanticAnalyzer(source, parsed.program).analyze();
    tests.check(!lexed.has_errors() && !parsed.has_errors() && !semantic.has_errors(),
                "byte source passes lexing, parsing, and semantic analysis");
    if (lexed.has_errors() || parsed.has_errors() || semantic.has_errors()) return;

    const auto lowered = vex::IRLowerer(source, parsed.program, semantic.model).lower();
    tests.check(!lowered.has_errors(), "byte values and conversions lower into typed IR");
    if (lowered.has_errors()) return;

    std::size_t checked_narrowing = 0, widening = 0, byte_bitwise = 0;
    for (const auto& function : lowered.module.functions)
        for (const auto& block : function.blocks)
            for (const auto& instruction : block.instructions) {
                checked_narrowing += instruction.op == vex::IROp::IntToByteChecked ? 1u : 0u;
                widening += instruction.op == vex::IROp::ByteToInt ? 1u : 0u;
                if (instruction.op == vex::IROp::BitwiseAnd ||
                    instruction.op == vex::IROp::BitwiseOr ||
                    instruction.op == vex::IROp::BitwiseXor ||
                    instruction.op == vex::IROp::BitwiseNot) {
                    if (instruction.result && instruction.result->type == vex::IRType::Byte)
                        ++byte_bitwise;
                }
            }
    tests.check(checked_narrowing == 2 && widening == 4 && byte_bitwise == 4,
                "IR preserves byte width and represents checked and widening conversions");
    tests.check(!vex::IRVerifier{}.verify(lowered.module).has_errors(),
                "byte operations pass independent IR verification");

    const auto c = vex::CBackend{}.generate(lowered.module);
    tests.check(!c.has_errors() &&
                c.source.find("uint8_t") != std::string::npos &&
                c.source.find("vex_checked_byte") != std::string::npos &&
                c.source.find("vex_say_byte") != std::string::npos,
                "C backend emits uint8_t storage, checked narrowing, and byte output");
    const auto llvm = vex::LLVMBackend{}.emit(lowered.module);
    tests.check(!llvm.has_errors() &&
                llvm.output.find("call i8 @vex_int_to_byte") != std::string::npos &&
                llvm.output.find("zext i8") != std::string::npos &&
                llvm.output.find(" = and i8 ") != std::string::npos &&
                llvm.output.find("icmp ult i8") != std::string::npos,
                "LLVM backend emits i8 operations and unsigned byte comparisons");
}


void test_source_enum_declarations_lower_to_ir(TestContext& tests) {
    const vex::SourceFile source{"enum_ir.vex",
        "struct Point { x: int, y: float }\n"
        "enum Event { Started, Moved(Point), Bytes(byte[3]), Failed(string) }\n"
        "make main() { }\n"};
    const vex::LexResult lexed = vex::Lexer(source).scan();
    const vex::ParseResult parsed = vex::Parser(source, lexed.tokens).parse();
    const vex::SemanticResult semantic = vex::SemanticAnalyzer(source, parsed.program).analyze();
    const vex::IRLoweringResult lowered = vex::IRLowerer(
        source, parsed.program, semantic.model).lower();
    tests.check(!lexed.has_errors() && !parsed.has_errors() &&
                !semantic.has_errors() && !lowered.has_errors() &&
                lowered.module.enum_types.size() == 1u,
                "validated source enums lower into the canonical IR type table");
    if (lowered.has_errors() || lowered.module.enum_types.empty()) return;
    const auto& event = lowered.module.enum_types.front();
    tests.check(event.id == 1u && event.name == "Event" && event.variants.size() == 4u &&
                event.variants[1].fields.size() == 1u &&
                event.variants[1].fields[0].type == vex::IRType::Struct &&
                event.variants[1].fields[0].struct_type_id == 1u &&
                event.variants[2].fields[0].type == vex::IRType::Array &&
                event.variants[2].fields[0].array_dimensions == std::vector<std::uint32_t>{3u},
                "IR enum payload descriptors preserve structure identity and array shape");
    tests.check(!vex::IRVerifier{}.verify(lowered.module).has_errors(),
                "enum declarations pass the independent IR verifier");
    const auto c = vex::CBackend{}.generate(lowered.module);
    const auto llvm = vex::LLVMBackend{}.emit(lowered.module);
    tests.check(!c.has_errors() && !llvm.has_errors(),
                "both backends accept declared enum types even before enum values are used");
}

void test_target_data_layout_service(TestContext& tests) {
    vex::IRModule module;
    vex::IRStructType record;
    record.id = 1u;
    record.name = "LayoutRecord";
    record.fields.push_back(vex::IRStructField{"flag", vex::IRType::Byte});
    record.fields.push_back(vex::IRStructField{"count", vex::IRType::Int});

    vex::IRStructField bytes{"bytes", vex::IRType::Array};
    bytes.element_type = vex::IRType::Byte;
    bytes.array_length = 3u;
    bytes.array_dimensions = {3u};
    record.fields.push_back(std::move(bytes));
    record.fields.push_back(vex::IRStructField{"label", vex::IRType::String});

    vex::IRStructField samples{"samples", vex::IRType::Array};
    samples.element_type = vex::IRType::Int;
    samples.array_dimensions = {0u};
    record.fields.push_back(std::move(samples));
    module.struct_types.push_back(std::move(record));

    const vex::TargetDataLayout layout;
    const auto structure = layout.layout_of_struct(1u, module);
    tests.check(structure.has_value(),
                "target layout computes scalar, fixed-array, string and dynamic-array structure fields");
    if (!structure) return;
    tests.check(structure->alignment == 8u && structure->size == 48u,
                "64-bit structure layout applies natural alignment and tail padding");
    const std::vector<std::uint64_t> offsets{0u, 8u, 16u, 24u, 40u};
    bool match = structure->fields.size() == offsets.size();
    for (std::size_t i = 0; match && i < offsets.size(); ++i)
        match = structure->fields[i].offset == offsets[i];
    tests.check(match, "target layout reports canonical structure field offsets");

    const auto fixed = layout.layout_of(vex::ir_make_array_value(
        vex::IRType::Int, {2u, 3u}), module);
    tests.check(fixed && fixed->size == 48u && fixed->alignment == 8u &&
                fixed->stride == 48u,
                "target layout recursively computes inline nested fixed-array size and stride");

    const auto dynamic = layout.layout_of(vex::ir_make_array_value(
        vex::IRType::Int, {0u, 3u}), module);
    tests.check(dynamic && dynamic->size == 8u && dynamic->alignment == 8u,
                "dynamic outer arrays use pointer-sized value representation");
    const auto dynamic_element = layout.layout_of_array_element(vex::ir_make_array_value(
        vex::IRType::Int, {0u, 3u}), module);
    tests.check(dynamic_element && dynamic_element->size == 24u &&
                dynamic_element->stride == 24u && dynamic_element->alignment == 8u,
                "target layout exposes the true stride of a dynamic array's fixed-row element");

    vex::IRValue string_value;
    string_value.type = vex::IRType::String;
    const auto string_layout = layout.layout_of(string_value, module);
    tests.check(string_layout && string_layout->size == 16u &&
                string_layout->alignment == 8u,
                "string descriptor layout is pointer plus 64-bit byte length");

    vex::TargetLayoutConfig narrow_config;
    narrow_config.pointer_size = 4u;
    narrow_config.pointer_alignment = 4u;
    const vex::TargetDataLayout narrow_layout(narrow_config);
    const auto narrow_pointer = narrow_layout.layout_of(vex::ir_make_array_value(
        vex::IRType::Int, {0u}), module);
    const auto narrow_string = narrow_layout.layout_of(string_value, module);
    tests.check(narrow_pointer && narrow_pointer->size == 4u &&
                narrow_string && narrow_string->size == 16u,
                "layout separates pointer width from the fixed 64-bit string length field");

    vex::TargetLayoutConfig bounded_config;
    bounded_config.maximum_object_size = 32u;
    const vex::TargetDataLayout bounded_layout(bounded_config);
    std::string error;
    const auto oversized = bounded_layout.layout_of(vex::ir_make_array_value(
        vex::IRType::Int, {8u}), module, &error);
    tests.check(!oversized && !error.empty(),
                "target layout rejects objects exceeding the configured size limit");

    error.clear();
    const auto invalid = layout.layout_of_struct(9u, module, &error);
    tests.check(!invalid && !error.empty(),
                "target layout diagnoses unknown structure identifiers");

    vex::IREnumType event;
    event.id = 1u;
    event.name = "Event";
    event.variants.push_back(vex::IREnumVariant{"Stopped", {}});
    vex::IREnumVariant moving;
    moving.name = "Moving";
    moving.fields.push_back(vex::IRStructField{"code", vex::IRType::Byte});
    moving.fields.push_back(vex::IRStructField{"distance", vex::IRType::Int});
    event.variants.push_back(std::move(moving));
    vex::IREnumVariant text;
    text.name = "Text";
    text.fields.push_back(vex::IRStructField{"contents", vex::IRType::String});
    event.variants.push_back(std::move(text));
    module.enum_types.push_back(std::move(event));

    const auto event_layout = layout.layout_of_enum(1u, module);
    tests.check(event_layout && event_layout->discriminant_size == 4u &&
                event_layout->discriminant_alignment == 4u &&
                event_layout->payload_offset == 8u &&
                event_layout->payload_size == 16u &&
                event_layout->payload_alignment == 8u &&
                event_layout->size == 24u && event_layout->alignment == 8u,
                "enum layout computes a fixed tag and naturally aligned maximum variant payload");
    tests.check(event_layout && event_layout->variants.size() == 3u &&
                event_layout->variants[0].tag == 0u &&
                event_layout->variants[1].tag == 1u &&
                event_layout->variants[1].fields.size() == 2u &&
                event_layout->variants[1].fields[1].offset == 8u,
                "enum layout preserves declaration-order tags and per-variant payload offsets");

    vex::IREnumType unit_enum;
    unit_enum.id = 2u;
    unit_enum.name = "Direction";
    unit_enum.variants = {{"North", {}}, {"South", {}}, {"East", {}}, {"West", {}}};
    module.enum_types.push_back(std::move(unit_enum));
    const auto unit_layout = layout.layout_of_enum(2u, module);
    tests.check(unit_layout && unit_layout->size == 4u &&
                unit_layout->alignment == 4u && unit_layout->payload_offset == 4u &&
                unit_layout->payload_size == 0u,
                "payload-free enums are compact 32-bit discriminant values");

    vex::IREnumType duplicate_enum;
    duplicate_enum.id = 3u;
    duplicate_enum.name = "Invalid";
    duplicate_enum.variants = {{"Same", {}}, {"Same", {}}};
    module.enum_types.push_back(std::move(duplicate_enum));
    error.clear();
    tests.check(!layout.layout_of_enum(3u, module, &error) && !error.empty(),
                "target layout rejects duplicate enum variant names");
    const auto enum_verification = vex::IRVerifier{}.verify(module);
    tests.check(has_code(enum_verification, "IR083"),
                "independent IR verifier rejects duplicate enum variants");
    module.enum_types.pop_back();

    vex::IRValue record_parameter;
    record_parameter.id = 1u;
    record_parameter.type = vex::IRType::Struct;
    record_parameter.struct_type_id = 1u;
    vex::IRBlock consume_entry;
    consume_entry.id = 0u;
    consume_entry.name = "entry";
    consume_entry.terminator = vex::IRTerminator{vex::IRReturn{std::nullopt}, {}};
    vex::IRFunction consume;
    consume.name = "consume_layout_record";
    consume.parameters.push_back(vex::IRFunctionParameter{"record", record_parameter});
    consume.blocks.push_back(std::move(consume_entry));
    module.functions.push_back(std::move(consume));

    vex::IRBlock main_entry;
    main_entry.id = 0u;
    main_entry.name = "entry";
    main_entry.terminator = vex::IRTerminator{vex::IRReturn{std::nullopt}, {}};
    vex::IRFunction main;
    main.name = "main";
    main.blocks.push_back(std::move(main_entry));
    module.functions.push_back(std::move(main));

    const auto verified = vex::IRVerifier{}.verify(module);
    tests.check(!verified.has_errors(),
                "layout conformance module passes independent IR verification");
    if (verified.has_errors()) return;
    const auto c = vex::CBackend{}.generate(module);
    tests.check(!c.has_errors() &&
                c.source.find("_Static_assert(sizeof(vex_struct_1) == 48u") != std::string::npos &&
                c.source.find("offsetof(vex_struct_1, vex_field_1) == 8u") != std::string::npos,
                "C backend asserts target structure size and field offsets");
    const auto llvm = vex::LLVMBackend{}.emit(module);
    tests.check(!llvm.has_errors() &&
                llvm.output.find("alloca %vex.struct.1, align 8") != std::string::npos,
                "LLVM backend consumes target layout for structure stack-slot alignment");
}

void test_end_to_end_enum_construction_and_matching(TestContext& tests) {
    const vex::SourceFile source{"enum_pipeline.vex",
        "enum Result { Success(int), Failure(string), Cancelled }\n"
        "make create() -> Result {\n"
        "    return Result.Success(42)\n"
        "}\n"
        "make get_value(result: Result) -> int {\n"
        "    let answer = 0\n"
        "    match result {\n"
        "        Result.Success(value) => { answer = value }\n"
        "        Result.Failure(reason) => { answer = 1 }\n"
        "        Result.Cancelled => { answer = 0 }\n"
        "    }\n"
        "    return answer\n"
        "}\n"
        "make main() {\n"
        "    let result = create()\n"
        "    let answer = get_value(result)\n"
        "    say(answer)\n"
        "}\n"};
    const vex::LexResult lexed = vex::Lexer{source}.scan();
    tests.check(!lexed.has_errors(), "enum pipeline source lexes");
    if (lexed.has_errors()) return;
    const vex::ParseResult parsed = vex::Parser{source, lexed.tokens}.parse();
    tests.check(!parsed.has_errors(), "enum constructors and exhaustive match parse");
    if (parsed.has_errors()) return;
    const vex::SemanticResult semantic =
        vex::SemanticAnalyzer{source, parsed.program}.analyze();
    tests.check(!semantic.has_errors(), "enum constructor and match semantics validate");
    if (semantic.has_errors()) return;
    const vex::IRLoweringResult lowered =
        vex::IRLowerer{source, parsed.program, semantic.model}.lower();
    tests.check(!lowered.has_errors(), "enum values and match arms lower to typed IR");
    if (lowered.has_errors()) return;
    const vex::IRVerificationResult verified =
        vex::IRVerifier{}.verify(lowered.module);
    tests.check(!verified.has_errors(),
                "IR verifier accepts enum construction, tag tests and payload projections");

    std::size_t constructions = 0u;
    std::size_t tag_reads = 0u;
    std::size_t payload_reads = 0u;
    for (const vex::IRFunction& function : lowered.module.functions) {
        for (const vex::IRBlock& block : function.blocks) {
            for (const vex::IRInstruction& instruction : block.instructions) {
                if (instruction.op == vex::IROp::EnumMake) ++constructions;
                if (instruction.op == vex::IROp::EnumTag) ++tag_reads;
                if (instruction.op == vex::IROp::EnumPayloadGet) ++payload_reads;
            }
        }
    }
    tests.check(constructions >= 1u && tag_reads >= 1u && payload_reads >= 1u,
                "lowered enum pipeline contains constructor, discriminant and payload operations");

    const vex::CBackendResult c_output = vex::CBackend{}.generate(lowered.module);
    tests.check(!c_output.has_errors() && !c_output.source.empty(),
                "C backend emits enum constructors and match payload access");
    const vex::BackendResult llvm_output = vex::LLVMBackend{}.emit(lowered.module);
    tests.check(!llvm_output.has_errors() && !llvm_output.output.empty(),
                "LLVM backend emits enum constructors and match payload access");
}

void test_nominal_type_and_function_names_share_namespace(TestContext& tests) {
    vex::IRModule module;

    vex::IRStructType record;
    record.id = 1u;
    record.name = "Record";
    record.fields.push_back(vex::IRStructField{"value", vex::IRType::Int});
    module.struct_types.push_back(std::move(record));

    vex::IREnumType state;
    state.id = 1u;
    state.name = "State";
    state.variants.push_back(vex::IREnumVariant{"Ready", {}});
    module.enum_types.push_back(std::move(state));

    vex::IRFunction function = simple_integer_function();
    function.name = "run";
    module.functions.push_back(std::move(function));

    const auto valid = vex::IRVerifier{}.verify(module);
    tests.check(!valid.has_errors(),
                "distinct struct, enum, and function names pass IR verification");

    vex::IRModule duplicate_nominal_name = module;
    duplicate_nominal_name.enum_types[0].name = "Record";
    const auto duplicate_type = vex::IRVerifier{}.verify(duplicate_nominal_name);
    tests.check(has_code(duplicate_type, "IR082"),
                "IR verifier rejects an enum whose name collides with a structure");

    vex::IRModule function_matches_enum = module;
    function_matches_enum.functions[0].name = "State";
    const auto enum_function = vex::IRVerifier{}.verify(function_matches_enum);
    tests.check(has_code(enum_function, "IR085"),
                "IR verifier rejects a function whose name collides with an enum");

    vex::IRModule function_matches_struct = module;
    function_matches_struct.functions[0].name = "Record";
    const auto struct_function = vex::IRVerifier{}.verify(function_matches_struct);
    tests.check(has_code(struct_function, "IR085"),
                "IR verifier rejects a function whose name collides with a structure");
}

} // namespace

void run_ir_tests(TestContext& tests) {
    test_source_to_verified_ir_lowering(tests);
    test_fixed_array_lowering_and_backends(tests);
    test_struct_lowering_and_backend_aggregates(tests);
    test_composed_structure_arrays_and_recursive_dynamic_fields(tests);
    test_implicit_and_explicit_entry_points(tests);
    test_valid_typed_function(tests);
    test_block_arguments_and_control_flow(tests);
    test_invalid_ir_is_rejected(tests);
    test_dominance_and_call_signatures(tests);
    test_dynamic_array_capacity_lowering_and_backends(tests);
    test_string_byte_length_lowering_and_backends(tests);
    test_string_join_lowering_and_backends(tests);
    test_bitwise_ir_lowering_and_backends(tests);
    test_default_initialized_scalar_lowering_and_backends(tests);
    test_recursive_array_ir_shapes_and_backends(tests);
    test_byte_lowering_and_backends(tests);
    test_source_enum_declarations_lower_to_ir(tests);
    test_target_data_layout_service(tests);
    test_nominal_type_and_function_names_share_namespace(tests);
    test_end_to_end_enum_construction_and_matching(tests);
}
