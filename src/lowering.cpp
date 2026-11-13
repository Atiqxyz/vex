#include "vex/lowering.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vex {
namespace {

using Environment = std::unordered_map<SemanticBindingId, IRValue>;

std::optional<IRType> to_ir_type(const SemanticType type) {
    switch (type) {
    case SemanticType::Void: return IRType::Void;
    case SemanticType::Int: return IRType::Int;
    case SemanticType::Float: return IRType::Float;
    case SemanticType::Bool: return IRType::Bool;
    case SemanticType::String: return IRType::String;
    case SemanticType::Array: return IRType::Array;
    case SemanticType::Error:
    case SemanticType::Unknown:
    case SemanticType::Any:
    case SemanticType::Range:
        return std::nullopt;
    }
    return std::nullopt;
}

bool is_numeric(const IRType type) {
    return type == IRType::Int || type == IRType::Float;
}

class FunctionLowerer {
public:
    FunctionLowerer(const SourceFile& source, const SemanticModel& semantic_model,
                    const std::unordered_map<std::string, SemanticFunctionSignature>& signatures,
                    const FunctionDeclStmt& declaration,
                    std::vector<Diagnostic>& diagnostics,
                    const std::vector<const Statement*>* implicit_body = nullptr)
        : source_(source), semantic_model_(semantic_model), signatures_(signatures),
          declaration_(declaration), diagnostics_(diagnostics),
          implicit_body_(implicit_body) {}

    IRFunction lower() {
        const bool is_implicit_entry = implicit_body_ != nullptr;
        function_.name = is_implicit_entry
            ? "main" : std::string(source_.slice(declaration_.name));
        function_.span = is_implicit_entry ? SourceSpan{} : declaration_.name;

        if (is_implicit_entry) {
            function_.return_type = IRType::Void;
        } else {
            const auto signature = semantic_model_.functions.find(&declaration_);
            if (signature == semantic_model_.functions.end()) {
                report("IR100", "function is missing its semantic signature",
                       declaration_.name);
                function_.return_type = IRType::Void;
            } else {
                const auto return_type = to_ir_type(signature->second.return_type);
                if (!return_type) {
                    report("IR101", "function has a type that cannot be represented in the initial IR",
                           declaration_.name);
                    function_.return_type = IRType::Void;
                } else {
                    function_.return_type = *return_type;
                    if (function_.return_type == IRType::Array) {
                        if (!signature->second.return_array_type) {
                            report("IR101", "array return type is missing its fixed shape",
                                   declaration_.name);
                        } else {
                            const auto element_type =
                                to_ir_type(signature->second.return_array_type->element_type);
                            if (!element_type || *element_type == IRType::Array ||
                                *element_type == IRType::Void) {
                                report("IR101", "array return element type cannot be represented in IR",
                                       declaration_.name);
                            } else {
                                function_.return_element_type = *element_type;
                                function_.return_array_length =
                                    signature->second.return_array_type->length;
                            }
                        }
                    }
                }
            }
        }

        const IRBlockId entry = create_block("entry");
        function_.entry_block = entry;
        current_block_ = entry;

        for (const Parameter& parameter : declaration_.parameters) {
            const auto binding = semantic_model_.parameter_bindings.find(&parameter);
            if (binding == semantic_model_.parameter_bindings.end()) {
                report("IR102", "function parameter has no resolved binding",
                       parameter.name);
                continue;
            }
            const SemanticBinding* info = get_binding(binding->second);
            if (!info) {
                report("IR103", "function parameter binding metadata is missing",
                       parameter.name);
                continue;
            }
            const auto type = to_ir_type(info->type);
            if (!type || *type == IRType::Void) {
                report("IR104", "function parameter type cannot be represented in IR",
                       parameter.name);
                continue;
            }
            IRValue value = make_value(*type, parameter.name);
            if (*type == IRType::Array) {
                if (!info->array_type) {
                    report("IR104", "array parameter is missing its fixed shape",
                           parameter.name);
                    continue;
                }
                const auto element_type = to_ir_type(info->array_type->element_type);
                if (!element_type || *element_type == IRType::Array ||
                    *element_type == IRType::Void) {
                    report("IR104", "array parameter element type cannot be represented in IR",
                           parameter.name);
                    continue;
                }
                value = make_array_value(*element_type, info->array_type->length,
                                         parameter.name);
            }
            function_.parameters.push_back(IRFunctionParameter{
                std::string(source_.slice(parameter.name)), value
            });
            environment_[binding->second] = value;
        }

        if (implicit_body_) {
            for (const Statement* statement : *implicit_body_) {
                if (statement && current_block_) lower_statement(*statement);
            }
        } else if (declaration_.body) {
            lower_statement(*declaration_.body);
        }

        if (current_block_) {
            if (function_.return_type == IRType::Void) {
                set_terminator(*current_block_, IRTerminator{IRReturn{}, function_.span});
            } else {
                report("IR105", "non-void function reaches the end without a return",
                       function_.span);
                set_terminator(*current_block_,
                               IRTerminator{IRUnreachable{}, function_.span});
            }
        }

        return std::move(function_);
    }

private:
    struct FlowPath {
        IRBlockId block{0};
        Environment environment;
    };

    struct LoopContext {
        IRBlockId continue_target{0};
        IRBlockId break_target{0};
        std::vector<SemanticBindingId> environment_ids;
        std::optional<IRValue> counter;
        bool increment_counter_on_continue{true};
    };

    void report(const char* code, std::string message, const SourceSpan span) {
        diagnostics_.push_back(Diagnostic{
            DiagnosticSeverity::Error, code, std::move(message), span
        });
    }

    [[nodiscard]] std::string spelling(const SourceSpan span) const {
        return std::string(source_.slice(span));
    }

    [[nodiscard]] const SemanticBinding* get_binding(
        const SemanticBindingId id) const {
        const auto found = semantic_model_.bindings.find(id);
        return found == semantic_model_.bindings.end() ? nullptr : &found->second;
    }

    [[nodiscard]] std::optional<IRType> expression_type(const Expr& expression) {
        const auto found = semantic_model_.expression_types.find(&expression);
        if (found == semantic_model_.expression_types.end()) {
            report("IR106", "expression is missing semantic type information",
                   expression.span);
            return std::nullopt;
        }
        const auto type = to_ir_type(found->second);
        if (!type) {
            report("IR107", "expression type cannot be represented in the initial IR",
                   expression.span);
        }
        return type;
    }

    [[nodiscard]] IRValue make_value(const IRType type, const SourceSpan span) {
        return IRValue{next_value_id_++, type, span};
    }

    [[nodiscard]] IRValue make_array_value(const IRType element_type,
                                           const std::uint32_t length,
                                           const SourceSpan span) {
        IRValue value = make_value(IRType::Array, span);
        value.element_type = element_type;
        value.array_length = length;
        return value;
    }

    [[nodiscard]] IRValue make_value_like(const IRValue& source,
                                          const SourceSpan span) {
        IRValue value = make_value(source.type, span);
        value.element_type = source.element_type;
        value.array_length = source.array_length;
        return value;
    }

