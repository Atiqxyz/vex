#include "vex/ir.hpp"
#include "vex/stack_policy.hpp"

#include <algorithm>
#include <ostream>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace vex {
namespace {

bool is_numeric(const IRType type) noexcept {
    return type == IRType::Int || type == IRType::Float;
}

bool is_value_type(const IRType type) noexcept {
    return type != IRType::Void;
}

struct FunctionSignature {
    std::vector<IRValue> parameters;
    IRType return_type{IRType::Void};
    IRType return_element_type{IRType::Void};
    std::uint32_t return_array_length{0};
    std::uint32_t return_struct_type_id{0};
    std::uint32_t return_enum_type_id{0};
    std::uint32_t return_element_struct_type_id{0};
    std::uint32_t return_element_enum_type_id{0};
    std::vector<std::uint32_t> return_array_dimensions;
    IRType return_pointer_pointee_type{IRType::Void};
    std::uint32_t return_pointer_pointee_struct_type_id{0};
    std::uint32_t return_pointer_pointee_enum_type_id{0};
};

bool valid_array_descriptor(const IRValue& value, const std::size_t struct_count,
                             const std::size_t enum_count) {
    if (value.type != IRType::Array || value.struct_type_id != 0 ||
        value.element_type == IRType::Void || value.element_type == IRType::Array) {
        return false;
    }
    const auto dimensions = ir_array_dimensions(value);
    if (dimensions.empty() || dimensions.size() > 64u ||
        value.array_length != dimensions.front()) return false;
    for (const std::uint32_t dimension : dimensions)
        if (dimension > 65536u) return false;
    std::uint64_t fixed_elements = 1u;
    std::size_t fixed_run = 0u;
    for (const std::uint32_t dimension : dimensions) {
        if (dimension == 0u) { fixed_elements = 1u; fixed_run = 0u; continue; }
        if (fixed_elements > max_nested_fixed_aggregate_bytes / dimension) return false;
        fixed_elements *= dimension;
        ++fixed_run;
        if (fixed_run >= 2u && fixed_elements > max_nested_fixed_aggregate_bytes) return false;
    }
    if (value.element_type == IRType::Struct)
        return value.element_struct_type_id != 0 &&
               value.element_struct_type_id <= struct_count &&
               value.enum_type_id == 0;
    if (value.element_type == IRType::Enum)
        return value.element_struct_type_id == 0 &&
               value.enum_type_id != 0 && value.enum_type_id <= enum_count;
    return value.element_struct_type_id == 0 && value.enum_type_id == 0;
}

bool valid_place_type(const IRValue& value, const std::size_t struct_count,
                      const std::size_t enum_count) {
    if (value.id != 0u || value.type == IRType::Void) return false;
    if (value.type != IRType::Pointer &&
        (value.pointer_pointee_type != IRType::Void ||
         value.pointer_pointee_struct_type_id != 0u ||
         value.pointer_pointee_enum_type_id != 0u)) return false;
    if (value.type == IRType::Array)
        return valid_array_descriptor(value, struct_count, enum_count);
    if (value.type == IRType::Struct) {
        return value.struct_type_id != 0u && value.struct_type_id <= struct_count &&
               value.element_type == IRType::Void && value.array_length == 0u &&
               value.element_struct_type_id == 0u && value.enum_type_id == 0u &&
               value.array_dimensions.empty();
    }
    if (value.type == IRType::Enum) {
        return value.enum_type_id != 0u && value.enum_type_id <= enum_count &&
               value.struct_type_id == 0u && value.element_type == IRType::Void &&
               value.array_length == 0u && value.element_struct_type_id == 0u &&
               value.array_dimensions.empty();
    }
    if (value.type == IRType::Pointer) {
        const bool primitive_pointee =
            (value.pointer_pointee_type == IRType::Int ||
             value.pointer_pointee_type == IRType::Byte ||
             value.pointer_pointee_type == IRType::Float ||
             value.pointer_pointee_type == IRType::Bool ||
             value.pointer_pointee_type == IRType::String) &&
            value.pointer_pointee_struct_type_id == 0u &&
            value.pointer_pointee_enum_type_id == 0u;
        const bool struct_pointee = value.pointer_pointee_type == IRType::Struct &&
            value.pointer_pointee_struct_type_id != 0u &&
            value.pointer_pointee_struct_type_id <= struct_count &&
            value.pointer_pointee_enum_type_id == 0u;
        const bool enum_pointee = value.pointer_pointee_type == IRType::Enum &&
            value.pointer_pointee_enum_type_id != 0u &&
            value.pointer_pointee_enum_type_id <= enum_count &&
            value.pointer_pointee_struct_type_id == 0u;
        return (primitive_pointee || struct_pointee || enum_pointee) &&
               value.struct_type_id == 0u && value.enum_type_id == 0u &&
               value.element_type == IRType::Void && value.array_length == 0u &&
               value.element_struct_type_id == 0u && value.array_dimensions.empty();
    }
    return (value.type == IRType::Int || value.type == IRType::Byte ||
            value.type == IRType::Float || value.type == IRType::Bool ||
            value.type == IRType::String) &&
           value.struct_type_id == 0u && value.enum_type_id == 0u &&
           value.element_type == IRType::Void && value.array_length == 0u &&
           value.element_struct_type_id == 0u && value.array_dimensions.empty() &&
           value.pointer_pointee_type == IRType::Void;
}

bool has_dynamic_outer_dimension(const IRValue& value) {
    const auto dimensions = ir_array_dimensions(value);
    return value.type == IRType::Array && !dimensions.empty() &&
           dimensions.front() == 0u;
}

bool is_simple_dynamic_array(const IRValue& value) {
    return has_dynamic_outer_dimension(value) &&
           ir_array_dimensions(value).size() == 1u;
}

IRValue function_array_return_value(const IRFunction& function) {
    auto dimensions = function.return_array_dimensions;
    if (dimensions.empty()) dimensions.push_back(function.return_array_length);
    return ir_make_array_value(function.return_element_type, std::move(dimensions),
                               function.return_element_struct_type_id, function.span,
                               function.return_element_enum_type_id);
}

struct Definition {
    IRValue value{};
    IRBlockId block{0};
    std::int64_t position{-1};
};

class FunctionVerifier {
public:
    FunctionVerifier(const IRFunction& function,
                     const std::unordered_map<std::string, FunctionSignature>& functions,
                     const std::vector<IRStructType>& struct_types,
                     const std::vector<IREnumType>& enum_types,
                     std::vector<Diagnostic>& diagnostics)
        : function_(function), functions_(functions), struct_types_(struct_types),
          enum_types_(enum_types), diagnostics_(diagnostics) {}

    void run() {
        index_blocks();
        index_places();
        collect_definitions();
        validate_blocks();
        validate_reachability_and_dominance();
    }

private:
    void report(const char* code, std::string message, const SourceSpan span) {
        diagnostics_.push_back(Diagnostic{
            DiagnosticSeverity::Error, code, std::move(message), span
        });
    }

    void index_blocks() {
        if (function_.return_type != IRType::Pointer &&
            (function_.return_pointer_pointee_type != IRType::Void ||
             function_.return_pointer_pointee_struct_type_id != 0u ||
             function_.return_pointer_pointee_enum_type_id != 0u)) {
            report("IR093", "non-pointer function return carries pointer pointee metadata",
                   function_.span);
        }
        for (const IRBlock& block : function_.blocks) {
            if (!blocks_.emplace(block.id, &block).second) {
                report("IR001", "duplicate basic-block identifier", {});
            }
        }
        if (blocks_.find(function_.entry_block) == blocks_.end()) {
            report("IR002", "function entry block does not exist", function_.span);
        }
        if (function_.blocks.empty()) {
            report("IR003", "function must contain at least one basic block",
                   function_.span);
        }
        if (function_.return_type == IRType::Array) {
            if (!valid_array_descriptor(function_array_return_value(function_),
                                        struct_types_.size(), enum_types_.size()) ||
                function_.return_struct_type_id != 0 ||
                function_.return_enum_type_id != 0) {
                report("IR053", "function has an invalid array return shape or nominal metadata", function_.span);
            }
        } else if (function_.return_type == IRType::Struct) {
            if (function_.return_struct_type_id == 0 ||
                function_.return_struct_type_id > struct_types_.size())
                report("IR060", "function has an invalid structure return type", function_.span);
            if (function_.return_element_type != IRType::Void ||
                function_.return_array_length != 0 ||
                function_.return_element_struct_type_id != 0 ||
                function_.return_element_enum_type_id != 0 ||
                !function_.return_array_dimensions.empty())
                report("IR054", "structure function return cannot carry array shape metadata", function_.span);
        } else if (function_.return_type == IRType::Enum) {
            if (function_.return_enum_type_id == 0 ||
                function_.return_enum_type_id > enum_types_.size())
                report("IR086", "function has an invalid enum return type", function_.span);
            if (function_.return_element_type != IRType::Void ||
                function_.return_array_length != 0 ||
                function_.return_element_struct_type_id != 0 ||
                function_.return_element_enum_type_id != 0 ||
                !function_.return_array_dimensions.empty() ||
                function_.return_struct_type_id != 0)
                report("IR054", "enum function return cannot carry unrelated aggregate metadata", function_.span);
        } else if (function_.return_type == IRType::Pointer) {
            const IRValue pointer_return = ir_make_pointer_value(
                function_.return_pointer_pointee_type, function_.span,
                function_.return_pointer_pointee_struct_type_id,
                function_.return_pointer_pointee_enum_type_id);
            const bool primitive_pointee =
                (pointer_return.pointer_pointee_type == IRType::Int ||
                 pointer_return.pointer_pointee_type == IRType::Byte ||
                 pointer_return.pointer_pointee_type == IRType::Float ||
                 pointer_return.pointer_pointee_type == IRType::Bool ||
                 pointer_return.pointer_pointee_type == IRType::String) &&
                pointer_return.pointer_pointee_struct_type_id == 0u &&
                pointer_return.pointer_pointee_enum_type_id == 0u;
            const bool struct_pointee =
                pointer_return.pointer_pointee_type == IRType::Struct &&
                pointer_return.pointer_pointee_struct_type_id != 0u &&
                pointer_return.pointer_pointee_struct_type_id <= struct_types_.size() &&
                pointer_return.pointer_pointee_enum_type_id == 0u;
            const bool enum_pointee =
                pointer_return.pointer_pointee_type == IRType::Enum &&
                pointer_return.pointer_pointee_enum_type_id != 0u &&
                pointer_return.pointer_pointee_enum_type_id <= enum_types_.size() &&
                pointer_return.pointer_pointee_struct_type_id == 0u;
            if (!primitive_pointee && !struct_pointee && !enum_pointee)
                report("IR090", "function has an invalid pointer return descriptor", function_.span);
            if (function_.return_element_type != IRType::Void ||
                function_.return_array_length != 0u ||
                function_.return_element_struct_type_id != 0u ||
                function_.return_element_enum_type_id != 0u ||
                function_.return_struct_type_id != 0u ||
                function_.return_enum_type_id != 0u ||
                !function_.return_array_dimensions.empty())
                report("IR091", "pointer function return cannot carry unrelated type metadata",
                       function_.span);
        } else if (function_.return_type == IRType::Void ||
                   function_.return_type == IRType::Int ||
                   function_.return_type == IRType::Float ||
                   function_.return_type == IRType::Bool ||
                   function_.return_type == IRType::Byte ||
                   function_.return_type == IRType::String) {
            if (function_.return_element_type != IRType::Void ||
                function_.return_array_length != 0 ||
                function_.return_element_struct_type_id != 0 ||
                function_.return_element_enum_type_id != 0 ||
                function_.return_struct_type_id != 0 ||
                function_.return_enum_type_id != 0 ||
                !function_.return_array_dimensions.empty()) {
                report("IR054", "primitive function return cannot carry array shape metadata",
                       function_.span);
            }
        } else {
            report("IR004", "function has an unknown return type", function_.span);
        }
        for (const IRFunctionParameter& parameter : function_.parameters) {
            if (!is_value_type(parameter.value.type)) {
                report("IR005", "function parameters cannot have type void",
                       parameter.value.span);
            }
        }
    }

