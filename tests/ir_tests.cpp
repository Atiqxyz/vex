#include "test_support.hpp"

#include "vex/ir.hpp"
#include "vex/lexer.hpp"
#include "vex/lowering.hpp"
#include "vex/parser.hpp"
#include "vex/semantic.hpp"

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

} // namespace

void run_ir_tests(TestContext& tests) {
    test_source_to_verified_ir_lowering(tests);
    test_valid_typed_function(tests);
    test_block_arguments_and_control_flow(tests);
    test_invalid_ir_is_rejected(tests);
    test_dominance_and_call_signatures(tests);
}