    [[nodiscard]] IRBlockId create_block(std::string name,
                                          std::vector<IRValue> parameters = {}) {
        const IRBlockId id = next_block_id_++;
        IRBlock block;
        block.id = id;
        block.name = std::move(name);
        block.parameters = std::move(parameters);
        function_.blocks.push_back(std::move(block));
        return id;
    }

    [[nodiscard]] IRBlock* find_block(const IRBlockId id) {
        const auto found = std::find_if(function_.blocks.begin(), function_.blocks.end(),
            [id](const IRBlock& block) { return block.id == id; });
        return found == function_.blocks.end() ? nullptr : &*found;
    }

    void set_terminator(const IRBlockId block_id, IRTerminator terminator) {
        IRBlock* block = find_block(block_id);
        if (!block) {
            report("IR108", "attempted to terminate a block that does not exist",
                   terminator.span);
            return;
        }
        if (block->terminator) {
            report("IR109", "basic block already has a terminator", terminator.span);
            return;
        }
        block->terminator = std::move(terminator);
    }

    void terminate_current(IRTerminator terminator) {
        if (!current_block_) return;
        set_terminator(*current_block_, std::move(terminator));
        current_block_.reset();
    }

    void emit(IRInstruction instruction) {
        if (!current_block_) {
            report("IR110", "cannot emit an instruction after a terminator",
                   instruction.span);
            return;
        }
        IRBlock* block = find_block(*current_block_);
        if (!block) {
            report("IR108", "current basic block does not exist", instruction.span);
            return;
        }
        block->instructions.push_back(std::move(instruction));
    }

    [[nodiscard]] IRValue emit_constant(const IRType type,
                                        const std::string& literal,
                                        const SourceSpan span) {
        const IRValue result = make_value(type, span);
        IRInstruction instruction;
        instruction.op = IROp::Constant;
        instruction.result = result;
        instruction.immediate = literal;
        instruction.span = span;
        emit(std::move(instruction));
        return result;
    }

    [[nodiscard]] IRValue emit_operation(const IROp op, const IRType result_type,
                                         const std::vector<IRValueId>& operands,
                                         const SourceSpan span) {
        const IRValue result = make_value(result_type, span);
        IRInstruction instruction;
        instruction.op = op;
        instruction.result = result;
        instruction.operands = operands;
        instruction.span = span;
        emit(std::move(instruction));
        return result;
    }

    [[nodiscard]] std::optional<IRValue> convert(IRValue value,
                                                  const IRType target,
                                                  const SourceSpan span) {
        if (value.type == target) return value;
        if (value.type == IRType::Int && target == IRType::Float) {
            return emit_operation(IROp::IntToFloat, IRType::Float,
                                  {value.id}, span);
        }
        report("IR111", "cannot convert " + std::string(ir_type_name(value.type)) +
               " to " + ir_type_name(target) + " during IR lowering", span);
        return std::nullopt;
    }

    [[nodiscard]] IRValue current_value(const SemanticBindingId id,
                                        const SourceSpan span) {
        const auto found = environment_.find(id);
        if (found == environment_.end()) {
            report("IR112", "binding '" +
                   (get_binding(id) ? get_binding(id)->name : std::string("<unknown>")) +
                   "' has no current SSA value", span);
            return IRValue{};
        }
        return found->second;
    }

    [[nodiscard]] std::vector<SemanticBindingId> sorted_environment_ids(
        const Environment& environment) const {
        std::vector<SemanticBindingId> ids;
        ids.reserve(environment.size());
        for (const auto& entry : environment) ids.push_back(entry.first);
        std::sort(ids.begin(), ids.end());
        return ids;
    }

    [[nodiscard]] IRValue environment_value(const Environment& environment,
                                             const SemanticBindingId id,
                                             const SourceSpan span) {
        const auto found = environment.find(id);
        if (found != environment.end()) return found->second;
        const auto base = environment_.find(id);
        if (base != environment_.end()) return base->second;
        report("IR113", "control-flow path lost a live binding", span);
        return IRValue{};
    }

    void merge_paths(const Environment& base, const std::vector<FlowPath>& paths,
                     const SourceSpan span, const std::string& block_name) {
        if (paths.empty()) {
            environment_ = base;
            current_block_.reset();
            return;
        }

        if (paths.size() == 1) {
            const IRBlockId merge = create_block(block_name);
            set_terminator(paths.front().block,
                IRTerminator{IRBranch{merge, {}}, span});
            environment_ = paths.front().environment;
            current_block_ = merge;
            return;
        }

        const std::vector<SemanticBindingId> ids = sorted_environment_ids(base);
        std::vector<SemanticBindingId> changed;
        for (const SemanticBindingId id : ids) {
            const IRValue before = base.at(id);
            const IRValue first = environment_value(paths[0].environment, id, span);
            const IRValue second = environment_value(paths[1].environment, id, span);
            if (first.id != second.id) changed.push_back(id);
            else if (first.id != before.id) {
                // Both paths agree on the new SSA version; no block parameter is needed.
            }
        }

        std::vector<IRValue> parameters;
        parameters.reserve(changed.size());
        for (const SemanticBindingId id : changed) {
            parameters.push_back(make_value_like(base.at(id), span));
        }
        const IRBlockId merge = create_block(block_name, parameters);

        for (const FlowPath& path : paths) {
            std::vector<IRValueId> arguments;
            arguments.reserve(changed.size());
            for (const SemanticBindingId id : changed) {
                arguments.push_back(environment_value(path.environment, id, span).id);
            }
            set_terminator(path.block,
                IRTerminator{IRBranch{merge, std::move(arguments)}, span});
        }

        environment_ = base;
        for (std::size_t index = 0; index < changed.size(); ++index) {
            environment_[changed[index]] = parameters[index];
        }
        current_block_ = merge;
    }