    void index_places() {
        for (std::size_t index = 0; index < function_.places.size(); ++index) {
            const IRPlace& place = function_.places[index];
            if (place.id != index + 1u ||
                !places_.emplace(place.id, &place).second) {
                report("IR094", "function places must have unique sequential nonzero identifiers",
                       place.span);
            }
            if (!valid_place_type(place.value_type, struct_types_.size(), enum_types_.size())) {
                report("IR095", "function place has an invalid or incomplete storage type descriptor",
                       place.span);
            }
            if (place.projection == IRPlaceProjectionKind::Root) {
                if (place.parent != 0u || place.field_index != 0u || place.index_value != 0u || place.pointer_value != 0u)
                    report("IR104", "root place cannot carry projection metadata", place.span);
                continue;
            }
            if (place.projection == IRPlaceProjectionKind::PointerTarget) {
                if (place.parent != 0u || place.field_index != 0u || place.index_value != 0u ||
                    place.pointer_value == 0u || !place.mutable_place)
                    report("IR113", "pointer-target place must identify one mutable pointer value", place.span);
                continue;
            }
            if (place.pointer_value != 0u)
                report("IR113", "only pointer-target places may carry a pointer value", place.span);
            const auto parent = places_.find(place.parent);
            if (place.parent == 0u || place.parent >= place.id || parent == places_.end()) {
                report("IR105", "projected place must reference an earlier valid parent place",
                       place.span);
                continue;
            }
            if (place.mutable_place != parent->second->mutable_place)
                report("IR106", "projected place mutability must match its parent", place.span);
            if (place.projection == IRPlaceProjectionKind::StructField) {
                const IRStructField* field = struct_field(
                    parent->second->value_type.struct_type_id, place.field_index);
                if (parent->second->value_type.type != IRType::Struct || !field ||
                    place.index_value != 0u ||
                    !same_value_shape(place.value_type, ir_struct_field_value(*field)))
                    report("IR107", "structure-field projection has an invalid parent, field, or result type",
                           place.span);
            } else if (place.projection == IRPlaceProjectionKind::ArrayElement) {
                if (parent->second->value_type.type != IRType::Array ||
                    place.field_index != 0u || place.index_value == 0u ||
                    !same_value_shape(place.value_type,
                                      ir_array_element_value(parent->second->value_type)))
                    report("IR108", "array-element projection has an invalid parent, index, or result type",
                           place.span);
            } else {
                report("IR109", "function place has an unknown projection kind", place.span);
            }
        }
    }

    void add_definition(const IRValue& value, const IRBlockId block,
                        const std::int64_t position) {
        if (!is_value_type(value.type)) {
            report("IR006", "SSA values cannot have type void", value.span);
        }
        if (value.type == IRType::Array &&
            (!valid_array_descriptor(value, struct_types_.size(), enum_types_.size()) ||
             value.pointer_pointee_type != IRType::Void ||
             value.pointer_pointee_struct_type_id != 0u ||
             value.pointer_pointee_enum_type_id != 0u)) {
            report("IR050", "SSA array value has an invalid element type, aggregate identity, or pointer metadata",
                   value.span);
        } else if (value.type == IRType::Struct) {
            if (value.pointer_pointee_type != IRType::Void ||
                value.pointer_pointee_struct_type_id != 0u ||
                value.pointer_pointee_enum_type_id != 0u)
                report("IR112", "non-pointer structure value carries pointer metadata", value.span);
            if (value.struct_type_id == 0 || value.struct_type_id > struct_types_.size())
                report("IR060", "SSA value has an invalid structure type", value.span);
            if (value.element_type != IRType::Void || value.array_length != 0 ||
                value.element_struct_type_id != 0 || value.enum_type_id != 0 ||
                !value.array_dimensions.empty())
                report("IR051", "structure SSA value cannot carry array shape metadata", value.span);
        } else if (value.type == IRType::Pointer) {
            const bool primitive_pointee =
                (value.pointer_pointee_type == IRType::Int ||
                 value.pointer_pointee_type == IRType::Byte ||
                 value.pointer_pointee_type == IRType::Float ||
                 value.pointer_pointee_type == IRType::Bool ||
                 value.pointer_pointee_type == IRType::String) &&
                value.pointer_pointee_struct_type_id == 0u &&
                value.pointer_pointee_enum_type_id == 0u;
            const bool struct_pointee = value.pointer_pointee_type == IRType::Struct &&
                value.pointer_pointee_struct_type_id != 0u &&
                value.pointer_pointee_struct_type_id <= struct_types_.size() &&
                value.pointer_pointee_enum_type_id == 0u;
            const bool enum_pointee = value.pointer_pointee_type == IRType::Enum &&
                value.pointer_pointee_enum_type_id != 0u &&
                value.pointer_pointee_enum_type_id <= enum_types_.size() &&
                value.pointer_pointee_struct_type_id == 0u;
            if (!primitive_pointee && !struct_pointee && !enum_pointee)
                report("IR112", "SSA pointer has an unsupported or invalid pointee type", value.span);
            if (value.struct_type_id != 0 || value.enum_type_id != 0 ||
                value.element_type != IRType::Void || value.array_length != 0 ||
                value.element_struct_type_id != 0 || !value.array_dimensions.empty())
                report("IR112", "pointer SSA value carries invalid aggregate metadata", value.span);
        } else if (value.type == IRType::Enum) {
            if (value.pointer_pointee_type != IRType::Void ||
                value.pointer_pointee_struct_type_id != 0u ||
                value.pointer_pointee_enum_type_id != 0u)
                report("IR112", "non-pointer enum value carries pointer metadata", value.span);
            if (value.enum_type_id == 0 || value.enum_type_id > enum_types_.size())
                report("IR086", "SSA value has an invalid enum type", value.span);
            if (value.element_type != IRType::Void || value.array_length != 0 ||
                value.struct_type_id != 0 || value.element_struct_type_id != 0 ||
                !value.array_dimensions.empty())
                report("IR051", "enum SSA value cannot carry aggregate shape metadata", value.span);
        } else if (value.type != IRType::Array &&
                   (value.element_type != IRType::Void || value.array_length != 0 ||
                    value.struct_type_id != 0 || value.element_struct_type_id != 0 ||
                    value.enum_type_id != 0 || !value.array_dimensions.empty() ||
                    value.pointer_pointee_type != IRType::Void ||
                    value.pointer_pointee_struct_type_id != 0u ||
                    value.pointer_pointee_enum_type_id != 0u)) {
            report("IR051", "primitive SSA value cannot carry aggregate shape metadata", value.span);
        }
        const auto inserted = definitions_.emplace(
            value.id, Definition{value, block, position});
        if (!inserted.second) {
            report("IR007", "SSA value identifier is defined more than once",
                   value.span);
        }
    }

    void collect_definitions() {
        for (const IRFunctionParameter& parameter : function_.parameters) {
            add_definition(parameter.value, function_.entry_block, -1);
        }
        for (const IRBlock& block : function_.blocks) {
            for (const IRValue& parameter : block.parameters) {
                add_definition(parameter, block.id, -1);
            }
            for (std::size_t index = 0; index < block.instructions.size(); ++index) {
                const IRInstruction& instruction = block.instructions[index];
                if (instruction.result) {
                    add_definition(*instruction.result, block.id,
                                   static_cast<std::int64_t>(index));
                }
            }
        }
    }

