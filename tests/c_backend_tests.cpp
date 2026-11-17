#include "test_support.hpp"

#include "vex/c_backend.hpp"

#include <string>
#include <string_view>

namespace {

bool has_code(const vex::CBackendResult& result, const std::string_view code) {
    for (const vex::Diagnostic& diagnostic : result.diagnostics) {
        if (diagnostic.code == code) return true;
    }
    return false;
}

vex::IRValue value(const vex::IRValueId id, const vex::IRType type) {
    return vex::IRValue{id, type, {}};
}

vex::IRModule nested_fixed_stack_module() {
    vex::IRFunction worker;
    worker.name = "worker";
    worker.return_type = vex::IRType::Void;
    worker.entry_block = 0;
    for (vex::IRValueId id = 1; id <= 3; ++id) {
        vex::IRValue aggregate = vex::ir_make_array_value(vex::IRType::Int, {256u, 128u});
        aggregate.id = id;
        worker.parameters.push_back({"matrix" + std::to_string(id), aggregate});
    }
    vex::IRBlock entry;
    entry.id = 0;
    entry.name = "entry";
    entry.terminator = vex::IRTerminator{vex::IRReturn{}, {}};
    worker.blocks.push_back(entry);
    vex::IRModule module;
    module.functions.push_back(worker);
    return module;
}

vex::IRModule simple_main_module() {
    vex::IRBlock entry;
    entry.id = 0;
    entry.name = "entry";

    vex::IRInstruction literal;
    literal.op = vex::IROp::Constant;
    literal.result = value(1, vex::IRType::Int);
    literal.immediate = "42";
    entry.instructions.push_back(literal);

    vex::IRInstruction say;
    say.op = vex::IROp::Say;
    say.operands = {1};
    entry.instructions.push_back(say);
    entry.terminator = vex::IRTerminator{vex::IRReturn{}, {}};

    vex::IRFunction main;
    main.name = "main";
    main.return_type = vex::IRType::Void;
    main.entry_block = 0;
    main.blocks.push_back(entry);

    vex::IRModule module;
    module.functions.push_back(main);
    return module;
}

vex::IRModule integer_main_module() {
    vex::IRModule module = simple_main_module();
    module.functions.front().return_type = vex::IRType::Int;
    module.functions.front().blocks.front().terminator =
        vex::IRTerminator{vex::IRReturn{1}, {}};
    return module;
}

void test_c_backend_emits_typed_c(TestContext& tests) {
    const vex::CBackendResult result = vex::CBackend{}.generate(simple_main_module());
    tests.check(!result.has_errors(), "valid typed IR is accepted by the C backend");
    tests.check(result.source.find("int main(void)") != std::string::npos,
                "C backend emits the host C entry-point wrapper");
    tests.check(result.source.find("INT64_C(42)") != std::string::npos,
                "C backend emits a portable fixed-width integer constant");
    tests.check(result.source.find("vex_say_int(vex_v_1)") != std::string::npos,
                "C backend maps the say intrinsic to its runtime helper");
    const auto concat_start = result.source.find("static vex_string vex_string_concat");
    const auto concat_end = result.source.find("static vex_string vex_string_join", concat_start);
    const bool has_concat_helper = concat_start != std::string::npos &&
                                   concat_end != std::string::npos;
    tests.check(has_concat_helper, "C backend emits its string concatenation runtime helper");
    if (has_concat_helper) {
        const std::string_view helper{result.source.data() + concat_start, concat_end - concat_start};
        const auto first_malloc = helper.find("malloc(");
        tests.check(first_malloc != std::string_view::npos &&
                    helper.find("malloc(", first_malloc + 1) == std::string_view::npos,
                    "C string concatenation uses one allocation for metadata and bytes");
        tests.check(helper.find("if (left.length == 0u) return right;") != std::string_view::npos &&
                    helper.find("if (right.length == 0u) return left;") != std::string_view::npos,
                    "C string concatenation returns the non-empty operand without allocating");
    }
}

void test_c_backend_emits_nested_fixed_stack_budget_assertions(TestContext& tests) {
    const vex::CBackendResult result = vex::CBackend{}.generate(nested_fixed_stack_module());
    tests.check(!result.has_errors(), "C backend accepts nested fixed aggregate IR for target-layout checks");
    tests.check(result.source.find("sizeof(vex_array_int_256_128) <= 262144u") != std::string::npos,
                "C backend emits target-native sizeof assertions for nested fixed aggregate objects");
    tests.check(result.source.find("512 KiB nested fixed aggregate stack budget") != std::string::npos,
                "C backend emits a per-function nested fixed aggregate storage assertion");
}

void test_c_backend_checks_allocation_failures_and_exit_cleanup(TestContext& tests) {
    const vex::CBackendResult result = vex::CBackend{}.generate(integer_main_module());
    tests.check(!result.has_errors(), "C backend accepts an integer-returning entry for cleanup verification");
    tests.check(result.source.find("dynamic array allocation failed") != std::string::npos &&
                result.source.find("dynamic array reservation failed") != std::string::npos &&
                result.source.find("dynamic array growth failed") != std::string::npos &&
                result.source.find("string allocation failed") != std::string::npos &&
                result.source.find("string join allocation failed") != std::string::npos,
                "C runtime checks array, growth, reservation, concatenation, and join allocation failures");
    tests.check(result.source.find("free(array->data);") != std::string::npos &&
                result.source.find("free(array);") != std::string::npos &&
                result.source.find("free(allocation);") != std::string::npos,
                "C runtime releases tracked array buffers, headers, and string allocations");
    const std::size_t entry = result.source.find("int main(void)");
    const std::size_t arrays = result.source.find("vex_array_cleanup();", entry);
    const std::size_t strings = result.source.find("vex_string_cleanup();", arrays);
    const std::size_t invalid_status = result.source.find("vex_status < 0", strings);
    tests.check(entry != std::string::npos && arrays > entry && strings > arrays &&
                invalid_status > strings,
                "C entry wrapper cleans tracked allocations before rejecting an invalid exit status");
}

void test_c_backend_rejects_invalid_entry_point(TestContext& tests) {
    vex::IRModule module = simple_main_module();
    module.functions.front().parameters.push_back(
        {"argument", value(2, vex::IRType::Int)});

    const vex::CBackendResult result = vex::CBackend{}.generate(module);
    tests.check(has_code(result, "CG001"),
                "C backend diagnoses unsupported main parameters");
    tests.check(result.source.empty(),
                "C backend does not return partial source after a code-generation error");
}

void test_c_backend_requires_verified_ir(TestContext& tests) {
    vex::IRFunction function;
    function.name = "main";
    function.return_type = vex::IRType::Void;
    function.entry_block = 0;
    vex::IRBlock block;
    block.id = 0;
    function.blocks.push_back(block);

    vex::IRModule module;
    module.functions.push_back(function);
    const vex::CBackendResult result = vex::CBackend{}.generate(module);
    tests.check(has_code(result, "IR028"),
                "C backend refuses IR that fails the independent verifier");
    tests.check(result.source.empty(),
                "invalid IR cannot produce C source");
}

} // namespace

void run_c_backend_tests(TestContext& tests) {
    test_c_backend_emits_typed_c(tests);
    test_c_backend_emits_nested_fixed_stack_budget_assertions(tests);
    test_c_backend_checks_allocation_failures_and_exit_cleanup(tests);
    test_c_backend_rejects_invalid_entry_point(tests);
    test_c_backend_requires_verified_ir(tests);
}