    // Lower the left side first; the right side exists only on the RHS path.
    [[nodiscard]] std::optional<IRValue> lower_short_circuit(
        const BinaryExpr& binary, const Expr& expression) {
        const auto left = binary.left ? lower_expression(*binary.left) : std::nullopt;
        if (!left || left->type != IRType::Bool) return std::nullopt;
        const Environment base = environment_;
        const IRBlockId origin = *current_block_;
        const IRBlockId right_block = create_block("logic.rhs");
        const IRBlockId short_block = create_block("logic.short");
        const bool is_and = binary.op == TokenKind::AndAnd;
        const IRBlockId true_target = is_and ? right_block : short_block;
        const IRBlockId false_target = is_and ? short_block : right_block;
        set_terminator(origin, IRTerminator{
            IRConditionalBranch{left->id, true_target, {}, false_target, {}},
            expression.span
        });

        current_block_ = short_block;
        environment_ = base;
        const IRValue short_value = emit_constant(
            IRType::Bool, is_and ? "false" : "true", expression.span);
        const FlowPath short_path{*current_block_, environment_};

        current_block_ = right_block;
        environment_ = base;
        const auto right = binary.right ? lower_expression(*binary.right) : std::nullopt;
        if (!right || right->type != IRType::Bool || !current_block_) {
            report("IR114", "logical expression did not produce a bool value",
                   expression.span);
            current_block_.reset();
            environment_ = base;
            return std::nullopt;
        }
        const FlowPath right_path{*current_block_, environment_};

        const std::vector<SemanticBindingId> ids = sorted_environment_ids(base);
        std::vector<SemanticBindingId> changed;
        for (const SemanticBindingId id : ids) {
            if (environment_value(short_path.environment, id, expression.span).id !=
                environment_value(right_path.environment, id, expression.span).id) {
                changed.push_back(id);
            }
        }

        std::vector<IRValue> parameters;
        parameters.reserve(changed.size() + 1);
        for (const SemanticBindingId id : changed) {
            parameters.push_back(make_value_like(base.at(id), expression.span));
        }
        const IRValue result_parameter = make_value(IRType::Bool, expression.span);
        parameters.push_back(result_parameter);
        const IRBlockId merge = create_block("logic.end", parameters);

        auto branch_arguments = [&](const Environment& environment,
                                    const IRValue& value) {
            std::vector<IRValueId> arguments;
            arguments.reserve(changed.size() + 1);
            for (const SemanticBindingId id : changed) {
                arguments.push_back(environment_value(environment, id, expression.span).id);
            }
            arguments.push_back(value.id);
            return arguments;
        };
        set_terminator(short_path.block, IRTerminator{
            IRBranch{merge, branch_arguments(short_path.environment, short_value)},
            expression.span
        });
        set_terminator(right_path.block, IRTerminator{
            IRBranch{merge, branch_arguments(right_path.environment, *right)},
            expression.span
        });

        environment_ = base;
        for (std::size_t index = 0; index < changed.size(); ++index) {
            environment_[changed[index]] = parameters[index];
        }
        current_block_ = merge;
        return result_parameter;
    }

    [[nodiscard]] std::optional<IRValue> lower_binary(
        const BinaryExpr& binary, const Expr& expression) {
        if (binary.op == TokenKind::AndAnd || binary.op == TokenKind::OrOr) {
            return lower_short_circuit(binary, expression);
        }

        // Emit ordinary binary operands in VEX's left-to-right order.
        auto left = binary.left ? lower_expression(*binary.left) : std::nullopt;
        auto right = binary.right ? lower_expression(*binary.right) : std::nullopt;
        if (!left || !right) return std::nullopt;

        const auto semantic_left = binary.left
            ? semantic_model_.expression_types.find(binary.left.get())
            : semantic_model_.expression_types.end();
        const auto semantic_right = binary.right
            ? semantic_model_.expression_types.find(binary.right.get())
            : semantic_model_.expression_types.end();
        const bool numeric_pair =
            semantic_left != semantic_model_.expression_types.end() &&
            semantic_right != semantic_model_.expression_types.end() &&
            (semantic_left->second == SemanticType::Int ||
             semantic_left->second == SemanticType::Float) &&
            (semantic_right->second == SemanticType::Int ||
             semantic_right->second == SemanticType::Float);
        if (numeric_pair && left->type != right->type) {
            auto converted_left = convert(*left, IRType::Float, expression.span);
            auto converted_right = convert(*right, IRType::Float, expression.span);
            if (!converted_left || !converted_right) return std::nullopt;
            left = *converted_left;
            right = *converted_right;
        }

        const auto result_type = expression_type(expression);
        if (!result_type) return std::nullopt;

        IROp op = IROp::Add;
        switch (binary.op) {
        case TokenKind::Plus: op = IROp::Add; break;
        case TokenKind::Minus: op = IROp::Subtract; break;
        case TokenKind::Star: op = IROp::Multiply; break;
        case TokenKind::Slash: op = IROp::Divide; break;
        case TokenKind::Percent: op = IROp::Remainder; break;
        case TokenKind::EqualEqual: op = IROp::Equal; break;
        case TokenKind::BangEqual: op = IROp::NotEqual; break;
        case TokenKind::Less: op = IROp::Less; break;
        case TokenKind::LessEqual: op = IROp::LessEqual; break;
        case TokenKind::Greater: op = IROp::Greater; break;
        case TokenKind::GreaterEqual: op = IROp::GreaterEqual; break;
        default:
            report("IR115", "unsupported binary operator during IR lowering",
                   expression.span);
            return std::nullopt;
        }
        return emit_operation(op, *result_type, {left->id, right->id}, expression.span);
    }