    [[nodiscard]] std::optional<IRType> operand_type(
        const IRInstruction& instruction, const std::size_t index) {
        if (index >= instruction.operands.size()) return std::nullopt;
        const auto found = definitions_.find(instruction.operands[index]);
        if (found == definitions_.end()) return std::nullopt;
        return found->second.value.type;
    }

    [[nodiscard]] const IRValue* operand_value(
        const IRInstruction& instruction, const std::size_t index) const {
        if (index >= instruction.operands.size()) return nullptr;
        const auto found = definitions_.find(instruction.operands[index]);
        return found == definitions_.end() ? nullptr : &found->second.value;
    }

    static bool same_array_shape(const IRValue& left, const IRValue& right) {
        return ir_same_array_shape(left, right);
    }

    static IRValue field_value(const IRStructField& field) {
        return ir_struct_field_value(field);
    }

    static IRValue array_element_value(const IRValue& array) {
        return ir_array_element_value(array);
    }

    static bool same_value_shape(const IRValue& left, const IRValue& right) {
        if (left.type != right.type) return false;
        if (left.type == IRType::Array) return same_array_shape(left, right);
        if (left.type == IRType::Struct) return left.struct_type_id == right.struct_type_id;
        if (left.type == IRType::Enum) return left.enum_type_id == right.enum_type_id;
        if (left.type == IRType::Pointer)
            return left.pointer_pointee_type == right.pointer_pointee_type &&
                   left.pointer_pointee_struct_type_id == right.pointer_pointee_struct_type_id &&
                   left.pointer_pointee_enum_type_id == right.pointer_pointee_enum_type_id;
        return true;
    }

    [[nodiscard]] const IRStructField* struct_field(
        const std::uint32_t type_id, const std::uint32_t index) const {
        if (type_id == 0 || type_id > struct_types_.size()) return nullptr;
        const IRStructType& type = struct_types_[type_id - 1u];
        return index < type.fields.size() ? &type.fields[index] : nullptr;
    }

    [[nodiscard]] static bool parse_field_index(
        const std::string& text, std::uint32_t& index) {
        if (text.empty()) return false;
        std::uint64_t parsed = 0;
        for (const char character : text) {
            if (character < '0' || character > '9') return false;
            parsed = parsed * 10u + static_cast<std::uint64_t>(character - '0');
            if (parsed > 0xffffffffu) return false;
        }
        index = static_cast<std::uint32_t>(parsed);
        return true;
    }

    void check_arity(const IRInstruction& instruction, const std::size_t expected) {
        if (instruction.operands.size() != expected) {
            report("IR008", std::string(ir_op_name(instruction.op)) +
                   " expects " + std::to_string(expected) + " operand(s), got " +
                   std::to_string(instruction.operands.size()), instruction.span);
        }
    }

    void check_result(const IRInstruction& instruction, const IRType expected) {
        if (!instruction.result) {
            report("IR009", std::string(ir_op_name(instruction.op)) +
                   " must define a result value", instruction.span);
        } else if (instruction.result->type != expected) {
            report("IR010", std::string(ir_op_name(instruction.op)) +
                   " result must have type " + ir_type_name(expected), instruction.result->span);
        }
    }

    void check_no_result(const IRInstruction& instruction) {
        if (instruction.result) {
            report("IR011", std::string(ir_op_name(instruction.op)) +
                   " cannot define a result value", instruction.result->span);
        }
    }

