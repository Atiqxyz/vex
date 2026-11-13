#include "vex/ir.hpp"

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
};

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
                     std::vector<Diagnostic>& diagnostics)
        : function_(function), functions_(functions), struct_types_(struct_types),
          diagnostics_(diagnostics) {}

    void run() {
        index_blocks();
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
            if (function_.return_element_type == IRType::Void ||
                function_.return_element_type == IRType::Array ||
                function_.return_array_length > 65536u) {
                report("IR053", "function has an invalid fixed-array return shape",
                       function_.span);
            }
        } else if (function_.return_type == IRType::Struct) {
            if (function_.return_struct_type_id == 0 ||
                function_.return_struct_type_id > struct_types_.size())
                report("IR060", "function has an invalid structure return type", function_.span);
            if (function_.return_element_type != IRType::Void || function_.return_array_length != 0)
                report("IR054", "structure function return cannot carry array shape metadata", function_.span);
        } else if (function_.return_type == IRType::Void ||
                   function_.return_type == IRType::Int ||
                   function_.return_type == IRType::Float ||
                   function_.return_type == IRType::Bool ||
                   function_.return_type == IRType::String) {
            if (function_.return_element_type != IRType::Void ||
                function_.return_array_length != 0) {
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

    void add_definition(const IRValue& value, const IRBlockId block,
                        const std::int64_t position) {
        if (!is_value_type(value.type)) {
            report("IR006", "SSA values cannot have type void", value.span);
        }
        if (value.type == IRType::Array &&
            (value.element_type == IRType::Void || value.element_type == IRType::Array ||
             value.array_length > 65536u)) {
            report("IR050", "SSA array value has an invalid element type or fixed length",
                   value.span);
        } else if (value.type == IRType::Struct) {
            if (value.struct_type_id == 0 || value.struct_type_id > struct_types_.size())
                report("IR060", "SSA value has an invalid structure type", value.span);
            if (value.element_type != IRType::Void || value.array_length != 0)
                report("IR051", "structure SSA value cannot carry array shape metadata", value.span);
        } else if (value.type != IRType::Array &&
                   (value.element_type != IRType::Void || value.array_length != 0 ||
                    value.struct_type_id != 0)) {
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
        return left.type == IRType::Array && right.type == IRType::Array &&
               left.element_type == right.element_type &&
               left.array_length == right.array_length;
    }

    static bool same_value_shape(const IRValue& left, const IRValue& right) {
        if (left.type != right.type) return false;
        if (left.type == IRType::Array) return same_array_shape(left, right);
        if (left.type == IRType::Struct) return left.struct_type_id == right.struct_type_id;
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
        switch (instruction.op) {
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
            break;
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
        case IROp::NotEqual:
            check_arity(instruction, 2);
            if (left && right && (*left != *right ||
                !is_value_type(*left) || !is_value_type(*right))) {
                report("IR018", "equality operands must have the same non-void type",
                       instruction.span);
            }
            check_result(instruction, IRType::Bool);
            break;
        case IROp::Less:
        case IROp::LessEqual:
        case IROp::Greater:
        case IROp::GreaterEqual:
            check_arity(instruction, 2);
            if (left && right && (*left != *right || !is_numeric(*left))) {
                report("IR019", "relational operands must have the same numeric type",
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
                    (instruction.result->element_type != found->second.return_element_type ||
                     instruction.result->array_length != found->second.return_array_length)) {
                    report("IR055", "call result does not match the callee's fixed-array return shape",
                           instruction.span);
                }
            }
            break;
        }
        case IROp::Say:
            check_arity(instruction, 1);
            if (left && (!is_value_type(*left) || *left == IRType::Array)) {
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
            if (result.element_type == IRType::Void || result.element_type == IRType::Array ||
                result.array_length > 65536u)
                report("IR042", "array result has an invalid element type or length", result.span);
            if (result.array_length != 0 &&
                instruction.operands.size() != result.array_length)
                report("IR043", "array construction operand count does not match its fixed length", instruction.span);
            for (std::size_t index = 0; index < instruction.operands.size(); ++index) {
                const auto actual = operand_type(instruction, index);
                if (actual && *actual != result.element_type)
                    report("IR044", "array element operand has the wrong type", instruction.span);
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
                if (actual && (actual->type != fields[index].type ||
                    (actual->type == IRType::Struct && actual->struct_type_id != fields[index].struct_type_id)))
                    report("IR063", "structure construction field has the wrong type", instruction.span);
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
                if (value && (value->type != field->type ||
                    (value->type == IRType::Struct && value->struct_type_id != field->struct_type_id)))
                    report("IR063", "structure field update value has the wrong type", instruction.span);
                if (!instruction.result || instruction.result->type != IRType::Struct ||
                    instruction.result->struct_type_id != object->struct_type_id)
                    report("IR066", "structure field update must preserve the source structure type", instruction.span);
            } else {
                if (!instruction.result || instruction.result->type != field->type ||
                    (field->type == IRType::Struct && instruction.result->struct_type_id != field->struct_type_id))
                    report("IR067", "structure field read result has the wrong type", instruction.span);
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
            if (array) check_result(instruction, array->element_type);
            else if (!instruction.result)
                report("IR009", "array indexing must define a result", instruction.span);
            break;
        }
        case IROp::ArraySet: {
            check_arity(instruction, 3);
            const IRValue* array = operand_value(instruction, 0);
            const auto index_type = operand_type(instruction, 1);
            const auto value_type = operand_type(instruction, 2);
            if (array && array->type != IRType::Array)
                report("IR047", "array update requires an array operand", instruction.span);
            if (index_type && *index_type != IRType::Int)
                report("IR046", "array index operand must have type int", instruction.span);
            if (array && value_type && *value_type != array->element_type)
                report("IR048", "array update value has the wrong element type", instruction.span);
            if (array && instruction.result && !same_array_shape(*array, *instruction.result))
                report("IR049", "array update result must preserve the source array shape", instruction.span);
            else if (array && !instruction.result)
                report("IR009", "array update must define a result", instruction.span);
            break;
        }
        case IROp::ArrayLength: {
            check_arity(instruction, 1);
            const IRValue* array = operand_value(instruction, 0);
            if (array && (array->type != IRType::Array || array->array_length != 0))
                report("IR056", "dynamic array length requires a dynamic-array operand", instruction.span);
            check_result(instruction, IRType::Int);
            break;
        }
        case IROp::ArrayPush: {
            check_arity(instruction, 2);
            const IRValue* array = operand_value(instruction, 0);
            const auto value_type = operand_type(instruction, 1);
            if (array && (array->type != IRType::Array || array->array_length != 0))
                report("IR057", "array push requires a dynamic array operand", instruction.span);
            if (array && value_type && *value_type != array->element_type)
                report("IR058", "array push value has the wrong element type", instruction.span);
            check_no_result(instruction);
            break;
        }
        case IROp::ArrayPop: {
            check_arity(instruction, 1);
            const IRValue* array = operand_value(instruction, 0);
            if (array && (array->type != IRType::Array || array->array_length != 0))
                report("IR059", "array pop requires a dynamic array operand", instruction.span);
            if (array) check_result(instruction, array->element_type);
            else if (!instruction.result)
                report("IR009", "array pop must define a result", instruction.span);
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
                (value->second.value.type != parameters[index].type ||
                 (parameters[index].type == IRType::Array &&
                  !same_array_shape(value->second.value, parameters[index])))) {
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
                             (returned.element_type != function_.return_element_type ||
                              returned.array_length != function_.return_array_length)) ||
                            (function_.return_type == IRType::Struct &&
                             returned.struct_type_id != function_.return_struct_type_id)) {
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
    std::vector<Diagnostic>& diagnostics_;
    std::unordered_map<IRBlockId, const IRBlock*> blocks_;
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
    case IRType::Float: return "float";
    case IRType::Bool: return "bool";
    case IRType::String: return "string";
    case IRType::Array: return "array";
    case IRType::Struct: return "struct";
    }
    return "unknown";
}

const char* ir_op_name(const IROp op) noexcept {
    switch (op) {
    case IROp::Constant: return "const";
    case IROp::Add: return "add";
    case IROp::Subtract: return "sub";
    case IROp::Multiply: return "mul";
    case IROp::Divide: return "div";
    case IROp::Remainder: return "rem";
    case IROp::Negate: return "neg";
    case IROp::LogicalNot: return "not";
    case IROp::LogicalAnd: return "and";
    case IROp::LogicalOr: return "or";
    case IROp::Equal: return "eq";
    case IROp::NotEqual: return "ne";
    case IROp::Less: return "lt";
    case IROp::LessEqual: return "le";
    case IROp::Greater: return "gt";
    case IROp::GreaterEqual: return "ge";
    case IROp::IntToFloat: return "int_to_float";
    case IROp::Call: return "call";
    case IROp::Math: return "math";
    case IROp::Say: return "say";
    case IROp::ArrayMake: return "array_make";
    case IROp::ArrayGet: return "array_get";
    case IROp::ArraySet: return "array_set";
    case IROp::ArrayLength: return "array_length";
    case IROp::ArrayPush: return "array_push";
    case IROp::ArrayPop: return "array_pop";
    case IROp::StructMake: return "struct_make";
    case IROp::StructGet: return "struct_get";
    case IROp::StructSet: return "struct_set";
    }
    return "unknown";
}

bool IRVerificationResult::has_errors() const noexcept {
    return std::any_of(diagnostics.begin(), diagnostics.end(),
        [](const Diagnostic& diagnostic) {
            return diagnostic.severity == DiagnosticSeverity::Error;
        });
}

IRVerificationResult IRVerifier::verify(const IRModule& module) const {
    IRVerificationResult result;
    std::unordered_map<std::string, FunctionSignature> functions;
    std::unordered_map<std::uint32_t, bool> structure_ids;
    std::unordered_map<std::string, bool> structure_names;
    for (const IRStructType& structure : module.struct_types) {
        if (structure.id == 0 || structure.id > module.struct_types.size() ||
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
            if (field.name.empty() || !fields.emplace(field.name, true).second ||
                field.type == IRType::Void || field.type == IRType::Array ||
                (field.type == IRType::Struct &&
                 (field.struct_type_id == 0 || field.struct_type_id >= structure.id)) ||
                (field.type != IRType::Struct && field.struct_type_id != 0)) {
                result.diagnostics.push_back(Diagnostic{DiagnosticSeverity::Error, "IR070",
                    "structure field has an invalid name or unsupported type", {}});
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
        FunctionSignature signature;
        signature.return_type = function.return_type;
        signature.return_element_type = function.return_element_type;
        signature.return_array_length = function.return_array_length;
        signature.return_struct_type_id = function.return_struct_type_id;
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
        FunctionVerifier(function, functions, module.struct_types, result.diagnostics).run();
    }
    return result;
}

void print_ir(std::ostream& output, const IRModule& module) {
    for (const IRStructType& structure : module.struct_types) {
        output << "struct " << structure.name << " {\n";
        for (const IRStructField& field : structure.fields)
            output << "  " << field.name << ": " << ir_type_name(field.type) << '\\n';
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