    [[nodiscard]] std::optional<IRValue> lower_call(
        const CallExpr& call, const Expr& expression) {
        const auto target = semantic_model_.resolved_calls.find(&call);
        if (target == semantic_model_.resolved_calls.end()) {
            report("IR116", "function call is missing resolved target information",
                   expression.span);
            return std::nullopt;
        }

        std::vector<IRValue> arguments;
        arguments.reserve(call.arguments.size());
        // Evaluate call arguments in source order before emitting the call.
        for (const ExprPtr& argument : call.arguments) {
            if (!argument) {
                report("IR117", "call contains a missing argument", expression.span);
                return std::nullopt;
            }
            const auto value = lower_expression(*argument);
            if (!value) {
                report("IR118", "call argument did not produce a value", argument->span);
                return std::nullopt;
            }
            arguments.push_back(*value);
        }

        if (target->second == "say") {
            if (arguments.size() != 1) {
                report("IR119", "say expects exactly one argument", expression.span);
                return std::nullopt;
            }
            IRInstruction instruction;
            instruction.op = IROp::Say;
            instruction.operands.push_back(arguments.front().id);
            instruction.span = expression.span;
            emit(std::move(instruction));
            return std::nullopt;
        }

        const bool is_math_builtin =
            target->second == "sqrt" || target->second == "sin" ||
            target->second == "cos" || target->second == "pow" ||
            target->second == "abs";
        if (is_math_builtin) {
            const std::size_t expected_arity = target->second == "pow" ? 2u : 1u;
            if (arguments.size() != expected_arity) {
                report("IR126", "mathematical function has an unexpected argument count",
                       expression.span);
                return std::nullopt;
            }
            std::vector<IRValueId> operands;
            operands.reserve(arguments.size());
            for (std::size_t index = 0; index < arguments.size(); ++index) {
                const auto converted = convert(arguments[index], IRType::Float,
                                               call.arguments[index]->span);
                if (!converted) return std::nullopt;
                operands.push_back(converted->id);
            }
            IRInstruction instruction;
            instruction.op = IROp::Math;
            instruction.operands = std::move(operands);
            instruction.callee = target->second;
            instruction.result = make_value(IRType::Float, expression.span);
            instruction.span = expression.span;
            const IRValue result = *instruction.result;
            emit(std::move(instruction));
            return result;
        }

        const auto signature = signatures_.find(target->second);
        if (signature == signatures_.end()) {
            report("IR120", "resolved function signature is missing for '" +
                   target->second + "'", expression.span);
            return std::nullopt;
        }
        if (arguments.size() != signature->second.parameter_types.size()) {
            report("IR121", "function call has an unexpected argument count",
                   expression.span);
            return std::nullopt;
        }

        std::vector<IRValueId> operands;
        operands.reserve(arguments.size());
        for (std::size_t index = 0; index < arguments.size(); ++index) {
            const auto parameter_type =
                to_ir_type(signature->second.parameter_types[index]);
            if (!parameter_type) {
                report("IR122", "function parameter type cannot be represented in IR",
                       expression.span);
                return std::nullopt;
            }
            if (*parameter_type == IRType::Array) {
                if (index >= signature->second.parameter_array_types.size() ||
                    !signature->second.parameter_array_types[index]) {
                    report("IR122", "array parameter is missing its fixed shape",
                           call.arguments[index]->span);
                    return std::nullopt;
                }
                const SemanticArrayType& expected =
                    *signature->second.parameter_array_types[index];
                const auto expected_element = to_ir_type(expected.element_type);
                if (!expected_element || arguments[index].type != IRType::Array ||
                    arguments[index].element_type != *expected_element ||
                    arguments[index].array_length != expected.length) {
                    report("IR122", "array argument does not match the parameter's fixed shape",
                           call.arguments[index]->span);
                    return std::nullopt;
                }
            }
            const auto converted = convert(arguments[index], *parameter_type,
                                           call.arguments[index]->span);
            if (!converted) return std::nullopt;
            operands.push_back(converted->id);
        }

        const auto return_type = to_ir_type(signature->second.return_type);
        if (!return_type) {
            report("IR123", "function return type cannot be represented in IR",
                   expression.span);
            return std::nullopt;
        }
        IRInstruction instruction;
        instruction.op = IROp::Call;
        instruction.operands = std::move(operands);
        instruction.callee = target->second;
        instruction.span = expression.span;
        if (*return_type != IRType::Void) {
            if (*return_type == IRType::Array) {
                if (!signature->second.return_array_type) {
                    report("IR123", "array return type is missing its fixed shape",
                           expression.span);
                    return std::nullopt;
                }
                const auto element_type =
                    to_ir_type(signature->second.return_array_type->element_type);
                if (!element_type || *element_type == IRType::Array ||
                    *element_type == IRType::Void) {
                    report("IR123", "array return element type cannot be represented in IR",
                           expression.span);
                    return std::nullopt;
                }
                instruction.result = make_array_value(
                    *element_type, signature->second.return_array_type->length,
                    expression.span);
            } else {
                instruction.result = make_value(*return_type, expression.span);
            }
            const IRValue result = *instruction.result;
            emit(std::move(instruction));
            return result;
        }
        emit(std::move(instruction));
        return std::nullopt;
    }

    [[nodiscard]] std::optional<IRValue> lower_expression(const Expr& expression) {
        if (!current_block_) {
            report("IR124", "expression appears in unreachable code", expression.span);
            return std::nullopt;
        }

        if (const auto* literal = std::get_if<LiteralExpr>(&expression.data)) {
            static_cast<void>(literal);
            const auto type = expression_type(expression);
            if (!type || *type == IRType::Void) return std::nullopt;
            return emit_constant(*type, spelling(expression.span), expression.span);
        }
        if (const auto* name = std::get_if<NameExpr>(&expression.data)) {
            const auto binding = semantic_model_.resolved_names.find(name);
            if (binding == semantic_model_.resolved_names.end()) {
                report("IR125", "variable reference has no resolved binding",
                       name->name);
                return std::nullopt;
            }
            const auto current = environment_.find(binding->second);
            if (current == environment_.end()) {
                report("IR112", "variable has no current SSA value", name->name);
                return std::nullopt;
            }
            return current->second;
        }
        if (const auto* group = std::get_if<GroupExpr>(&expression.data)) {
            return group->expression ? lower_expression(*group->expression) : std::nullopt;
        }
        if (const auto* array = std::get_if<ArrayLiteralExpr>(&expression.data)) {
            const auto shape = semantic_model_.array_expression_types.find(&expression);
            if (shape == semantic_model_.array_expression_types.end()) {
                report("IR141", "array literal is missing semantic shape information", expression.span);
                return std::nullopt;
            }
            const auto element_type = to_ir_type(shape->second.element_type);
            if (!element_type || *element_type == IRType::Void || *element_type == IRType::Array) {
                report("IR142", "array element type cannot be represented in IR", expression.span);
                return std::nullopt;
            }
            IRInstruction instruction;
            instruction.op = IROp::ArrayMake;
            instruction.span = expression.span;
            instruction.result = make_array_value(*element_type, shape->second.length, expression.span);
            instruction.operands.reserve(array->elements.size());
            // Array elements are evaluated from first to last.
            for (const ExprPtr& element : array->elements) {
                if (!element) return std::nullopt;
                const auto value = lower_expression(*element);
                if (!value) return std::nullopt;
                const auto converted = convert(*value, *element_type, element->span);
                if (!converted) return std::nullopt;
                instruction.operands.push_back(converted->id);
            }
            const IRValue result = *instruction.result;
            emit(std::move(instruction));
            return result;
        }
        if (const auto* indexed = std::get_if<IndexExpr>(&expression.data)) {
            if (!indexed->object || !indexed->index) return std::nullopt;
            // Indexed reads evaluate the array expression before the index.
            const auto array = lower_expression(*indexed->object);
            const auto index = lower_expression(*indexed->index);
            if (!array || !index || array->type != IRType::Array || index->type != IRType::Int)
                return std::nullopt;
            IRInstruction instruction;
            instruction.op = IROp::ArrayGet;
            instruction.operands = {array->id, index->id};
            instruction.result = make_value(array->element_type, expression.span);
            instruction.span = expression.span;
            const IRValue result = *instruction.result;
            emit(std::move(instruction));
            return result;
        }
        if (const auto* unary = std::get_if<UnaryExpr>(&expression.data)) {
            if (!unary->operand) return std::nullopt;
            const auto operand = lower_expression(*unary->operand);
            if (!operand) return std::nullopt;
            if (unary->op == TokenKind::Plus) return operand;
            const auto type = expression_type(expression);
            if (!type) return std::nullopt;
            const IROp op = unary->op == TokenKind::Bang
                ? IROp::LogicalNot : IROp::Negate;
            return emit_operation(op, *type, {operand->id}, expression.span);
        }
        if (const auto* binary = std::get_if<BinaryExpr>(&expression.data)) {
            return lower_binary(*binary, expression);
        }
        if (const auto* assignment =
                std::get_if<AssignmentExpr>(&expression.data)) {
            if (const auto* indexed = assignment->target
                    ? std::get_if<IndexExpr>(&assignment->target->data) : nullptr) {
                const auto target = semantic_model_.assignment_targets.find(assignment);
                if (target == semantic_model_.assignment_targets.end() ||
                    !indexed->index || !assignment->value) {
                    report("IR143", "array element assignment is missing resolved target information", expression.span);
                    return std::nullopt;
                }
                const IRValue array = current_value(target->second, expression.span);
                if (array.type != IRType::Array) {
                    report("IR144", "array element assignment target is not an array value", expression.span);
                    return std::nullopt;
                }
                // For indexed assignment, evaluate the index before the RHS.
                const auto index = lower_expression(*indexed->index);
                const auto value = lower_expression(*assignment->value);
                if (!index || !value || index->type != IRType::Int) return std::nullopt;
                const auto converted = convert(*value, array.element_type, expression.span);
                if (!converted) return std::nullopt;
                IRInstruction instruction;
                instruction.op = IROp::ArraySet;
                instruction.operands = {array.id, index->id, converted->id};
                instruction.result = make_value_like(array, expression.span);
                instruction.span = expression.span;
                const IRValue updated = *instruction.result;
                emit(std::move(instruction));
                environment_[target->second] = updated;
                return *converted;
            }
            const auto target = semantic_model_.assignment_targets.find(assignment);
            if (target == semantic_model_.assignment_targets.end()) {
                report("IR126", "assignment has no resolved target binding",
                       expression.span);
                return std::nullopt;
            }
            if (!assignment->value) return std::nullopt;
            const auto value = lower_expression(*assignment->value);
            if (!value) return std::nullopt;
            const SemanticBinding* binding = get_binding(target->second);
            if (!binding) {
                report("IR127", "assignment target binding metadata is missing",
                       expression.span);
                return std::nullopt;
            }
            const auto target_type = to_ir_type(binding->type);
            if (!target_type) {
                report("IR128", "assignment target type cannot be represented in IR",
                       expression.span);
                return std::nullopt;
            }
            const auto converted = convert(*value, *target_type, expression.span);
            if (!converted) return std::nullopt;
            environment_[target->second] = *converted;
            return *converted;
        }
        if (const auto* call = std::get_if<CallExpr>(&expression.data)) {
            return lower_call(*call, expression);
        }

        report("IR129", "unknown expression node during IR lowering", expression.span);
        return std::nullopt;
    }