    void validate_instruction(const IRInstruction& instruction) {
        const auto left = operand_type(instruction, 0);
        const auto right = operand_type(instruction, 1);
        const bool is_memory_operation = instruction.op == IROp::Load ||
                                         instruction.op == IROp::Store ||
                                         instruction.op == IROp::AddressOf;
        if (!is_memory_operation && instruction.place)
            report("IR096", "only load, store, and address-of instructions may reference a place",
                   instruction.span);
        switch (instruction.op) {
        case IROp::AddressOf: {
            check_arity(instruction, 0u);
            if (!instruction.place) {
                report("IR114", "address-of must identify a function place", instruction.span);
                break;
            }
            const auto found = places_.find(*instruction.place);
            if (found == places_.end()) {
                report("IR114", "address-of references an unknown function place", instruction.span);
                break;
            }
            if (!found->second->mutable_place)
                report("IR114", "address-of currently requires a mutable place", instruction.span);
            if (!instruction.result || instruction.result->type != IRType::Pointer ||
                instruction.result->pointer_pointee_type != found->second->value_type.type ||
                instruction.result->pointer_pointee_struct_type_id !=
                    (found->second->value_type.type == IRType::Struct
                        ? found->second->value_type.struct_type_id : 0u) ||
                instruction.result->pointer_pointee_enum_type_id !=
                    (found->second->value_type.type == IRType::Enum
                        ? found->second->value_type.enum_type_id : 0u))
                report("IR114", "address-of result must point to the complete place value type", instruction.span);
            break;
        }
        case IROp::Load: {
            check_arity(instruction, 0u);
            if (!instruction.place) {
                report("IR097", "load must identify a function place", instruction.span);
                break;
            }
            const auto found = places_.find(*instruction.place);
            if (found == places_.end()) {
                report("IR098", "load references an unknown function place", instruction.span);
                break;
            }
            if (!instruction.result ||
                !same_value_shape(*instruction.result, found->second->value_type)) {
                report("IR099", "load result does not match the place storage type", instruction.span);
            }
            break;
        }
        case IROp::Store: {
            check_arity(instruction, 1u);
            check_no_result(instruction);
            if (!instruction.place) {
                report("IR100", "store must identify a function place", instruction.span);
                break;
            }
            const auto found = places_.find(*instruction.place);
            if (found == places_.end()) {
                report("IR101", "store references an unknown function place", instruction.span);
                break;
            }
            if (!found->second->mutable_place)
                report("IR102", "store cannot modify an immutable function place", instruction.span);
            const IRValue* stored = operand_value(instruction, 0u);
            if (stored && !same_value_shape(*stored, found->second->value_type))
                report("IR103", "stored value does not match the place storage type", instruction.span);
            break;
        }
        case IROp::Constant:
            check_arity(instruction, 0);
            if (!instruction.result) {
                report("IR009", "constant must define a result value", instruction.span);
            } else if (!is_value_type(instruction.result->type)) {
                report("IR006", "constant cannot have type void", instruction.result->span);
            }
            if (instruction.immediate.empty()) {
                report("IR012", "constant must retain its literal spelling", instruction.span);
            }
            if (instruction.result && instruction.result->type == IRType::Pointer &&
                instruction.immediate != "null") {
                report("IR094", "pointer constants must use the typed null literal", instruction.span);
            }
            break;
        case IROp::StringConcat:
            check_arity(instruction, 2);
            if ((left && *left != IRType::String) || (right && *right != IRType::String))
                report("IR072", "string concatenation requires two string operands", instruction.span);
            check_result(instruction, IRType::String);
            break;
        case IROp::StringLength:
            check_arity(instruction, 1);
            if (left && *left != IRType::String)
                report("IR073", "string length requires a string operand", instruction.span);
            check_result(instruction, IRType::Int);
            break;
        case IROp::StringJoin: {
            check_arity(instruction, 1);
            const IRValue* array = operand_value(instruction, 0);
            if (array && (!is_simple_dynamic_array(*array) ||
                          array->element_type != IRType::String ||
                          array->element_struct_type_id != 0)) {
                report("IR074", "string join requires a dynamic array of strings", instruction.span);
            }
            check_result(instruction, IRType::String);
            break;
        }
        case IROp::ByteSlice: {
            check_arity(instruction, 3);
            const IRValue* array = operand_value(instruction, 0);
            const IRValue* start = operand_value(instruction, 1);
            const IRValue* count = operand_value(instruction, 2);
            if (array && (!is_simple_dynamic_array(*array) ||
                          array->element_type != IRType::Byte ||
                          array->element_struct_type_id != 0))
                report("IR080", "byte_slice requires a dynamic byte array", instruction.span);
            if (start && start->type != IRType::Int)
                report("IR081", "byte_slice start must be an int", instruction.span);
            if (count && count->type != IRType::Int)
                report("IR082", "byte_slice length must be an int", instruction.span);
            check_result(instruction, IRType::Array);
            if (instruction.result && !is_simple_dynamic_array(*instruction.result))
                report("IR083", "byte_slice result must be a dynamic byte array", instruction.span);
            break;
        }
        case IROp::ByteRead:
        case IROp::ByteWrite: {
            const bool writing = instruction.op == IROp::ByteWrite;
            check_arity(instruction, writing ? 3u : 2u);
            const IRValue* array = operand_value(instruction, 0);
            const IRValue* offset = operand_value(instruction, 1);
            const IRValue* value = writing ? operand_value(instruction, 2) : nullptr;
            if (array && (!is_simple_dynamic_array(*array) ||
                          array->element_type != IRType::Byte ||
                          array->element_struct_type_id != 0))
                report("IR084", "binary integer operations require a dynamic byte array", instruction.span);
            if (offset && offset->type != IRType::Int)
                report("IR085", "binary integer offset must be an int", instruction.span);
            if (value && value->type != IRType::Int)
                report("IR086", "binary integer value must be an int", instruction.span);
            if (instruction.immediate != "2le" && instruction.immediate != "2be" &&
                instruction.immediate != "4le" && instruction.immediate != "4be")
                report("IR087", "binary integer operation has an invalid width or byte order", instruction.span);
            if (writing) check_no_result(instruction);
            else check_result(instruction, IRType::Int);
            break;
        }
        case IROp::FileRead:
        case IROp::FileWrite: {
            check_arity(instruction, 2);
            const IRValue* path = operand_value(instruction, 0);
            const IRValue* array = operand_value(instruction, 1);
            if (path && path->type != IRType::String)
                report("IR088", "file path must be a string", instruction.span);
            if (array && (!is_simple_dynamic_array(*array) ||
                          array->element_type != IRType::Byte ||
                          array->element_struct_type_id != 0))
                report("IR089", "binary file operations require a dynamic byte array", instruction.span);
            check_result(instruction, IRType::Int);
            break;
        }
        case IROp::Add:
        case IROp::Subtract:
        case IROp::Multiply:
        case IROp::Divide:
            check_arity(instruction, 2);
            if (left && right && (*left != *right || !is_numeric(*left))) {
                report("IR013", "arithmetic operands must have the same numeric type",
                       instruction.span);
            }
            if (left) check_result(instruction, *left);
            else if (!instruction.result)
                report("IR009", "arithmetic instruction must define a result", instruction.span);
            break;
        case IROp::BitwiseAnd:
        case IROp::BitwiseOr:
        case IROp::BitwiseXor:
            check_arity(instruction, 2);
            if (left && right && (*left != *right ||
                (*left != IRType::Int && *left != IRType::Byte)))
                report("IR078", "bitwise binary operations require matching int or byte operands", instruction.span);
            if (left) check_result(instruction, *left);
            break;
        case IROp::BitwiseNot:
            check_arity(instruction, 1);
            if (left && *left != IRType::Int && *left != IRType::Byte)
                report("IR079", "bitwise NOT requires an int or byte operand", instruction.span);
            if (left) check_result(instruction, *left);
            break;
        case IROp::ShiftLeft:
        case IROp::ShiftRight:
        case IROp::RotateLeft:
        case IROp::RotateRight:
            check_arity(instruction, 2);
            if (left && *left != IRType::Int && *left != IRType::Byte)
                report("IR080", "shift/rotate value must be int or byte", instruction.span);
            if (right && *right != IRType::Int)
                report("IR081", "shift/rotate count must be int", instruction.span);
            if (left) check_result(instruction, *left);
            break;
        case IROp::Remainder:
            check_arity(instruction, 2);
            if ((left && *left != IRType::Int) || (right && *right != IRType::Int)) {
                report("IR014", "remainder operands must both have type int",
                       instruction.span);
            }
            check_result(instruction, IRType::Int);
            break;
        case IROp::Negate:
            check_arity(instruction, 1);
            if (left && !is_numeric(*left)) {
                report("IR015", "negation requires a numeric operand", instruction.span);
            }
            if (left) check_result(instruction, *left);
            break;
        case IROp::LogicalNot:
            check_arity(instruction, 1);
            if (left && *left != IRType::Bool) {
                report("IR016", "logical not requires a bool operand", instruction.span);
            }
            check_result(instruction, IRType::Bool);
            break;
        case IROp::LogicalAnd:
        case IROp::LogicalOr:
            check_arity(instruction, 2);
            if ((left && *left != IRType::Bool) ||
                (right && *right != IRType::Bool)) {
                report("IR017", "logical operators require bool operands", instruction.span);
            }
            check_result(instruction, IRType::Bool);
            break;
        case IROp::Equal:
        case IROp::NotEqual: {
            check_arity(instruction, 2);
            if (left && right && (*left != *right ||
                !is_value_type(*left) || !is_value_type(*right))) {
                report("IR018", "equality operands must have the same non-void type",
                       instruction.span);
            }
            const IRValue* left_value = operand_value(instruction, 0);
            const IRValue* right_value = operand_value(instruction, 1);
            if (left_value && right_value &&
                !same_value_shape(*left_value, *right_value)) {
                report("IR018", "equality operands must have identical nominal aggregate types",
                       instruction.span);
            }
            check_result(instruction, IRType::Bool);
            break;
        }
        case IROp::Less:
        case IROp::LessEqual:
        case IROp::Greater:
        case IROp::GreaterEqual:
            check_arity(instruction, 2);
            if (left && right && (*left != *right ||
                (!is_numeric(*left) && *left != IRType::Byte))) {
                report("IR019", "relational operands must have the same numeric type or both be byte",
                       instruction.span);
            }
            check_result(instruction, IRType::Bool);
            break;
        case IROp::IntToFloat:
            check_arity(instruction, 1);
            if (left && *left != IRType::Int) {
                report("IR020", "int-to-float conversion requires an int operand",
                       instruction.span);
            }
            check_result(instruction, IRType::Float);
            break;
        case IROp::IntToByteChecked:
            check_arity(instruction, 1);
            if (left && *left != IRType::Int)
                report("IR081", "checked int-to-byte conversion requires an int operand", instruction.span);
            check_result(instruction, IRType::Byte);
            break;
        case IROp::ByteToInt:
            check_arity(instruction, 1);
            if (left && *left != IRType::Byte)
                report("IR082", "byte-to-int conversion requires a byte operand", instruction.span);
            check_result(instruction, IRType::Int);
            break;
        case IROp::Math: {
            const bool unary_math = instruction.callee == "sqrt" ||
                instruction.callee == "sin" || instruction.callee == "cos" ||
                instruction.callee == "abs";
            const bool binary_math = instruction.callee == "pow";
            if (!unary_math && !binary_math) {
                report("IR039", "unknown mathematical intrinsic '" +
                       instruction.callee + "'", instruction.span);
                break;
            }
            check_arity(instruction, binary_math ? 2u : 1u);
            for (std::size_t index = 0; index < instruction.operands.size(); ++index) {
                const auto actual = operand_type(instruction, index);
                if (actual && *actual != IRType::Float) {
                    report("IR040", "mathematical intrinsic arguments must be float",
                           instruction.span);
                }
            }
            check_result(instruction, IRType::Float);
            break;
        }
        case IROp::Call: {
            const auto found = functions_.find(instruction.callee);
            if (found == functions_.end()) {
                report("IR021", "call references unknown function '" +
                       instruction.callee + "'", instruction.span);
                break;
            }
            if (instruction.operands.size() != found->second.parameters.size()) {
                report("IR022", "call to '" + instruction.callee + "' has the wrong argument count",
                       instruction.span);
            }
            const std::size_t count = std::min(
                instruction.operands.size(), found->second.parameters.size());
            for (std::size_t index = 0; index < count; ++index) {
                const IRValue* actual = operand_value(instruction, index);
                const IRValue& expected = found->second.parameters[index];
                if (actual && (actual->type != expected.type ||
                    !same_value_shape(*actual, expected))) {
                    report("IR023", "call argument " + std::to_string(index + 1) +
                           " has the wrong type or fixed-array shape", instruction.span);
                }
            }
            if (found->second.return_type == IRType::Void) {
                check_no_result(instruction);
            } else {
                check_result(instruction, found->second.return_type);
                if (found->second.return_type == IRType::Array &&
                    instruction.result &&
                    !same_array_shape(*instruction.result, ir_make_array_value(
                        found->second.return_element_type,
                        found->second.return_array_dimensions.empty()
                            ? std::vector<std::uint32_t>{found->second.return_array_length}
                            : found->second.return_array_dimensions,
                        found->second.return_element_struct_type_id, instruction.span,
                        found->second.return_element_enum_type_id))) {
                    report("IR055", "call result does not match the callee's fixed-array return shape",
                           instruction.span);
                }
                if (found->second.return_type == IRType::Struct &&
                    instruction.result &&
                    instruction.result->struct_type_id != found->second.return_struct_type_id) {
                    report("IR071", "call result does not match the callee's structure type",
                           instruction.span);
                }
                if (found->second.return_type == IRType::Enum &&
                    instruction.result &&
                    instruction.result->enum_type_id != found->second.return_enum_type_id) {
                    report("IR086", "call result does not match the callee's enum type",
                           instruction.span);
                }
                if (found->second.return_type == IRType::Pointer && instruction.result &&
                    (instruction.result->pointer_pointee_type !=
                         found->second.return_pointer_pointee_type ||
                     instruction.result->pointer_pointee_struct_type_id !=
                         found->second.return_pointer_pointee_struct_type_id ||
                     instruction.result->pointer_pointee_enum_type_id !=
                         found->second.return_pointer_pointee_enum_type_id)) {
                    report("IR092", "call result does not match the callee's typed pointer return",
                           instruction.span);
                }
            }
            break;
        }
        case IROp::EnumMake: {
            const IRValue* result = instruction.result ? &*instruction.result : nullptr;
            if (!result || result->type != IRType::Enum ||
                result->enum_type_id == 0 || result->enum_type_id > enum_types_.size()) {
                report("IR086", "enum construction must define a value with a valid nominal enum type",
                       instruction.span);
                break;
            }
            std::uint32_t variant_index = 0;
            if (!parse_field_index(instruction.immediate, variant_index)) {
                report("IR087", "enum construction has an invalid variant index", instruction.span);
                break;
            }
            const IREnumType& enumeration = enum_types_[result->enum_type_id - 1u];
            if (variant_index >= enumeration.variants.size()) {
                report("IR087", "enum construction variant index is out of range", instruction.span);
                break;
            }
            const auto& fields = enumeration.variants[variant_index].fields;
            if (instruction.operands.size() != fields.size())
                report("IR088", "enum construction operand count does not match the variant payload", instruction.span);
            const std::size_t count = std::min(instruction.operands.size(), fields.size());
            for (std::size_t index = 0; index < count; ++index) {
                const IRValue* actual = operand_value(instruction, index);
                if (actual && !same_value_shape(*actual, field_value(fields[index])))
                    report("IR089", "enum construction payload has the wrong type or nominal identity", instruction.span);
            }
            break;
        }
        case IROp::EnumTag: {
            check_arity(instruction, 1);
            const IRValue* object = operand_value(instruction, 0);
            if (object && (object->type != IRType::Enum || object->enum_type_id == 0 ||
                           object->enum_type_id > enum_types_.size()))
                report("IR090", "enum tag extraction requires a valid enum operand", instruction.span);
            check_result(instruction, IRType::Int);
            break;
        }
        case IROp::EnumPayloadGet: {
            check_arity(instruction, 1);
            const IRValue* object = operand_value(instruction, 0);
            if (!object || object->type != IRType::Enum || object->enum_type_id == 0 ||
                object->enum_type_id > enum_types_.size()) {
                report("IR091", "enum payload extraction requires a valid enum operand", instruction.span);
                break;
            }
            const auto separator = instruction.immediate.find(':');
            std::uint32_t variant_index = 0;
            std::uint32_t field_index = 0;
            if (separator == std::string::npos ||
                !parse_field_index(instruction.immediate.substr(0, separator), variant_index) ||
                !parse_field_index(instruction.immediate.substr(separator + 1u), field_index)) {
                report("IR092", "enum payload extraction has an invalid variant/field selector",
                       instruction.span);
                break;
            }
            const IREnumType& enumeration = enum_types_[object->enum_type_id - 1u];
            if (variant_index >= enumeration.variants.size() ||
                field_index >= enumeration.variants[variant_index].fields.size()) {
                report("IR092", "enum payload extraction selector is out of range", instruction.span);
                break;
            }
            const IRValue expected = field_value(enumeration.variants[variant_index].fields[field_index]);
            if (!instruction.result || !same_value_shape(*instruction.result, expected))
                report("IR093", "enum payload extraction result has the wrong type or nominal identity",
                       instruction.span);
            break;
        }
        case IROp::Say:
            check_arity(instruction, 1);
            if (left && (!is_value_type(*left) || *left == IRType::Array ||
                         *left == IRType::Struct || *left == IRType::Enum)) {
                report("IR024", "say requires a printable primitive value", instruction.span);
            }
            check_no_result(instruction);
            break;
        case IROp::ArrayMake: {
            if (!instruction.result || instruction.result->type != IRType::Array) {
                report("IR041", "array construction must define an array result", instruction.span);
                break;
            }
            const IRValue& result = *instruction.result;
            if (!valid_array_descriptor(result, struct_types_.size(), enum_types_.size()))
                report("IR042", "array result has an invalid element type, dimension vector, or nominal identity", result.span);
            if (result.array_length != 0 &&
                instruction.operands.size() != result.array_length)
                report("IR043", "array construction operand count does not match its fixed length", instruction.span);
            for (std::size_t index = 0; index < instruction.operands.size(); ++index) {
                const IRValue* actual = operand_value(instruction, index);
                const IRValue expected = array_element_value(result);
                if (actual && !same_value_shape(*actual, expected))
                    report("IR044", "array element operand has the wrong type or structure identity", instruction.span);
            }
            break;
        }
        case IROp::StructMake: {
            if (!instruction.result || instruction.result->type != IRType::Struct) {
                report("IR061", "structure construction must define a structure result", instruction.span);
                break;
            }
            const IRValue& result = *instruction.result;
            if (result.struct_type_id == 0 || result.struct_type_id > struct_types_.size()) {
                report("IR060", "structure construction has an invalid structure type", instruction.span);
                break;
            }
            const auto& fields = struct_types_[result.struct_type_id - 1u].fields;
            if (instruction.operands.size() != fields.size())
                report("IR062", "structure construction operand count does not match its field count", instruction.span);
            const std::size_t count = std::min(instruction.operands.size(), fields.size());
            for (std::size_t index = 0; index < count; ++index) {
                const IRValue* actual = operand_value(instruction, index);
                if (actual && !same_value_shape(*actual, field_value(fields[index])))
                    report("IR063", "structure construction field has the wrong type or aggregate shape", instruction.span);
            }
            break;
        }
        case IROp::StructGet:
        case IROp::StructSet: {
            const bool is_set = instruction.op == IROp::StructSet;
            check_arity(instruction, is_set ? 2u : 1u);
            const IRValue* object = operand_value(instruction, 0);
            if (!object || object->type != IRType::Struct) {
                report("IR064", "structure field operation requires a structure operand", instruction.span);
                break;
            }
            std::uint32_t field_index = 0;
            if (!parse_field_index(instruction.immediate, field_index)) {
                report("IR065", "structure field operation has an invalid field index", instruction.span);
                break;
            }
            const IRStructField* field = struct_field(object->struct_type_id, field_index);
            if (!field) {
                report("IR065", "structure field index is out of range", instruction.span);
                break;
            }
            if (is_set) {
                const IRValue* value = operand_value(instruction, 1);
                if (value && !same_value_shape(*value, field_value(*field)))
                    report("IR063", "structure field update value has the wrong type or aggregate shape", instruction.span);
                if (!instruction.result || instruction.result->type != IRType::Struct ||
                    instruction.result->struct_type_id != object->struct_type_id)
                    report("IR066", "structure field update must preserve the source structure type", instruction.span);
            } else {
                if (!instruction.result ||
                    !same_value_shape(*instruction.result, field_value(*field)))
                    report("IR067", "structure field read result has the wrong type or aggregate shape", instruction.span);
            }
            break;
        }
        case IROp::ArrayGet: {
            check_arity(instruction, 2);
            const IRValue* array = operand_value(instruction, 0);
            if (array && array->type != IRType::Array)
                report("IR045", "array indexing requires an array operand", instruction.span);
            if (right && *right != IRType::Int)
                report("IR046", "array index operand must have type int", instruction.span);
            if (array) {
                if (!instruction.result ||
                    !same_value_shape(*instruction.result, array_element_value(*array)))
                    report("IR009", "array indexing result has the wrong immediate element type or aggregate shape",
                           instruction.span);
            } else if (!instruction.result) {
                report("IR009", "array indexing must define a result", instruction.span);
            }
            break;
        }
        case IROp::ArraySet: {
            check_arity(instruction, 3);
            const IRValue* array = operand_value(instruction, 0);
            const auto index_type = operand_type(instruction, 1);
            const IRValue* value = operand_value(instruction, 2);
            if (array && array->type != IRType::Array)
                report("IR047", "array update requires an array operand", instruction.span);
            if (index_type && *index_type != IRType::Int)
                report("IR046", "array index operand must have type int", instruction.span);
            if (array && value &&
                !same_value_shape(*value, array_element_value(*array)))
                report("IR048", "array update value has the wrong immediate element type or aggregate shape", instruction.span);
            if (array && instruction.result && !same_array_shape(*array, *instruction.result))
                report("IR049", "array update result must preserve the source array shape", instruction.span);
            else if (array && !instruction.result)
                report("IR009", "array update must define a result", instruction.span);
            break;
        }
        case IROp::ArrayLength: {
            check_arity(instruction, 1);
            const IRValue* array = operand_value(instruction, 0);
            if (array && (!has_dynamic_outer_dimension(*array)))
                report("IR056", "dynamic array length requires a dynamic-array operand", instruction.span);
            check_result(instruction, IRType::Int);
            break;
        }
        case IROp::ArrayCapacity: {
            check_arity(instruction, 1);
            const IRValue* array = operand_value(instruction, 0);
            if (array && (!has_dynamic_outer_dimension(*array)))
                report("IR075", "array capacity requires a dynamic-array operand", instruction.span);
            check_result(instruction, IRType::Int);
            break;
        }
        case IROp::ArrayReserve: {
            check_arity(instruction, 2);
            const IRValue* array = operand_value(instruction, 0);
            const IRValue* requested = operand_value(instruction, 1);
            if (array && (!has_dynamic_outer_dimension(*array)))
                report("IR076", "array reserve requires a dynamic-array operand", instruction.span);
            if (requested && requested->type != IRType::Int)
                report("IR077", "array reserve capacity must be an int", instruction.span);
            check_no_result(instruction);
            break;
        }
        case IROp::ArrayPush: {
            check_arity(instruction, 2);
            const IRValue* array = operand_value(instruction, 0);
            const IRValue* value = operand_value(instruction, 1);
            if (array && (!has_dynamic_outer_dimension(*array)))
                report("IR057", "array push requires a dynamic array operand", instruction.span);
            if (array && value &&
                !same_value_shape(*value, array_element_value(*array)))
                report("IR058", "array push value has the wrong immediate element type or aggregate shape", instruction.span);
            check_no_result(instruction);
            break;
        }
        case IROp::ArrayPop: {
            check_arity(instruction, 1);
            const IRValue* array = operand_value(instruction, 0);
            if (array && (!has_dynamic_outer_dimension(*array)))
                report("IR059", "array pop requires a dynamic array operand", instruction.span);
            if (array) {
                if (!instruction.result ||
                    !same_value_shape(*instruction.result, array_element_value(*array)))
                    report("IR009", "array pop result has the wrong immediate element type or aggregate shape",
                           instruction.span);
            } else if (!instruction.result) {
                report("IR009", "array pop must define a result", instruction.span);
            }
            break;
        }
        }
    }

