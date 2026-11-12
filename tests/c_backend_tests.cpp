#include "test_support.hpp"

#include "vex/c_backend.hpp"

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

void test_c_backend_emits_typed_c(TestContext& tests) {
    const vex::CBackendResult result = vex::CBackend{}.generate(simple_main_module());
    tests.check(!result.has_errors(), "valid typed IR is accepted by the C backend");
    tests.check(result.source.find("int main(void)") != std::string::npos,
                "C backend emits the host C entry-point wrapper");
    tests.check(result.source.find("INT64_C(42)") != std::string::npos,
                "C backend emits a portable fixed-width integer constant");
    tests.check(result.source.find("vex_say_int(vex_v_1)") != std::string::npos,
                "C backend maps the say intrinsic to its runtime helper");
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
    test_c_backend_rejects_invalid_entry_point(tests);
    test_c_backend_requires_verified_ir(tests);
}