    void lower_variable(const VariableDeclStmt& variable) {
        const auto binding = semantic_model_.variable_bindings.find(&variable);
        if (binding == semantic_model_.variable_bindings.end()) {
            report("IR130", "variable declaration has no resolved binding",
                   variable.name);
            return;
        }
        const SemanticBinding* info = get_binding(binding->second);
        if (!info) {
            report("IR131", "variable declaration binding metadata is missing",
                   variable.name);
            return;
        }
        if (!variable.initializer) {
            report("IR132", "variable declaration has no initializer",
                   variable.name);
            return;
        }
        const auto value = lower_expression(*variable.initializer);
        if (!value) {
            report("IR133", "variable initializer did not produce a value",
                   variable.initializer->span);
            return;
        }
        const auto target_type = to_ir_type(info->type);
        if (!target_type || *target_type == IRType::Void) {
            report("IR134", "variable type cannot be represented in the initial IR",
                   variable.name);
            return;
        }
        const auto converted = convert(*value, *target_type, variable.name);
        if (converted) environment_[binding->second] = *converted;
    }

    void lower_if(const IfStmt& conditional, const SourceSpan span) {
        if (!conditional.condition || !conditional.then_branch) return;
        const auto condition = lower_expression(*conditional.condition);
        if (!condition) return;

        const Environment base = environment_;
        const IRBlockId origin = *current_block_;
        const IRBlockId then_block = create_block("if.then");
        const IRBlockId else_block = create_block("if.else");
        set_terminator(origin, IRTerminator{
            IRConditionalBranch{condition->id, then_block, {}, else_block, {}}, span
        });

        std::vector<FlowPath> paths;
        current_block_ = then_block;
        environment_ = base;
        lower_statement(*conditional.then_branch);
        if (current_block_) paths.push_back(FlowPath{*current_block_, environment_});

        current_block_ = else_block;
        environment_ = base;
        if (conditional.else_branch) lower_statement(*conditional.else_branch);
        if (current_block_) paths.push_back(FlowPath{*current_block_, environment_});

        merge_paths(base, paths, span, "if.end");
    }

    // The top context makes break/continue target only the innermost loop.
    void lower_loop_control(const bool is_break, const SourceSpan span) {
        if (loops_.empty()) {
            report("IR138", is_break
                ? "break has no enclosing loop during IR lowering"
                : "continue has no enclosing loop during IR lowering", span);
            current_block_.reset();
            return;
        }

        const LoopContext& loop = loops_.back();
        std::vector<IRValueId> arguments;
        arguments.reserve(loop.environment_ids.size() + (loop.counter && !is_break ? 1u : 0u));
        if (!is_break && loop.counter) {
            if (loop.increment_counter_on_continue) {
                const IRValue one = emit_constant(IRType::Int, "1", span);
                const IRValue next_index = emit_operation(
                    IROp::Add, IRType::Int, {loop.counter->id, one.id}, span);
                arguments.push_back(next_index.id);
            } else {
                arguments.push_back(loop.counter->id);
            }
        }
        for (const SemanticBindingId id : loop.environment_ids) {
            arguments.push_back(environment_value(environment_, id, span).id);
        }
        terminate_current(IRTerminator{
            IRBranch{is_break ? loop.break_target : loop.continue_target,
                     std::move(arguments)}, span
        });
    }

    void lower_while(const WhileStmt& loop, const SourceSpan span) {
        if (!loop.condition || !loop.body || !current_block_) return;

        const Environment base = environment_;
        const std::vector<SemanticBindingId> ids = sorted_environment_ids(base);
        const IRBlockId preheader = *current_block_;

        std::vector<IRValue> header_parameters;
        std::vector<IRValue> body_parameters;
        std::vector<IRValue> exit_parameters;
        header_parameters.reserve(ids.size());
        body_parameters.reserve(ids.size());
        exit_parameters.reserve(ids.size());
        for (const SemanticBindingId id : ids) {
            header_parameters.push_back(make_value_like(base.at(id), span));
            body_parameters.push_back(make_value_like(base.at(id), span));
            exit_parameters.push_back(make_value_like(base.at(id), span));
        }

        const IRBlockId header = create_block("while.header", header_parameters);
        const IRBlockId body = create_block("while.body", body_parameters);
        const IRBlockId exit = create_block("while.exit", exit_parameters);

        std::vector<IRValueId> initial_arguments;
        initial_arguments.reserve(ids.size());
        for (const SemanticBindingId id : ids) initial_arguments.push_back(base.at(id).id);
        set_terminator(preheader,
            IRTerminator{IRBranch{header, std::move(initial_arguments)}, span});

        environment_ = base;
        for (std::size_t index = 0; index < ids.size(); ++index)
            environment_[ids[index]] = header_parameters[index];
        current_block_ = header;
        const auto condition = lower_expression(*loop.condition);
        if (!condition || condition->type != IRType::Bool || !current_block_) {
            report("IR139", "while condition did not produce a bool value",
                   loop.condition->span);
            current_block_.reset();
            return;
        }

        std::vector<IRValueId> branch_arguments;
        branch_arguments.reserve(ids.size());
        for (const SemanticBindingId id : ids)
            branch_arguments.push_back(environment_value(environment_, id, span).id);
        set_terminator(*current_block_, IRTerminator{
            IRConditionalBranch{condition->id, body, branch_arguments,
                                exit, branch_arguments}, span
        });

        environment_ = base;
        for (std::size_t index = 0; index < ids.size(); ++index)
            environment_[ids[index]] = body_parameters[index];
        current_block_ = body;
        loops_.push_back(LoopContext{header, exit, ids, std::nullopt});
        lower_statement(*loop.body);
        loops_.pop_back();

        if (current_block_) {
            std::vector<IRValueId> backedge_arguments;
            backedge_arguments.reserve(ids.size());
            for (const SemanticBindingId id : ids)
                backedge_arguments.push_back(environment_value(environment_, id, span).id);
            terminate_current(IRTerminator{
                IRBranch{header, std::move(backedge_arguments)}, span
            });
        }

        environment_ = base;
        for (std::size_t index = 0; index < ids.size(); ++index)
            environment_[ids[index]] = exit_parameters[index];
        current_block_ = exit;
    }

