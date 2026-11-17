#pragma once

#include "vex/diagnostic.hpp"

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace vex {

using IRValueId = std::uint32_t;
using IRBlockId = std::uint32_t;

enum class IRType {
    Void,
    Int,
    Byte,
    Float,
    Bool,
    String,
    Array,
    Struct,
    Enum
};

[[nodiscard]] const char* ir_type_name(IRType type) noexcept;

enum class IROp {
    Constant,
    Add,
    StringConcat,
    StringLength,
    StringJoin,
    ByteSlice,
    ByteRead,
    ByteWrite,
    FileRead,
    FileWrite,
    Subtract,
    Multiply,
    Divide,
    Remainder,
    Negate,
    LogicalNot,
    LogicalAnd,
    LogicalOr,
    BitwiseAnd,
    BitwiseOr,
    BitwiseXor,
    BitwiseNot,
    ShiftLeft,
    ShiftRight,
    RotateLeft,
    RotateRight,
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    IntToFloat,
    IntToByteChecked,
    ByteToInt,
    Call,
    Math,
    Say,
    ArrayMake,
    ArrayGet,
    ArraySet,
    ArrayLength,
    ArrayCapacity,
    ArrayReserve,
    ArrayPush,
    ArrayPop,
    StructMake,
    StructGet,
    StructSet,
    EnumMake,
    EnumTag,
    EnumPayloadGet
};

[[nodiscard]] const char* ir_op_name(IROp op) noexcept;

struct IRValue {
    IRValueId id{0};
    IRType type{IRType::Void};
    SourceSpan span{};
    // Arrays store their scalar/nominal leaf type plus every outermost-first
    // dimension. A zero dimension is dynamic; fixed dimensions are positive.
    IRType element_type{IRType::Void};
    std::uint32_t array_length{0}; // compatibility mirror of the outer dimension
    std::uint32_t struct_type_id{0};
    std::uint32_t element_struct_type_id{0};
    std::vector<std::uint32_t> array_dimensions;
    // Nominal identity when type is Enum.
    std::uint32_t enum_type_id{0};
};

// Older IR construction sites may only populate array_length. Treat that as
// a one-dimensional descriptor; new code should always retain the full vector.
[[nodiscard]] inline std::vector<std::uint32_t> ir_array_dimensions(
    const IRValue& value) {
    return value.array_dimensions.empty()
        ? std::vector<std::uint32_t>{value.array_length}
        : value.array_dimensions;
}

[[nodiscard]] inline IRValue ir_make_array_value(
    const IRType leaf_type, std::vector<std::uint32_t> dimensions,
    const std::uint32_t leaf_struct_type_id = 0, const SourceSpan span = {},
    const std::uint32_t leaf_enum_type_id = 0) {
    IRValue value;
    value.type = IRType::Array;
    value.span = span;
    value.element_type = leaf_type;
    value.array_length = dimensions.empty() ? 0u : dimensions.front();
    value.element_struct_type_id = leaf_struct_type_id;
    value.enum_type_id = leaf_enum_type_id;
    value.array_dimensions = std::move(dimensions);
    return value;
}

// Produces the immediate element type. For T[a][b], this is T[b]; for T[a],
// it is T. The fixed/dynamic representation of the remaining dimensions is
// preserved exactly.
[[nodiscard]] inline IRValue ir_array_element_value(
    const IRValue& array, const SourceSpan span = {}) {
    const std::vector<std::uint32_t> dimensions = ir_array_dimensions(array);
    if (dimensions.size() > 1u) {
        return ir_make_array_value(
            array.element_type,
            std::vector<std::uint32_t>(dimensions.begin() + 1, dimensions.end()),
            array.element_struct_type_id, span, array.enum_type_id);
    }
    IRValue value;
    value.type = array.element_type;
    value.span = span;
    value.struct_type_id = array.element_struct_type_id;
    value.enum_type_id = array.enum_type_id;
    return value;
}

[[nodiscard]] inline bool ir_same_array_shape(
    const IRValue& left, const IRValue& right) {
    return left.type == IRType::Array && right.type == IRType::Array &&
           left.element_type == right.element_type &&
           left.element_struct_type_id == right.element_struct_type_id &&
           left.enum_type_id == right.enum_type_id &&
           ir_array_dimensions(left) == ir_array_dimensions(right);
}

struct IRStructField {
    std::string name;
    IRType type{IRType::Void};
    std::uint32_t struct_type_id{0};
    IRType element_type{IRType::Void};
    std::uint32_t array_length{0};
    std::uint32_t element_struct_type_id{0};
    std::vector<std::uint32_t> array_dimensions;
    std::uint32_t enum_type_id{0};
};

[[nodiscard]] inline IRValue ir_struct_field_value(const IRStructField& field) {
    IRValue value;
    value.type = field.type;
    value.struct_type_id = field.struct_type_id;
    value.element_type = field.element_type;
    value.array_length = field.array_length;
    value.element_struct_type_id = field.element_struct_type_id;
    value.array_dimensions = field.array_dimensions;
    value.enum_type_id = field.enum_type_id;
    return value;
}

struct IRStructType {
    std::uint32_t id{0};
    std::string name;
    std::vector<IRStructField> fields;
};

struct IREnumVariant {
    std::string name;
    std::vector<IRStructField> fields;
};

struct IREnumType {
    std::uint32_t id{0};
    std::string name;
    std::vector<IREnumVariant> variants;
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
    IRType return_element_type{IRType::Void};
    std::uint32_t return_array_length{0};
    std::uint32_t return_struct_type_id{0};
    std::uint32_t return_enum_type_id{0};
    std::uint32_t return_element_struct_type_id{0};
    std::vector<std::uint32_t> return_array_dimensions;
    std::vector<IRBlock> blocks;
    IRBlockId entry_block{0};
    SourceSpan span{};
};

struct IRModule {
    std::vector<IRStructType> struct_types;
    std::vector<IRFunction> functions;
    // Nominal enum declarations; kept last to preserve older aggregate initializers.
    std::vector<IREnumType> enum_types;
};

[[nodiscard]] bool ir_value_has_nested_fixed_storage(
    const IRValue& value, const IRModule& module);
[[nodiscard]] std::vector<IRValue> ir_function_nested_fixed_stack_values(
    const IRFunction& function, const IRModule& module);

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
