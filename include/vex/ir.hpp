#pragma once

#include "vex/diagnostic.hpp"

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace vex {

using IRValueId = std::uint32_t;
using IRBlockId = std::uint32_t;

enum class IRType {
    Void,
    Int,
    Float,
    Bool,
    String,
    Array
};

[[nodiscard]] const char* ir_type_name(IRType type) noexcept;

enum class IROp {
    Constant,
    Add,
    Subtract,
    Multiply,
    Divide,
    Remainder,
    Negate,
    LogicalNot,
    LogicalAnd,
    LogicalOr,
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    IntToFloat,
    Call,
    Math,
    Say,
    ArrayMake,
    ArrayGet,
    ArraySet
};

[[nodiscard]] const char* ir_op_name(IROp op) noexcept;

struct IRValue {
    IRValueId id{0};
    IRType type{IRType::Void};
    SourceSpan span{};
    IRType element_type{IRType::Void};
    std::uint32_t array_length{0};
};

struct IRInstruction {
    IROp op{IROp::Constant};
    std::optional<IRValue> result;
    std::vector<IRValueId> operands;
    // Constants retain their source spelling; Call uses callee.
    std::string immediate;
    std::string callee;
    SourceSpan span{};
};

struct IRReturn {
    std::optional<IRValueId> value;
};

struct IRBranch {
    IRBlockId target{0};
    std::vector<IRValueId> arguments;
};

struct IRConditionalBranch {
    IRValueId condition{0};
    IRBlockId true_target{0};
    std::vector<IRValueId> true_arguments;
    IRBlockId false_target{0};
    std::vector<IRValueId> false_arguments;
};

struct IRUnreachable {};

using IRTerminatorData =
    std::variant<IRReturn, IRBranch, IRConditionalBranch, IRUnreachable>;

struct IRTerminator {
    IRTerminatorData data;
    SourceSpan span{};
};

struct IRBlock {
    IRBlockId id{0};
    std::string name;
    // Block parameters are SSA definitions populated by incoming branch arguments.
    std::vector<IRValue> parameters;
    std::vector<IRInstruction> instructions;
    std::optional<IRTerminator> terminator;
};

struct IRFunctionParameter {
    std::string name;
    IRValue value;
};

struct IRFunction {
    std::string name;
    std::vector<IRFunctionParameter> parameters;
    IRType return_type{IRType::Void};
    std::vector<IRBlock> blocks;
    IRBlockId entry_block{0};
    SourceSpan span{};
};

struct IRModule {
    std::vector<IRFunction> functions;
};

struct IRVerificationResult {
    std::vector<Diagnostic> diagnostics;

    [[nodiscard]] bool has_errors() const noexcept;
};

class IRVerifier {
public:
    [[nodiscard]] IRVerificationResult verify(const IRModule& module) const;
};

void print_ir(std::ostream& output, const IRModule& module);

} // namespace vex