    void lower_repeat(const RepeatStmt& repeat, const SourceSpan span) {
        if (!repeat.count || !repeat.body) return;
        const auto count = lower_expression(*repeat.count);
        if (!count || count->type != IRType::Int || !current_block_) return;

        const Environment base = environment_;
        const std::vector<SemanticBindingId> ids = sorted_environment_ids(base);
        const IRBlockId preheader = *current_block_;
        const IRValue zero = emit_constant(IRType::Int, "0", span);

        std::vector<IRValue> header_parameters;
        header_parameters.reserve(ids.size() + 1);
        const IRValue index_parameter = make_value(IRType::Int, span);
        header_parameters.push_back(index_parameter);
        for (const SemanticBindingId id : ids) {
            header_parameters.push_back(make_value_like(base.at(id), span));
        }

        std::vector<IRValue> exit_parameters;
        exit_parameters.reserve(ids.size());
        for (const SemanticBindingId id : ids) {
            exit_parameters.push_back(make_value_like(base.at(id), span));
        }

        const IRBlockId header = create_block("repeat.header", header_parameters);
        const IRBlockId body = create_block("repeat.body");
        const IRBlockId exit = create_block("repeat.exit", exit_parameters);

        std::vector<IRValueId> initial_arguments;
        initial_arguments.reserve(ids.size() + 1);
        initial_arguments.push_back(zero.id);
        for (const SemanticBindingId id : ids) {
            initial_arguments.push_back(base.at(id).id);
        }
        set_terminator(preheader,
            IRTerminator{IRBranch{header, std::move(initial_arguments)}, span});

        Environment loop_environment = base;
        for (std::size_t index = 0; index < ids.size(); ++index) {
            loop_environment[ids[index]] = header_parameters[index + 1];
        }

        current_block_ = header;
        environment_ = loop_environment;
        const IRValue comparison = emit_operation(
            IROp::Less, IRType::Bool, {index_parameter.id, count->id}, span);

        std::vector<IRValueId> exit_arguments;
        exit_arguments.reserve(ids.size());
        for (const SemanticBindingId id : ids) {
            exit_arguments.push_back(loop_environment.at(id).id);
        }
        set_terminator(header, IRTerminator{
            IRConditionalBranch{comparison.id, body, {}, exit, std::move(exit_arguments)},
            span
        });

        current_block_ = body;
        environment_ = loop_environment;
        loops_.push_back(LoopContext{header, exit, ids, index_parameter});
        lower_statement(*repeat.body);
        loops_.pop_back();
        if (current_block_) {
            const IRValue one = emit_constant(IRType::Int, "1", span);
            const IRValue next_index = emit_operation(
                IROp::Add, IRType::Int, {index_parameter.id, one.id}, span);
            std::vector<IRValueId> backedge_arguments;
            backedge_arguments.reserve(ids.size() + 1);
            backedge_arguments.push_back(next_index.id);
            for (const SemanticBindingId id : ids) {
                backedge_arguments.push_back(environment_value(environment_, id, span).id);
            }
            terminate_current(IRTerminator{
                IRBranch{header, std::move(backedge_arguments)}, span
            });
        }

        current_block_ = exit;
        environment_ = base;
        for (std::size_t index = 0; index < ids.size(); ++index) {
            environment_[ids[index]] = exit_parameters[index];
        }
    }