    void validate_branch(const IRBlock& source, const IRBranch& branch,
                         const SourceSpan span) {
        const auto target = blocks_.find(branch.target);
        if (target == blocks_.end()) {
            report("IR025", "branch target block does not exist", span);
            return;
        }
        successors_[source.id].push_back(branch.target);
        const auto& parameters = target->second->parameters;
        if (branch.arguments.size() != parameters.size()) {
            report("IR026", "branch argument count does not match target block parameters",
                   span);
        }
        const std::size_t count = std::min(branch.arguments.size(), parameters.size());
        for (std::size_t index = 0; index < count; ++index) {
            const auto value = definitions_.find(branch.arguments[index]);
            if (value != definitions_.end() &&
                !same_value_shape(value->second.value, parameters[index])) {
                report("IR027", "branch argument type does not match target block parameter",
                       span);
            }
        }
    }

    void validate_terminator(const IRBlock& block) {
        if (!block.terminator) {
            report("IR028", "basic block has no terminator", {});
            return;
        }
        std::visit([&](const auto& terminator) {
            using T = std::decay_t<decltype(terminator)>;
            if constexpr (std::is_same_v<T, IRReturn>) {
                if (function_.return_type == IRType::Void) {
                    if (terminator.value) {
                        report("IR029", "void function cannot return a value",
                               block.terminator->span);
                    }
                } else if (!terminator.value) {
                    report("IR030", "non-void function must return a value",
                           block.terminator->span);
                } else {
                    const auto value = definitions_.find(*terminator.value);
                    if (value != definitions_.end()) {
                        const IRValue& returned = value->second.value;
                        if (returned.type != function_.return_type ||
                            (function_.return_type == IRType::Array &&
                             !same_array_shape(returned, function_array_return_value(function_))) ||
                            (function_.return_type == IRType::Struct &&
                             returned.struct_type_id != function_.return_struct_type_id) ||
                            (function_.return_type == IRType::Pointer &&
                             (returned.pointer_pointee_type != function_.return_pointer_pointee_type ||
                              returned.pointer_pointee_struct_type_id !=
                                  function_.return_pointer_pointee_struct_type_id ||
                              returned.pointer_pointee_enum_type_id !=
                                  function_.return_pointer_pointee_enum_type_id))) {
                            report("IR031", "returned value has the wrong type or fixed-array shape",
                                   block.terminator->span);
                        }
                    }
                }
            } else if constexpr (std::is_same_v<T, IRBranch>) {
                validate_branch(block, terminator, block.terminator->span);
            } else if constexpr (std::is_same_v<T, IRConditionalBranch>) {
                const auto condition = definitions_.find(terminator.condition);
                if (condition != definitions_.end() &&
                    condition->second.value.type != IRType::Bool) {
                    report("IR032", "conditional branch requires a bool condition",
                           block.terminator->span);
                }
                validate_branch(block,
                    IRBranch{terminator.true_target, terminator.true_arguments},
                    block.terminator->span);
                validate_branch(block,
                    IRBranch{terminator.false_target, terminator.false_arguments},
                    block.terminator->span);
            } else if constexpr (std::is_same_v<T, IRUnreachable>) {
                // No operands.
            }
        }, block.terminator->data);
    }

