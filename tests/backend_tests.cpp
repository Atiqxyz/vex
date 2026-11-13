#include "test_support.hpp"

#include "vex/backend.hpp"
#include "vex/c_backend.hpp"

#include <string_view>

namespace {

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

void test_factory_registers_c_backend(TestContext& tests) {
    const std::unique_ptr<vex::Backend> backend =
        vex::create_backend(vex::BackendKind::C);
    tests.check(backend != nullptr,
                "backend factory creates the registered C backend");
    if (!backend) return;

    tests.check(backend->name() == std::string_view{"c"},
                "C backend identifies itself through the common interface");

    const vex::BackendResult result = backend->emit(simple_main_module());
    tests.check(!result.has_errors(),
                "backend interface accepts valid verified IR");
    tests.check(result.output.find("int main(void)") != std::string::npos,
                "backend interface returns generated C source");
    tests.check(result.output.find("INT64_C(42)") != std::string::npos,
                "backend interface preserves typed C constant emission");
}

void test_factory_rejects_unregistered_backend(TestContext& tests) {
    const auto unknown = static_cast<vex::BackendKind>(255);
    const std::unique_ptr<vex::Backend> backend = vex::create_backend(unknown);
    tests.check(backend == nullptr,
                "backend factory returns null for an unregistered backend");
}

void test_c_backend_compatibility_api(TestContext& tests) {
    const vex::CBackendResult legacy = vex::CBackend{}.generate(simple_main_module());
    const std::unique_ptr<vex::Backend> backend =
        vex::create_backend(vex::BackendKind::C);
    if (!backend) {
        tests.check(false, "C backend is available for compatibility comparison");
        return;
    }

    const vex::BackendResult generic = backend->emit(simple_main_module());
    tests.check(generic.output == legacy.source,
                "backend-neutral output matches the retained C-specific API");
    tests.check(generic.diagnostics.size() == legacy.diagnostics.size(),
                "backend-neutral diagnostics match the retained C-specific API");
}

} // namespace

void run_backend_tests(TestContext& tests) {
    test_factory_registers_c_backend(tests);
    test_factory_rejects_unregistered_backend(tests);
    test_c_backend_compatibility_api(tests);
}