    void lower_range_for(const RangeExpr& range, const ForStmt& loop,
                          const SourceSpan span) {
        if (!range.start || !range.end || !loop.body || !current_block_) return;
        // Evaluate each range bound once, from start to end.
        const auto start = lower_expression(*range.start);
        const auto end = lower_expression(*range.end);
        if (!start || !end || start->type != IRType::Int || end->type != IRType::Int) {
            report("IR147", "integer range bounds must lower to int values", span);
            return;
        }
        const auto iteration_binding = semantic_model_.for_bindings.find(&loop);
        if (iteration_binding == semantic_model_.for_bindings.end()) {
            report("IR146", "for-in variable is missing its semantic binding", loop.variable);
            return;
        }
        const Environment base = environment_;
        const std::vector<SemanticBindingId> ids = sorted_environment_ids(base);
        const IRBlockId preheader = *current_block_;
        const IRValue index_parameter = make_value(IRType::Int, span);
        std::vector<IRValue> header_parameters;
        header_parameters.reserve(ids.size() + 1);
        header_parameters.push_back(index_parameter);
        for (const SemanticBindingId id : ids)
            header_parameters.push_back(make_value_like(base.at(id), span));
        std::vector<IRValue> step_parameters;
        step_parameters.reserve(ids.size() + 1);
        step_parameters.push_back(make_value(IRType::Int, span));
        for (const SemanticBindingId id : ids)
            step_parameters.push_back(make_value_like(base.at(id), span));
        std::vector<IRValue> exit_parameters;
        exit_parameters.reserve(ids.size());
        for (const SemanticBindingId id : ids)
            exit_parameters.push_back(make_value_like(base.at(id), span));
        const IRBlockId header = create_block("for.range.header", header_parameters);
        const IRBlockId body = create_block("for.range.body");
        const IRBlockId step = create_block("for.range.step", step_parameters);
        const IRBlockId exit = create_block("for.range.exit", exit_parameters);
        std::vector<IRValueId> initial_arguments;
        initial_arguments.reserve(ids.size() + 1);
        initial_arguments.push_back(start->id);
        for (const SemanticBindingId id : ids)
            initial_arguments.push_back(base.at(id).id);
        set_terminator(preheader,
            IRTerminator{IRBranch{header, std::move(initial_arguments)}, span});
        Environment loop_environment = base;
        for (std::size_t index = 0; index < ids.size(); ++index)
            loop_environment[ids[index]] = header_parameters[index + 1];
        current_block_ = header;
        environment_ = loop_environment;
        const IRValue condition = emit_operation(
            range.inclusive ? IROp::LessEqual : IROp::Less,
            IRType::Bool, {index_parameter.id, end->id}, span);
        std::vector<IRValueId> header_exit_arguments;
        header_exit_arguments.reserve(ids.size());
        for (const SemanticBindingId id : ids)
            header_exit_arguments.push_back(loop_environment.at(id).id);
        set_terminator(header, IRTerminator{
            IRConditionalBranch{condition.id, body, {}, exit,
                                std::move(header_exit_arguments)}, span
        });
        current_block_ = body;
        environment_ = loop_environment;
        environment_[iteration_binding->second] = index_parameter;
        loops_.push_back(LoopContext{step, exit, ids, index_parameter, false});
        lower_statement(*loop.body);
        loops_.pop_back();
        if (current_block_) {
            std::vector<IRValueId> step_arguments;
            step_arguments.reserve(ids.size() + 1);
            step_arguments.push_back(index_parameter.id);
            for (const SemanticBindingId id : ids)
                step_arguments.push_back(environment_value(environment_, id, span).id);
            terminate_current(IRTerminator{IRBranch{step, std::move(step_arguments)}, span});
        }
        Environment step_environment = base;
        for (std::size_t index = 0; index < ids.size(); ++index)
            step_environment[ids[index]] = step_parameters[index + 1];
        current_block_ = step;
        environment_ = step_environment;
        const IRValue step_index = step_parameters.front();
        if (range.inclusive) {
            const IRValue at_end = emit_operation(
                IROp::Equal, IRType::Bool, {step_index.id, end->id}, span);
            std::vector<IRValue> increment_parameters;
            increment_parameters.reserve(ids.size() + 1);
            increment_parameters.push_back(make_value(IRType::Int, span));
            for (const SemanticBindingId id : ids)
                increment_parameters.push_back(make_value_like(base.at(id), span));
            const IRBlockId increment =
                create_block("for.range.increment", increment_parameters);
            std::vector<IRValueId> exit_arguments;
            std::vector<IRValueId> increment_arguments;
            exit_arguments.reserve(ids.size());
            increment_arguments.reserve(ids.size() + 1);
            increment_arguments.push_back(step_index.id);
            for (const SemanticBindingId id : ids) {
                const IRValueId value = environment_.at(id).id;
                exit_arguments.push_back(value);
                increment_arguments.push_back(value);
            }
            set_terminator(step, IRTerminator{
                IRConditionalBranch{at_end.id, exit, std::move(exit_arguments),
                                    increment, std::move(increment_arguments)}, span
            });
            current_block_ = increment;
            environment_ = base;
            const IRValue increment_index = increment_parameters.front();
            for (std::size_t index = 0; index < ids.size(); ++index)
                environment_[ids[index]] = increment_parameters[index + 1];
            const IRValue one = emit_constant(IRType::Int, "1", span);
            const IRValue next_index = emit_operation(
                IROp::Add, IRType::Int, {increment_index.id, one.id}, span);
            std::vector<IRValueId> backedge_arguments;
            backedge_arguments.reserve(ids.size() + 1);
            backedge_arguments.push_back(next_index.id);
            for (const SemanticBindingId id : ids)
                backedge_arguments.push_back(environment_value(environment_, id, span).id);
            terminate_current(IRTerminator{IRBranch{header, std::move(backedge_arguments)}, span});
        } else {
            const IRValue one = emit_constant(IRType::Int, "1", span);
            const IRValue next_index = emit_operation(
                IROp::Add, IRType::Int, {step_index.id, one.id}, span);
            std::vector<IRValueId> backedge_arguments;
            backedge_arguments.reserve(ids.size() + 1);
            backedge_arguments.push_back(next_index.id);
            for (const SemanticBindingId id : ids)
                backedge_arguments.push_back(environment_value(environment_, id, span).id);
            terminate_current(IRTerminator{IRBranch{header, std::move(backedge_arguments)}, span});
        }
        environment_ = base;
        for (std::size_t index = 0; index < ids.size(); ++index)
            environment_[ids[index]] = exit_parameters[index];
        current_block_ = exit;
    }

    void lower_for(const ForStmt& loop, const SourceSpan span) {
        if (!loop.iterable || !loop.body || !current_block_) return;
        const Expr* iterable_expression = loop.iterable.get();
        while (const auto* group = std::get_if<GroupExpr>(&iterable_expression->data)) {
            if (!group->expression) break;
            iterable_expression = group->expression.get();
        }
        if (const auto* range = std::get_if<RangeExpr>(&iterable_expression->data)) {
            lower_range_for(*range, loop, span);
            return;
        }
        const auto iterable = lower_expression(*loop.iterable);
        if (!iterable || iterable->type != IRType::Array || iterable->array_length == 0) {
            report("IR145", "for-in requires a non-empty fixed-size array", span);
            return;
        }
        const auto iteration_binding = semantic_model_.for_bindings.find(&loop);
        if (iteration_binding == semantic_model_.for_bindings.end()) {
            report("IR146", "for-in variable is missing its semantic binding", loop.variable);
            return;
        }

        const Environment base = environment_;
        const std::vector<SemanticBindingId> ids = sorted_environment_ids(base);
        const IRBlockId preheader = *current_block_;
        const IRValue zero = emit_constant(IRType::Int, "0", span);
        const IRValue index_parameter = make_value(IRType::Int, span);

        std::vector<IRValue> header_parameters;
        header_parameters.reserve(ids.size() + 1);
        header_parameters.push_back(index_parameter);
        for (const SemanticBindingId id : ids)
            header_parameters.push_back(make_value_like(base.at(id), span));

        std::vector<IRValue> exit_parameters;
        exit_parameters.reserve(ids.size());
        for (const SemanticBindingId id : ids)
            exit_parameters.push_back(make_value_like(base.at(id), span));

        const IRBlockId header = create_block("for.header", header_parameters);
        const IRBlockId body = create_block("for.body");
        const IRBlockId exit = create_block("for.exit", exit_parameters);

        std::vector<IRValueId> initial_arguments;
        initial_arguments.reserve(ids.size() + 1);
        initial_arguments.push_back(zero.id);
        for (const SemanticBindingId id : ids)
            initial_arguments.push_back(base.at(id).id);
        set_terminator(preheader,
            IRTerminator{IRBranch{header, std::move(initial_arguments)}, span});

        Environment loop_environment = base;
        for (std::size_t index = 0; index < ids.size(); ++index)
            loop_environment[ids[index]] = header_parameters[index + 1];

        current_block_ = header;
        environment_ = loop_environment;
        const IRValue length = emit_constant(IRType::Int,
            std::to_string(iterable->array_length), span);
        const IRValue condition = emit_operation(
            IROp::Less, IRType::Bool,
            {index_parameter.id, length.id}, span);

        std::vector<IRValueId> exit_arguments;
        exit_arguments.reserve(ids.size());
        for (const SemanticBindingId id : ids)
            exit_arguments.push_back(loop_environment.at(id).id);
        set_terminator(header, IRTerminator{
            IRConditionalBranch{condition.id, body, {}, exit,
                                std::move(exit_arguments)}, span
        });

        current_block_ = body;
        environment_ = loop_environment;
        IRValue current_array = *iterable;
        if (const auto* name = std::get_if<NameExpr>(&iterable_expression->data)) {
            const auto binding = semantic_model_.resolved_names.find(name);
            if (binding != semantic_model_.resolved_names.end()) {
                const auto current = environment_.find(binding->second);
                if (current != environment_.end()) current_array = current->second;
            }
        }
        IRInstruction read_element;
        read_element.op = IROp::ArrayGet;
        read_element.operands = {current_array.id, index_parameter.id};
        read_element.result = make_value(iterable->element_type, span);
        read_element.span = span;
        const IRValue element = *read_element.result;
        emit(std::move(read_element));
        environment_[iteration_binding->second] = element;

        loops_.push_back(LoopContext{header, exit, ids, index_parameter});
        lower_statement(*loop.body);
        loops_.pop_back();
        if (current_block_) {
            const IRValue one = emit_constant(IRType::Int, "1", span);
            const IRValue next_index = emit_operation(
                IROp::Add, IRType::Int, {index_parameter.id, one.id}, span);
            std::vector<IRValueId> backedge_arguments;
            backedge_arguments.reserve(ids.size() + 1);
            backedge_arguments.push_back(next_index.id);
            for (const SemanticBindingId id : ids)
                backedge_arguments.push_back(environment_value(environment_, id, span).id);
            terminate_current(IRTerminator{
                IRBranch{header, std::move(backedge_arguments)}, span
            });
        }

        environment_ = base;
        for (std::size_t index = 0; index < ids.size(); ++index)
            environment_[ids[index]] = exit_parameters[index];
        current_block_ = exit;
    }