    void validate_blocks() {
        for (const IRBlock& block : function_.blocks) {
            for (const IRInstruction& instruction : block.instructions) {
                validate_instruction(instruction);
            }
            validate_terminator(block);
        }
    }

    void validate_use(const IRValueId id, const IRBlockId use_block,
                      const std::int64_t use_position,
                      const SourceSpan span) {
        const auto found = definitions_.find(id);
        if (found == definitions_.end()) {
            report("IR033", "use of undefined SSA value %" + std::to_string(id), span);
            return;
        }
        const Definition& definition = found->second;
        if (definition.block == use_block) {
            if (definition.position >= use_position) {
                report("IR034", "SSA value is used before its definition", span);
            }
            return;
        }
        const auto dominators = dominators_.find(use_block);
        if (dominators == dominators_.end() ||
            dominators->second.find(definition.block) == dominators->second.end()) {
            report("IR035", "SSA definition does not dominate its use", span);
        }
    }

    void validate_projected_place_index_uses(
        const IRPlaceId place_id, const IRBlockId use_block,
        const std::int64_t use_position, const SourceSpan span) {
        const auto found = places_.find(place_id);
        if (found == places_.end()) return;
        const IRPlace& place = *found->second;
        if (place.projection == IRPlaceProjectionKind::PointerTarget) {
            const auto definition = definitions_.find(place.pointer_value);
            if (definition == definitions_.end())
                report("IR115", "pointer-target place references an undefined pointer value", span);
            else if (definition->second.value.type != IRType::Pointer ||
                     definition->second.value.pointer_pointee_type != place.value_type.type ||
                     definition->second.value.pointer_pointee_struct_type_id !=
                        (place.value_type.type == IRType::Struct ? place.value_type.struct_type_id : 0u) ||
                     definition->second.value.pointer_pointee_enum_type_id !=
                        (place.value_type.type == IRType::Enum ? place.value_type.enum_type_id : 0u))
                report("IR115", "pointer-target place has a pointer with the wrong pointee type", span);
            validate_use(place.pointer_value, use_block, use_position, span);
        }
        if (place.projection == IRPlaceProjectionKind::ArrayElement) {
            const auto definition = definitions_.find(place.index_value);
            if (definition == definitions_.end())
                report("IR110", "array-element projection references an undefined index value", span);
            else if (definition->second.value.type != IRType::Int)
                report("IR111", "array-element projection index must have type int", span);
            validate_use(place.index_value, use_block, use_position, span);
        }
        if (place.parent != 0u)
            validate_projected_place_index_uses(place.parent, use_block, use_position, span);
    }

    void validate_reachability_and_dominance() {
        if (blocks_.find(function_.entry_block) == blocks_.end()) return;

        std::unordered_set<IRBlockId> reachable;
        std::vector<IRBlockId> pending{function_.entry_block};
        while (!pending.empty()) {
            const IRBlockId current = pending.back();
            pending.pop_back();
            if (!reachable.insert(current).second) continue;
            const auto next = successors_.find(current);
            if (next != successors_.end()) {
                for (const IRBlockId target : next->second) pending.push_back(target);
            }
        }
        for (const auto& entry : blocks_) {
            if (reachable.find(entry.first) == reachable.end()) {
                report("IR036", "basic block is unreachable from the function entry",
                       entry.second->terminator ? entry.second->terminator->span : SourceSpan{});
            }
        }

        std::unordered_map<IRBlockId, std::unordered_set<IRBlockId>> predecessors;
        for (const auto& edge : successors_) {
            for (const IRBlockId target : edge.second) {
                predecessors[target].insert(edge.first);
            }
        }

        for (const IRBlockId block : reachable) {
            if (block == function_.entry_block) {
                dominators_[block].insert(block);
            } else {
                dominators_[block] = reachable;
            }
        }

        bool changed = true;
        while (changed) {
            changed = false;
            for (const IRBlockId block : reachable) {
                if (block == function_.entry_block) continue;
                std::unordered_set<IRBlockId> next = reachable;
                const auto pred = predecessors.find(block);
                if (pred == predecessors.end() || pred->second.empty()) {
                    next.clear();
                } else {
                    for (const IRBlockId predecessor : pred->second) {
                        const auto dom = dominators_.find(predecessor);
                        if (dom == dominators_.end()) {
                            next.clear();
                            break;
                        }
                        for (auto item = next.begin(); item != next.end();) {
                            if (dom->second.find(*item) == dom->second.end()) {
                                item = next.erase(item);
                            } else {
                                ++item;
                            }
                        }
                    }
                }
                next.insert(block);
                if (next != dominators_[block]) {
                    dominators_[block] = std::move(next);
                    changed = true;
                }
            }
        }

        for (const IRBlock& block : function_.blocks) {
            for (std::size_t index = 0; index < block.instructions.size(); ++index) {
                const IRInstruction& instruction = block.instructions[index];
                for (const IRValueId operand : instruction.operands) {
                    validate_use(operand, block.id, static_cast<std::int64_t>(index),
                                 instruction.span);
                }
                if (instruction.place &&
                    (instruction.op == IROp::Load || instruction.op == IROp::Store ||
                     instruction.op == IROp::AddressOf))
                    validate_projected_place_index_uses(
                        *instruction.place, block.id, static_cast<std::int64_t>(index),
                        instruction.span);
            }
            if (!block.terminator) continue;
            const std::int64_t terminator_position =
                static_cast<std::int64_t>(block.instructions.size());
            std::visit([&](const auto& terminator) {
                using T = std::decay_t<decltype(terminator)>;
                if constexpr (std::is_same_v<T, IRReturn>) {
                    if (terminator.value)
                        validate_use(*terminator.value, block.id, terminator_position,
                                     block.terminator->span);
                } else if constexpr (std::is_same_v<T, IRBranch>) {
                    for (const IRValueId argument : terminator.arguments)
                        validate_use(argument, block.id, terminator_position,
                                     block.terminator->span);
                } else if constexpr (std::is_same_v<T, IRConditionalBranch>) {
                    validate_use(terminator.condition, block.id, terminator_position,
                                 block.terminator->span);
                    for (const IRValueId argument : terminator.true_arguments)
                        validate_use(argument, block.id, terminator_position,
                                     block.terminator->span);
                    for (const IRValueId argument : terminator.false_arguments)
                        validate_use(argument, block.id, terminator_position,
                                     block.terminator->span);
                }
            }, block.terminator->data);
        }
    }

    const IRFunction& function_;
    const std::unordered_map<std::string, FunctionSignature>& functions_;
    const std::vector<IRStructType>& struct_types_;
    const std::vector<IREnumType>& enum_types_;
    std::vector<Diagnostic>& diagnostics_;
    std::unordered_map<IRBlockId, const IRBlock*> blocks_;
    std::unordered_map<IRPlaceId, const IRPlace*> places_;
    std::unordered_map<IRValueId, Definition> definitions_;
    std::unordered_map<IRBlockId, std::vector<IRBlockId>> successors_;
    std::unordered_map<IRBlockId, std::unordered_set<IRBlockId>> dominators_;
};

void print_value(std::ostream& output, const IRValueId value) {
    output << '%' << value;
}

void print_arguments(std::ostream& output, const std::vector<IRValueId>& arguments) {
    output << '(';
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (index != 0) output << ", ";
        print_value(output, arguments[index]);
    }
    output << ')';
}

} // namespace

const char* ir_type_name(const IRType type) noexcept {
    switch (type) {
    case IRType::Void: return "void";
    case IRType::Int: return "int";
    case IRType::Byte: return "byte";
    case IRType::Float: return "float";
    case IRType::Bool: return "bool";
    case IRType::String: return "string";
    case IRType::Array: return "array";
    case IRType::Struct: return "struct";
    case IRType::Enum: return "enum";
    case IRType::Pointer: return "pointer";
    }
    return "unknown";
}