    void lower_statement(const Statement& statement) {
        if (!current_block_) return;

        if (const auto* variable = std::get_if<VariableDeclStmt>(&statement.data)) {
            lower_variable(*variable);
            return;
        }
        if (const auto* returned = std::get_if<ReturnStmt>(&statement.data)) {
            std::optional<IRValueId> return_value;
            if (returned->value) {
                const auto value = lower_expression(*returned->value);
                if (value) {
                    const auto converted = convert(*value, function_.return_type,
                                                   returned->value->span);
                    if (converted) return_value = converted->id;
                }
            }
            terminate_current(IRTerminator{IRReturn{return_value}, statement.span});
            return;
        }
        if (const auto* expression = std::get_if<ExpressionStmt>(&statement.data)) {
            if (expression->expression)
                static_cast<void>(lower_expression(*expression->expression));
            return;
        }
        if (const auto* block = std::get_if<BlockStmt>(&statement.data)) {
            for (const StmtPtr& child : block->statements) {
                if (!current_block_) break;
                if (child) lower_statement(*child);
            }
            return;
        }
        if (const auto* conditional = std::get_if<IfStmt>(&statement.data)) {
            lower_if(*conditional, statement.span);
            return;
        }
        if (const auto* loop = std::get_if<WhileStmt>(&statement.data)) {
            lower_while(*loop, statement.span);
            return;
        }
        if (std::holds_alternative<BreakStmt>(statement.data)) {
            lower_loop_control(true, statement.span);
            return;
        }
        if (std::holds_alternative<ContinueStmt>(statement.data)) {
            lower_loop_control(false, statement.span);
            return;
        }
        if (const auto* repeat = std::get_if<RepeatStmt>(&statement.data)) {
            lower_repeat(*repeat, statement.span);
            return;
        }
        if (const auto* loop = std::get_if<ForStmt>(&statement.data)) {
            lower_for(*loop, statement.span);
            return;
        }
        if (std::holds_alternative<FunctionDeclStmt>(statement.data)) {
            report("IR136", "nested function declarations are not supported",
                   statement.span);
        }
    }

    const SourceFile& source_;
    const SemanticModel& semantic_model_;
    const std::unordered_map<std::string, SemanticFunctionSignature>& signatures_;
    const FunctionDeclStmt& declaration_;
    std::vector<Diagnostic>& diagnostics_;
    const std::vector<const Statement*>* implicit_body_{nullptr};
    IRFunction function_;
    Environment environment_;
    std::vector<LoopContext> loops_;
    std::optional<IRBlockId> current_block_;
    IRValueId next_value_id_{1};
    IRBlockId next_block_id_{0};
};

class LowererImpl {
public:
    LowererImpl(const SourceFile& source, const Program& program,
                const SemanticModel& semantic_model)
        : source_(source), program_(program), semantic_model_(semantic_model) {}

    IRLoweringResult run() {
        std::unordered_map<std::string, SemanticFunctionSignature> signatures;
        for (const auto& entry : semantic_model_.functions) {
            signatures.emplace(entry.second.name, entry.second);
        }

        bool has_explicit_main = false;
        std::vector<const Statement*> top_level_statements;

        for (const StmtPtr& statement : program_.statements) {
            if (!statement) continue;
            const auto* function =
                std::get_if<FunctionDeclStmt>(&statement->data);
            if (!function) {
                top_level_statements.push_back(statement.get());
                continue;
            }
            if (source_.slice(function->name) == "main") has_explicit_main = true;
            module_.functions.push_back(FunctionLowerer(
                source_, semantic_model_, signatures, *function, diagnostics_).lower());
        }

        if (has_explicit_main) {
            if (!top_level_statements.empty()) {
                diagnostics_.push_back(Diagnostic{
                    DiagnosticSeverity::Error, "IR140",
                    "top-level executable statements cannot be combined with an explicit main(); move them into main()",
                    top_level_statements.front()->span
                });
            }
        } else {
            const FunctionDeclStmt implicit_entry{};
            module_.functions.push_back(FunctionLowerer(
                source_, semantic_model_, signatures, implicit_entry,
                diagnostics_, &top_level_statements).lower());
        }
        return IRLoweringResult{std::move(module_), std::move(diagnostics_)};
    }

private:
    const SourceFile& source_;
    const Program& program_;
    const SemanticModel& semantic_model_;
    IRModule module_;
    std::vector<Diagnostic> diagnostics_;
};

} // namespace

bool IRLoweringResult::has_errors() const noexcept {
    return std::any_of(diagnostics.begin(), diagnostics.end(),
        [](const Diagnostic& diagnostic) {
            return diagnostic.severity == DiagnosticSeverity::Error;
        });
}

IRLowerer::IRLowerer(const SourceFile& source, const Program& program,
                     const SemanticModel& semantic_model)
    : source_(source), program_(program), semantic_model_(semantic_model) {}

IRLoweringResult IRLowerer::lower() {
    return LowererImpl(source_, program_, semantic_model_).run();
}

} // namespace vex