const char* ir_op_name(const IROp op) noexcept {
    switch (op) {
    case IROp::Constant: return "const";
    case IROp::Add: return "add";
    case IROp::StringConcat: return "string_concat";
    case IROp::StringLength: return "string_length";
    case IROp::StringJoin: return "string_join";
    case IROp::ByteSlice: return "byte_slice";
    case IROp::ByteRead: return "byte_read";
    case IROp::ByteWrite: return "byte_write";
    case IROp::FileRead: return "file_read";
    case IROp::FileWrite: return "file_write";
    case IROp::Subtract: return "sub";
    case IROp::Multiply: return "mul";
    case IROp::Divide: return "div";
    case IROp::Remainder: return "rem";
    case IROp::Negate: return "neg";
    case IROp::LogicalNot: return "not";
    case IROp::LogicalAnd: return "and";
    case IROp::LogicalOr: return "or";
    case IROp::BitwiseAnd: return "bit_and";
    case IROp::BitwiseOr: return "bit_or";
    case IROp::BitwiseXor: return "bit_xor";
    case IROp::BitwiseNot: return "bit_not";
    case IROp::ShiftLeft: return "shl";
    case IROp::ShiftRight: return "shr";
    case IROp::RotateLeft: return "rotl";
    case IROp::RotateRight: return "rotr";
    case IROp::Equal: return "eq";
    case IROp::NotEqual: return "ne";
    case IROp::Less: return "lt";
    case IROp::LessEqual: return "le";
    case IROp::Greater: return "gt";
    case IROp::GreaterEqual: return "ge";
    case IROp::IntToFloat: return "int_to_float";
    case IROp::IntToByteChecked: return "int_to_byte_checked";
    case IROp::ByteToInt: return "byte_to_int";
    case IROp::Call: return "call";
    case IROp::Math: return "math";
    case IROp::Say: return "say";
    case IROp::ArrayMake: return "array_make";
    case IROp::ArrayGet: return "array_get";
    case IROp::ArraySet: return "array_set";
    case IROp::ArrayLength: return "array_length";
    case IROp::ArrayCapacity: return "array_capacity";
    case IROp::ArrayReserve: return "array_reserve";
    case IROp::ArrayPush: return "array_push";
    case IROp::ArrayPop: return "array_pop";
    case IROp::StructMake: return "struct_make";
    case IROp::StructGet: return "struct_get";
    case IROp::StructSet: return "struct_set";
    case IROp::EnumMake: return "enum_make";
    case IROp::EnumTag: return "enum_tag";
    case IROp::EnumPayloadGet: return "enum_payload_get";
    case IROp::Load: return "load";
    case IROp::Store: return "store";
    case IROp::AddressOf: return "address_of";
    }
    return "unknown";
}

bool IRVerificationResult::has_errors() const noexcept {
    return std::any_of(diagnostics.begin(), diagnostics.end(),
        [](const Diagnostic& diagnostic) {
            return diagnostic.severity == DiagnosticSeverity::Error;
        });
}

namespace {
bool value_has_nested_fixed_storage_impl(
    const IRValue& value, const IRModule& module,
    std::unordered_set<std::uint32_t>& active_structs) {
    if (value.type == IRType::Array) {
        const auto dimensions = ir_array_dimensions(value);
        std::size_t prefix = 0;
        while (prefix < dimensions.size() && dimensions[prefix] != 0u) ++prefix;
        if (prefix >= 2u) return true;
        if (prefix == 0u) return false;
        return value_has_nested_fixed_storage_impl(
            ir_array_element_value(value), module, active_structs);
    }
    if (value.type != IRType::Struct || value.struct_type_id == 0u ||
        value.struct_type_id > module.struct_types.size()) return false;
    if (!active_structs.insert(value.struct_type_id).second) return false;
    const IRStructType& structure = module.struct_types[value.struct_type_id - 1u];
    for (const IRStructField& field : structure.fields) {
        if (value_has_nested_fixed_storage_impl(
                ir_struct_field_value(field), module, active_structs)) {
            active_structs.erase(value.struct_type_id);
            return true;
        }
    }
    active_structs.erase(value.struct_type_id);
    return false;
}
} // namespace

bool ir_value_has_nested_fixed_storage(const IRValue& value, const IRModule& module) {
    std::unordered_set<std::uint32_t> active;
    return value_has_nested_fixed_storage_impl(value, module, active);
}

std::vector<IRValue> ir_function_nested_fixed_stack_values(
    const IRFunction& function, const IRModule& module) {
    std::vector<IRValue> result;
    const auto add = [&](const IRValue& value) {
        if (ir_value_has_nested_fixed_storage(value, module)) result.push_back(value);
    };
    // Only root places own stack storage; projections are address descriptors.
    for (const IRPlace& place : function.places)
        if (place.projection == IRPlaceProjectionKind::Root) add(place.value_type);
    std::unordered_map<IRValueId, IRValue> known;
    for (const auto& parameter : function.parameters) {
        known.emplace(parameter.value.id, parameter.value);
        add(parameter.value);
    }
    for (const IRBlock& block : function.blocks) {
        for (const IRValue& parameter : block.parameters) {
            known.emplace(parameter.id, parameter);
            add(parameter);
        }
        for (const IRInstruction& instruction : block.instructions) {
            if (instruction.result) {
                known.emplace(instruction.result->id, *instruction.result);
                add(*instruction.result);
            }
        }
    }
    // Count C aggregate temporaries in both backends for identical source limits.
    for (const IRBlock& block : function.blocks) {
        for (const IRInstruction& instruction : block.instructions) {
            if (instruction.op == IROp::ArrayMake && instruction.result &&
                instruction.result->type == IRType::Array) {
                const auto dims = ir_array_dimensions(*instruction.result);
                if (!dims.empty() && dims.front() == 0u) {
                    const IRValue element = ir_array_element_value(*instruction.result);
                    for (std::size_t i = 0; i < instruction.operands.size(); ++i) add(element);
                }
            } else if ((instruction.op == IROp::ArraySet || instruction.op == IROp::ArrayPush) &&
                       !instruction.operands.empty()) {
                const auto found = known.find(instruction.operands.front());
                if (found == known.end() || found->second.type != IRType::Array) continue;
                const auto dims = ir_array_dimensions(found->second);
                if (dims.empty() || dims.front() != 0u) continue;
                add(ir_array_element_value(found->second));
            }
        }
    }
    const auto add_edges = [&](IRBlockId target, const std::vector<IRValueId>& args) {
        const auto found = std::find_if(function.blocks.begin(), function.blocks.end(),
            [target](const IRBlock& block) { return block.id == target; });
        if (found == function.blocks.end()) return;
        for (std::size_t i = 0; i < std::min(args.size(), found->parameters.size()); ++i)
            add(found->parameters[i]);
    };
    for (const IRBlock& block : function.blocks) {
        if (!block.terminator) continue;
        std::visit([&](const auto& term) {
            using T = std::decay_t<decltype(term)>;
            if constexpr (std::is_same_v<T, IRBranch>) add_edges(term.target, term.arguments);
            else if constexpr (std::is_same_v<T, IRConditionalBranch>) {
                add_edges(term.true_target, term.true_arguments);
                add_edges(term.false_target, term.false_arguments);
            }
        }, block.terminator->data);
    }
    IRValue returned;
    if (function.return_type == IRType::Struct) {
        returned.type = IRType::Struct;
        returned.struct_type_id = function.return_struct_type_id;
        add(returned);
    } else if (function.return_type == IRType::Enum) {
        returned.type = IRType::Enum;
        returned.enum_type_id = function.return_enum_type_id;
        add(returned);
    } else if (function.return_type == IRType::Array) {
        auto dims = function.return_array_dimensions;
        if (dims.empty()) dims.push_back(function.return_array_length);
        returned = ir_make_array_value(function.return_element_type, std::move(dims),
            function.return_element_struct_type_id, function.span,
            function.return_element_enum_type_id);
        add(returned);
    }
    return result;
}

IRVerificationResult IRVerifier::verify(const IRModule& module) const {
    IRVerificationResult result;
    std::unordered_map<std::string, FunctionSignature> functions;
    std::unordered_map<std::uint32_t, bool> structure_ids;
    std::unordered_map<std::string, bool> structure_names;
    for (std::size_t structure_index = 0;
         structure_index < module.struct_types.size(); ++structure_index) {
        const IRStructType& structure = module.struct_types[structure_index];
        if (structure.id != structure_index + 1u ||
            !structure_ids.emplace(structure.id, true).second ||
            structure.name.empty() || !structure_names.emplace(structure.name, true).second) {
            result.diagnostics.push_back(Diagnostic{DiagnosticSeverity::Error, "IR069",
                "structure type declarations must have unique nonzero IDs and names", {}});
            continue;
        }
        std::unordered_map<std::string, bool> fields;
        if (structure.fields.empty())
            result.diagnostics.push_back(Diagnostic{DiagnosticSeverity::Error, "IR069",
                "structure types must contain at least one field", {}});
        for (const IRStructField& field : structure.fields) {
            const IRValue field_type = ir_struct_field_value(field);
            const std::vector<std::uint32_t> field_dimensions =
                field.type == IRType::Array ? ir_array_dimensions(field_type)
                                            : std::vector<std::uint32_t>{};
            const bool field_has_dynamic_dimension =
                std::find(field_dimensions.begin(), field_dimensions.end(), 0u) !=
                field_dimensions.end();
            const bool invalid_array =
                field.type == IRType::Array &&
                (!valid_array_descriptor(field_type, module.struct_types.size(),
                                          module.enum_types.size()) ||
                 (field.element_type == IRType::Struct &&
                  (field.element_struct_type_id > structure.id ||
                   (!field_has_dynamic_dimension &&
                    field.element_struct_type_id >= structure.id))));
            const bool invalid_non_array_shape =
                field.type != IRType::Array &&
                (field.element_type != IRType::Void || field.array_length != 0 ||
                 field.element_struct_type_id != 0 || !field.array_dimensions.empty());
            const bool invalid_enum_identity =
                field.type == IRType::Enum
                    ? (field.enum_type_id == 0u || field.enum_type_id > module.enum_types.size())
                    : field.enum_type_id != 0u;
            if (field.name.empty() || !fields.emplace(field.name, true).second ||
                field.type == IRType::Void ||
                (field.type == IRType::Struct &&
                 (field.struct_type_id == 0 || field.struct_type_id >= structure.id)) ||
                (field.type != IRType::Struct && field.struct_type_id != 0) ||
                invalid_array || invalid_non_array_shape || invalid_enum_identity) {
                result.diagnostics.push_back(Diagnostic{DiagnosticSeverity::Error, "IR070",
                    "structure field has an invalid name or unsupported aggregate shape", {}});
            }
        }
    }
    for (std::size_t enum_index = 0; enum_index < module.enum_types.size(); ++enum_index) {
        const IREnumType& enumeration = module.enum_types[enum_index];
        if (enumeration.id != enum_index + 1u || enumeration.name.empty() ||
            !structure_names.emplace(enumeration.name, true).second ||
            enumeration.variants.empty()) {
            result.diagnostics.push_back(Diagnostic{DiagnosticSeverity::Error, "IR082",
                "enum type declarations must have canonical nonzero IDs, unique names, and at least one variant", {}});
            continue;
        }

        std::unordered_map<std::string, bool> variant_names;
        for (const IREnumVariant& variant : enumeration.variants) {
            if (variant.name.empty() || !variant_names.emplace(variant.name, true).second) {
                result.diagnostics.push_back(Diagnostic{DiagnosticSeverity::Error, "IR083",
                    "enum variant names must be nonempty and unique within their enum", {}});
                continue;
            }
            std::unordered_map<std::string, bool> payload_names;
            for (const IRStructField& field : variant.fields) {
                const IRValue field_value = ir_struct_field_value(field);
                const bool invalid_array = field.type == IRType::Array &&
                    !valid_array_descriptor(field_value, module.struct_types.size(),
                                            module.enum_types.size());
                const bool invalid_non_array_shape = field.type != IRType::Array &&
                    (field.element_type != IRType::Void || field.array_length != 0u ||
                     field.element_struct_type_id != 0u || !field.array_dimensions.empty());
                const bool invalid_enum_identity = field.type == IRType::Enum
                    ? (field.enum_type_id == 0u || field.enum_type_id > module.enum_types.size())
                    : field.enum_type_id != 0u;
                const bool invalid_structure = field.type == IRType::Struct &&
                    (field.struct_type_id == 0u ||
                     field.struct_type_id > module.struct_types.size());
                const bool invalid_scalar = field.type != IRType::Array &&
                    field.type != IRType::Struct &&
                    field.type != IRType::Int && field.type != IRType::Byte &&
                    field.type != IRType::Float && field.type != IRType::Bool &&
                    field.type != IRType::String && field.type != IRType::Enum;
                if (field.name.empty() || !payload_names.emplace(field.name, true).second ||
                    field.type == IRType::Void || invalid_array || invalid_non_array_shape ||
                    invalid_structure || invalid_scalar || invalid_enum_identity ||
                    (field.type != IRType::Struct && field.struct_type_id != 0u)) {
                    result.diagnostics.push_back(Diagnostic{DiagnosticSeverity::Error, "IR084",
                        "enum variant payload field has an invalid name, type, or aggregate shape", {}});
                }
            }
        }
    }
    for (const IRFunction& function : module.functions) {
        if (function.name.empty()) {
            result.diagnostics.push_back(Diagnostic{
                DiagnosticSeverity::Error, "IR037", "function name cannot be empty",
                function.span
            });
            continue;
        }
        if (structure_names.find(function.name) != structure_names.end()) {
            result.diagnostics.push_back(Diagnostic{
                DiagnosticSeverity::Error, "IR085",
                "function name '" + function.name + "' conflicts with a nominal type name",
                function.span
            });
        }
        FunctionSignature signature;
        signature.return_type = function.return_type;
        signature.return_element_type = function.return_element_type;
        signature.return_array_length = function.return_array_length;
        signature.return_struct_type_id = function.return_struct_type_id;
        signature.return_enum_type_id = function.return_enum_type_id;
        signature.return_element_struct_type_id =
            function.return_element_struct_type_id;
        signature.return_element_enum_type_id =
            function.return_element_enum_type_id;
        signature.return_array_dimensions = function.return_array_dimensions;
        signature.return_pointer_pointee_type = function.return_pointer_pointee_type;
        signature.return_pointer_pointee_struct_type_id =
            function.return_pointer_pointee_struct_type_id;
        signature.return_pointer_pointee_enum_type_id =
            function.return_pointer_pointee_enum_type_id;
        for (const IRFunctionParameter& parameter : function.parameters) {
            signature.parameters.push_back(parameter.value);
        }
        if (!functions.emplace(function.name, std::move(signature)).second) {
            result.diagnostics.push_back(Diagnostic{
                DiagnosticSeverity::Error, "IR038",
                "duplicate function name '" + function.name + "'", function.span
            });
        }
    }

    for (const IRFunction& function : module.functions) {
        FunctionVerifier(function, functions, module.struct_types, module.enum_types,
                          result.diagnostics).run();
    }
    return result;
}

void print_ir(std::ostream& output, const IRModule& module) {
    for (const IRStructType& structure : module.struct_types) {
        output << "struct " << structure.name << " {\n";
        for (const IRStructField& field : structure.fields)
            output << "  " << field.name << ": " << ir_type_name(field.type) << '\n';
        output << "}\n";
    }
    for (const IRFunction& function : module.functions) {
        output << "func " << function.name << '(';
        for (std::size_t index = 0; index < function.parameters.size(); ++index) {
            if (index != 0) output << ", ";
            print_value(output, function.parameters[index].value.id);
            output << ": " << ir_type_name(function.parameters[index].value.type);
        }
        output << ") -> " << ir_type_name(function.return_type) << " {\n";
        if (!function.places.empty()) {
            output << "  places:\n";
            for (const IRPlace& place : function.places) {
                output << "    place#" << place.id << ' ';
                if (place.projection == IRPlaceProjectionKind::Root) {
                    output << "root";
                } else if (place.projection == IRPlaceProjectionKind::StructField) {
                    output << "field#" << place.field_index << " of place#" << place.parent;
                } else if (place.projection == IRPlaceProjectionKind::ArrayElement) {
                    output << "element of place#" << place.parent << " at ";
                    print_value(output, place.index_value);
                } else {
                    output << "unknown projection";
                }
                output << ": ";
                const IRValue& type = place.value_type;
                if (type.type == IRType::Array) {
                    output << "array<" << ir_type_name(type.element_type);
                    if (type.element_type == IRType::Struct)
                        output << '#' << type.element_struct_type_id;
                    else if (type.element_type == IRType::Enum)
                        output << '#' << type.enum_type_id;
                    output << '>';
                    for (const std::uint32_t dimension : ir_array_dimensions(type)) {
                        output << '[';
                        if (dimension == 0u) output << '?';
                        else output << dimension;
                        output << ']';
                    }
                } else {
                    output << ir_type_name(type.type);
                    if (type.type == IRType::Struct) output << '#' << type.struct_type_id;
                    else if (type.type == IRType::Enum) output << '#' << type.enum_type_id;
                }
                output << (place.mutable_place ? " mutable" : " immutable") << '\n';
            }
        }
        for (const IRBlock& block : function.blocks) {
            output << "  bb" << block.id;
            if (!block.name.empty()) output << " [" << block.name << ']';
            output << '(';
            for (std::size_t index = 0; index < block.parameters.size(); ++index) {
                if (index != 0) output << ", ";
                print_value(output, block.parameters[index].id);
                output << ": " << ir_type_name(block.parameters[index].type);
            }
            output << "):\n";
            for (const IRInstruction& instruction : block.instructions) {
                output << "    ";
                if (instruction.result) {
                    print_value(output, instruction.result->id);
                    output << " = ";
                }
                output << ir_op_name(instruction.op);
                if (instruction.op == IROp::Constant) {
                    output << ' ' << ir_type_name(instruction.result
                        ? instruction.result->type : IRType::Void)
                           << ' ' << instruction.immediate;
                } else if (instruction.op == IROp::Call) {
                    output << ' ' << instruction.callee;
                } else if (instruction.op == IROp::StructGet || instruction.op == IROp::StructSet) {
                    output << ' ' << instruction.immediate;
                }
                if (instruction.place) output << " place#" << *instruction.place;
                for (const IRValueId operand : instruction.operands) {
                    output << ' ';
                    print_value(output, operand);
                }
                output << '\n';
            }
            if (!block.terminator) {
                output << "    <missing terminator>\n";
                continue;
            }
            output << "    ";
            std::visit([&](const auto& terminator) {
                using T = std::decay_t<decltype(terminator)>;
                if constexpr (std::is_same_v<T, IRReturn>) {
                    output << "return";
                    if (terminator.value) {
                        output << ' ';
                        print_value(output, *terminator.value);
                    }
                } else if constexpr (std::is_same_v<T, IRBranch>) {
                    output << "br bb" << terminator.target;
                    print_arguments(output, terminator.arguments);
                } else if constexpr (std::is_same_v<T, IRConditionalBranch>) {
                    output << "condbr ";
                    print_value(output, terminator.condition);
                    output << ", bb" << terminator.true_target;
                    print_arguments(output, terminator.true_arguments);
                    output << ", bb" << terminator.false_target;
                    print_arguments(output, terminator.false_arguments);
                } else if constexpr (std::is_same_v<T, IRUnreachable>) {
                    output << "unreachable";
                }
            }, block.terminator->data);
            output << '\n';
        }
        output << "}\n";
    }
}

} // namespace vex
