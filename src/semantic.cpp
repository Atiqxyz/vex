#include "vex/semantic.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>
#include <string_view>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vex {
namespace {

using Type = SemanticType;

const char* type_name(const Type type) noexcept {
    return semantic_type_name(type);
}

bool is_numeric(const Type type) noexcept {
    return type == Type::Int || type == Type::Float;
}

bool is_assignable(const Type destination, const Type source) noexcept {
    if (destination == Type::Error || source == Type::Error ||
        destination == Type::Unknown || source == Type::Unknown) return true;
    if (source == Type::Void) return false;
    if (destination == Type::Any) return true;
    return destination == source ||
           (destination == Type::Float && source == Type::Int);
}

Type common_numeric_type(const Type left, const Type right) noexcept {
    if (left == Type::Error || right == Type::Error) return Type::Error;
    if (left == Type::Unknown || right == Type::Unknown) return Type::Unknown;
    if (!is_numeric(left) || !is_numeric(right)) return Type::Error;
    return left == Type::Float || right == Type::Float ? Type::Float : Type::Int;
}

struct Symbol {
    SemanticBindingId id{0};
    Type type{Type::Unknown};
    bool immutable{false};
    bool is_function{false};
    std::vector<Type> parameters;
    SourceSpan declaration{};
    std::optional<SemanticArrayType> array_type;
};

using Scope = std::unordered_map<std::string, Symbol>;

class AnalyzerImpl {
public:
    AnalyzerImpl(const SourceFile& source, const Program& program)
        : source_(source), program_(program) {
        scopes_.emplace_back();
    }

    SemanticResult run() {
        install_builtins();
        collect_function_signatures();
        for (const StmtPtr& statement : program_.statements) {
            if (statement) analyze_statement(*statement);
        }
        return SemanticResult{std::move(diagnostics_), std::move(model_)};
    }

private:
    void report(const char* code, std::string message, const SourceSpan span) {
        diagnostics_.push_back(Diagnostic{
            DiagnosticSeverity::Error, code, std::move(message), span
        });
    }

    [[nodiscard]] std::string spelling(const SourceSpan span) const {
        return std::string(source_.slice(span));
    }

    [[nodiscard]] Type annotation_type(const SourceSpan span,
                                       const bool allow_void) {
        const std::string name = spelling(span);
        if (name == "int") return Type::Int;
        if (name == "float") return Type::Float;
        if (name == "bool") return Type::Bool;
        if (name == "string") return Type::String;
        if (allow_void && name == "void") return Type::Void;
        report("SEM003", "unknown or unsupported type name '" + name + "'", span);
        return Type::Error;
    }

    void install_builtins() {
        Symbol say;
        say.type = Type::Void;
        say.is_function = true;
        say.parameters.push_back(Type::Any);
        say.declaration = {0, 0};
        scopes_.front().emplace("say", std::move(say));

        const auto add_math = [this](const char* name, const std::size_t arity) {
            Symbol function;
            function.type = Type::Float;
            function.is_function = true;
            function.parameters.assign(arity, Type::Float);
            function.declaration = {0, 0};
            scopes_.front().emplace(name, std::move(function));
        };
        add_math("sqrt", 1);
        add_math("sin", 1);
        add_math("cos", 1);
        add_math("pow", 2);
        add_math("abs", 1);
    }

    void collect_function_signatures() {
        for (const StmtPtr& statement : program_.statements) {
            if (!statement) continue;
            const auto* function =
                std::get_if<FunctionDeclStmt>(&statement->data);
            if (!function) continue;

            Symbol signature;
            signature.id = next_binding_id_++;
            signature.is_function = true;
            signature.declaration = function->name;
            signature.type = function->return_type
                ? annotation_type(*function->return_type, true)
                : Type::Void;

            for (const Parameter& parameter : function->parameters) {
                Type parameter_type = Type::Unknown;
                if (parameter.type_annotation) {
                    parameter_type = annotation_type(*parameter.type_annotation, false);
                } else {
                    report("SEM004",
                           "function parameters require an explicit type annotation",
                           parameter.name);
                }
                signature.parameters.push_back(parameter_type);
            }

            const std::string name = spelling(function->name);
            const auto existing = scopes_.front().find(name);
            if (existing != scopes_.front().end()) {
                report("SEM002", "duplicate global declaration of '" + name + "'",
                       function->name);
            } else {
                scopes_.front().emplace(name, signature);
            }
            SemanticFunctionSignature public_signature;
            public_signature.name = name;
            public_signature.parameter_types = signature.parameters;
            public_signature.return_type = signature.type;
            public_signature.declaration = function->name;
            model_.functions.emplace(function, std::move(public_signature));
            signatures_.emplace(function, std::move(signature));
        }
    }

    [[nodiscard]] Symbol* lookup(const std::string& name) {
        for (std::size_t index = scopes_.size(); index > 0; --index) {
            Scope& scope = scopes_[index - 1];
            const auto found = scope.find(name);
            if (found == scope.end()) continue;

            // File-scope values belong to implicit top-level execution. They
            // are not globals that separately declared functions can capture.
            if (inside_function_ && index == 1 && !found->second.is_function) {
                return nullptr;
            }
            return &found->second;
        }
        return nullptr;
    }

    [[nodiscard]] bool define(const std::string& name, Symbol symbol,
                              const SourceSpan declaration,
                              SemanticBindingId* assigned_id = nullptr) {
        Scope& current = scopes_.back();
        if (current.find(name) != current.end()) {
            report("SEM002", "duplicate declaration of '" + name +
                   "' in the same scope", declaration);
            return false;
        }
        if (symbol.id == 0) symbol.id = next_binding_id_++;
        if (assigned_id) *assigned_id = symbol.id;
        model_.bindings.emplace(symbol.id, SemanticBinding{
            symbol.id, name, symbol.type, !symbol.immutable && !symbol.is_function,
            declaration, symbol.array_type
        });
        current.emplace(name, std::move(symbol));
        return true;
    }

    [[nodiscard]] Type analyze_expression(const Expr& expression) {
        const Type result = analyze_expression_impl(expression);
        model_.expression_types[&expression] = result;
        return result;
    }

    [[nodiscard]] Type analyze_expression_impl(const Expr& expression) {
        if (const auto* literal = std::get_if<LiteralExpr>(&expression.data)) {
            switch (literal->token_kind) {
            case TokenKind::Integer: return Type::Int;
            case TokenKind::Float: return Type::Float;
            case TokenKind::String: return Type::String;
            case TokenKind::KwTrue:
            case TokenKind::KwFalse: return Type::Bool;
            default: return Type::Error;
            }
        }

        if (const auto* name = std::get_if<NameExpr>(&expression.data)) {
            const std::string identifier = spelling(name->name);
            Symbol* symbol = lookup(identifier);
            if (!symbol) {
                report("SEM001", "undefined identifier '" + identifier + "'",
                       name->name);
                return Type::Error;
            }
            if (symbol->is_function) {
                report("SEM010",
                       "function names can only be used as function call targets",
                       name->name);
                return Type::Error;
            }
            if (symbol->id != 0) model_.resolved_names[name] = symbol->id;
            if (symbol->array_type)
                model_.array_expression_types[&expression] = *symbol->array_type;
            return symbol->type;
        }

        if (const auto* group = std::get_if<GroupExpr>(&expression.data)) {
            if (!group->expression) return Type::Error;
            const Type grouped_type = analyze_expression(*group->expression);
            const auto shape = model_.array_expression_types.find(group->expression.get());
            if (shape != model_.array_expression_types.end())
                model_.array_expression_types[&expression] = shape->second;
            return grouped_type;
        }

        if (const auto* array = std::get_if<ArrayLiteralExpr>(&expression.data)) {
            if (array->elements.empty()) {
                report("SEM041", "empty arrays need an explicit element type; typed empty arrays are not implemented yet",
                       expression.span);
                return Type::Error;
            }
            Type element_type = Type::Unknown;
            for (const ExprPtr& element : array->elements) {
                const Type actual = element ? analyze_expression(*element) : Type::Error;
                if (actual == Type::Error || actual == Type::Void ||
                    actual == Type::Array || actual == Type::Any) {
                    report("SEM041", "array elements must use one supported primitive type",
                           element ? element->span : expression.span);
                    return Type::Error;
                }
                if (element_type == Type::Unknown) {
                    element_type = actual;
                } else if (is_numeric(element_type) && is_numeric(actual)) {
                    if (element_type == Type::Float || actual == Type::Float)
                        element_type = Type::Float;
                } else if (element_type != actual) {
                    report("SEM041", "all array elements must have compatible types",
                           element ? element->span : expression.span);
                    return Type::Error;
                }
            }
            if (array->elements.size() > 65536u) {
                report("SEM041", "fixed arrays are limited to 65,536 elements",
                       expression.span);
                return Type::Error;
            }
            model_.array_expression_types[&expression] = SemanticArrayType{
                element_type, static_cast<std::uint32_t>(array->elements.size())};
            return Type::Array;
        }

        if (const auto* range = std::get_if<RangeExpr>(&expression.data)) {
            if (!allow_range_expression_) {
                report("SEM046", "integer ranges can only be used directly in a for-in loop",
                       expression.span);
            }
            const bool previous_permission = allow_range_expression_;
            allow_range_expression_ = false;
            const Type start_type = range->start
                ? analyze_expression(*range->start) : Type::Error;
            const Type end_type = range->end
                ? analyze_expression(*range->end) : Type::Error;
            allow_range_expression_ = previous_permission;
            if (start_type != Type::Error && start_type != Type::Unknown &&
                start_type != Type::Int) {
                report("SEM047", "range start must have type int",
                       range->start ? range->start->span : expression.span);
            }
            if (end_type != Type::Error && end_type != Type::Unknown &&
                end_type != Type::Int) {
                report("SEM047", "range end must have type int",
                       range->end ? range->end->span : expression.span);
            }
            return Type::Range;
        }

        if (const auto* index = std::get_if<IndexExpr>(&expression.data)) {
            const Type object_type = index->object
                ? analyze_expression(*index->object) : Type::Error;
            const Type index_type = index->index
                ? analyze_expression(*index->index) : Type::Error;
            if (index_type != Type::Error && index_type != Type::Unknown &&
                index_type != Type::Int) {
                report("SEM042", "array index must have type int",
                       index->index ? index->index->span : expression.span);
            }
            if (object_type != Type::Array) {
                if (object_type != Type::Error && object_type != Type::Unknown)
                    report("SEM043", "indexing requires a fixed-size array value",
                           expression.span);
                return Type::Error;
            }
            const auto shape = index->object
                ? model_.array_expression_types.find(index->object.get())
                : model_.array_expression_types.end();
            if (shape == model_.array_expression_types.end()) {
                report("SEM043", "array type information is unavailable for this expression",
                       expression.span);
                return Type::Error;
            }
            return shape->second.element_type;
        }

        if (const auto* unary = std::get_if<UnaryExpr>(&expression.data)) {
            if (!unary->operand) return Type::Error;
            const Type operand = analyze_expression(*unary->operand);
            if (operand == Type::Error || operand == Type::Unknown) return operand;
            if (unary->op == TokenKind::Bang) {
                if (operand != Type::Bool) {
                    report("SEM011", "operator '!' requires a bool operand; got " +
                           std::string(type_name(operand)), unary->operand->span);
                    return Type::Error;
                }
                return Type::Bool;
            }
            if (unary->op == TokenKind::Plus || unary->op == TokenKind::Minus) {
                if (!is_numeric(operand)) {
                    report("SEM011", "unary numeric operators require int or float; got " +
                           std::string(type_name(operand)), unary->operand->span);
                    return Type::Error;
                }
                return operand;
            }
            return Type::Error;
        }

        if (const auto* binary = std::get_if<BinaryExpr>(&expression.data)) {
            if (!binary->left || !binary->right) return Type::Error;
            const Type left = analyze_expression(*binary->left);
            const Type right = analyze_expression(*binary->right);
            if (left == Type::Error || right == Type::Error) return Type::Error;
            if (left == Type::Unknown || right == Type::Unknown) return Type::Unknown;

            switch (binary->op) {
            case TokenKind::Plus:
            case TokenKind::Minus:
            case TokenKind::Star:
            case TokenKind::Slash: {
                const Type result = common_numeric_type(left, right);
                if (result == Type::Error) {
                    report("SEM012", "operator '" +
                           std::string(token_kind_name(binary->op)) +
                           "' requires numeric operands; got " +
                           type_name(left) + " and " + type_name(right),
                           expression.span);
                }
                return result;
            }
            case TokenKind::Percent:
                if (left != Type::Int || right != Type::Int) {
                    report("SEM012", "operator '%' requires two int operands",
                           expression.span);
                    return Type::Error;
                }
                return Type::Int;
            case TokenKind::Less:
            case TokenKind::LessEqual:
            case TokenKind::Greater:
            case TokenKind::GreaterEqual:
                if (!is_numeric(left) || !is_numeric(right)) {
                    report("SEM013", "comparison operators require numeric operands",
                           expression.span);
                    return Type::Error;
                }
                return Type::Bool;
            case TokenKind::EqualEqual:
            case TokenKind::BangEqual:
                if (left == Type::Array || right == Type::Array) {
                    report("SEM014", "fixed arrays cannot be compared as whole values", expression.span);
                    return Type::Error;
                }
                if ((!is_numeric(left) || !is_numeric(right)) && left != right) {
                    report("SEM014", "equality operands must have compatible types; got " +
                           std::string(type_name(left)) + " and " + type_name(right),
                           expression.span);
                    return Type::Error;
                }
                if (left == Type::Void || right == Type::Void) {
                    report("SEM014", "void values cannot be compared", expression.span);
                    return Type::Error;
                }
                return Type::Bool;
            case TokenKind::AndAnd:
            case TokenKind::OrOr:
                if (left != Type::Bool || right != Type::Bool) {
                    report("SEM015", "logical operators require two bool operands",
                           expression.span);
                    return Type::Error;
                }
                return Type::Bool;
            default:
                return Type::Error;
            }
        }

        if (const auto* assignment =
                std::get_if<AssignmentExpr>(&expression.data)) {
            if (const auto* indexed = assignment->target
                    ? std::get_if<IndexExpr>(&assignment->target->data) : nullptr) {
                const auto* base_name = indexed->object
                    ? std::get_if<NameExpr>(&indexed->object->data) : nullptr;
                if (!base_name) {
                    report("SEM044", "array elements can only be assigned through a named array binding",
                           indexed->object ? indexed->object->span : expression.span);
                    if (assignment->value)
                        static_cast<void>(analyze_expression(*assignment->value));
                    return Type::Error;
                }
                const std::string identifier = spelling(base_name->name);
                Symbol* symbol = lookup(identifier);
                if (!symbol || symbol->type != Type::Array || !symbol->array_type) {
                    report("SEM001", "undefined or non-array identifier '" + identifier + "'",
                           base_name->name);
                    if (assignment->value)
                        static_cast<void>(analyze_expression(*assignment->value));
                    return Type::Error;
                }
                if (indexed->index) {
                    const Type index_type = analyze_expression(*indexed->index);
                    if (index_type != Type::Error && index_type != Type::Unknown &&
                        index_type != Type::Int)
                        report("SEM042", "array index must have type int",
                               indexed->index->span);
                }
                model_.resolved_names[base_name] = symbol->id;
                model_.array_expression_types[indexed->object.get()] = *symbol->array_type;
                model_.assignment_targets[assignment] = symbol->id;
                const Type value = assignment->value
                    ? analyze_expression(*assignment->value) : Type::Error;
                if (symbol->immutable)
                    report("SEM017", "cannot modify an element of immutable array binding '" +
                           identifier + "'", base_name->name);
                if (!is_assignable(symbol->array_type->element_type, value))
                    report("SEM018", "cannot assign " + std::string(type_name(value)) +
                           " to array element of type " +
                           type_name(symbol->array_type->element_type), expression.span);
                return symbol->array_type->element_type;
            }

            const auto* target = assignment->target
                ? std::get_if<NameExpr>(&assignment->target->data) : nullptr;
            if (!target) return Type::Error;

            const std::string identifier = spelling(target->name);
            Symbol* symbol = lookup(identifier);
            if (!symbol) {
                report("SEM001", "undefined identifier '" + identifier + "'",
                       target->name);
                if (assignment->value) {
                    static_cast<void>(analyze_expression(*assignment->value));
                }
                return Type::Error;
            }
            if (symbol->is_function) {
                report("SEM016", "cannot assign to function '" + identifier + "'",
                       target->name);
                if (assignment->value) {
                    static_cast<void>(analyze_expression(*assignment->value));
                }
                return Type::Error;
            }
            model_.assignment_targets[assignment] = symbol->id;
            const Type value = assignment->value
                ? analyze_expression(*assignment->value) : Type::Error;
            if (symbol->type == Type::Array && assignment->value) {
                const auto shape = model_.array_expression_types.find(assignment->value.get());
                if (symbol->array_type && shape != model_.array_expression_types.end() &&
                    (symbol->array_type->element_type != shape->second.element_type ||
                     symbol->array_type->length != shape->second.length))
                    report("SEM045", "array assignment requires the same element type and length",
                           assignment->value->span);
            }
            if (symbol->immutable) {
                report("SEM017", "cannot assign to immutable binding '" +
                       identifier + "'", target->name);
            }
            if (!is_assignable(symbol->type, value)) {
                report("SEM018", "cannot assign " + std::string(type_name(value)) +
                       " to " + type_name(symbol->type) + " binding '" + identifier + "'",
                       expression.span);
            }
            return symbol->type;
        }

        if (const auto* call = std::get_if<CallExpr>(&expression.data)) {
            const Expr* callee = call->callee.get();
            while (callee) {
                const auto* group = std::get_if<GroupExpr>(&callee->data);
                if (!group) break;
                callee = group->expression.get();
            }

            const auto* name = callee ? std::get_if<NameExpr>(&callee->data) : nullptr;
            Symbol* function = nullptr;
            if (name) {
                const std::string identifier = spelling(name->name);
                function = lookup(identifier);
                if (!function) {
                    report("SEM001", "undefined identifier '" + identifier + "'",
                           name->name);
                } else if (!function->is_function) {
                    report("SEM019", "value '" + identifier + "' is not callable",
                           name->name);
                    function = nullptr;
                }
            } else if (call->callee) {
                static_cast<void>(analyze_expression(*call->callee));
                report("SEM019", "only named functions can be called in this VEX version",
                       call->callee->span);
            }

            std::vector<Type> argument_types;
            argument_types.reserve(call->arguments.size());
            for (const ExprPtr& argument : call->arguments) {
                argument_types.push_back(argument
                    ? analyze_expression(*argument) : Type::Error);
            }

            if (!function) return Type::Error;
            if (name) model_.resolved_calls[call] = spelling(name->name);
            if (name && spelling(name->name) == "say" &&
                std::find(argument_types.begin(), argument_types.end(), Type::Array) != argument_types.end()) {
                report("SEM046", "say does not accept array values; index an element first",
                       expression.span);
            }
            if (argument_types.size() != function->parameters.size()) {
                report("SEM020", "function expects " +
                       std::to_string(function->parameters.size()) +
                       " argument(s), but got " +
                       std::to_string(argument_types.size()), expression.span);
            }
            const std::size_t count =
                std::min(argument_types.size(), function->parameters.size());
            for (std::size_t index = 0; index < count; ++index) {
                if (!is_assignable(function->parameters[index], argument_types[index])) {
                    report("SEM021", "argument " + std::to_string(index + 1) +
                           " has type " + type_name(argument_types[index]) +
                           "; expected " + type_name(function->parameters[index]),
                           call->arguments[index]->span);
                }
            }
            return function->type;
        }

        return Type::Error;
    }

    void require_type(const Type actual, const Type expected,
                      const SourceSpan span, const char* code,
                      const char* description) {
        if (actual == Type::Error || actual == Type::Unknown ||
            is_assignable(expected, actual)) return;
        report(code, std::string(description) + "; expected " +
               type_name(expected) + ", got " + type_name(actual), span);
    }

    void analyze_variable(const VariableDeclStmt& variable) {
        const Type inferred = variable.initializer
            ? analyze_expression(*variable.initializer) : Type::Error;
        Type declared = inferred;
        if (variable.type_annotation) {
            declared = annotation_type(*variable.type_annotation, false);
            require_type(inferred, declared,
                         variable.initializer ? variable.initializer->span : variable.name,
                         "SEM022", "initializer type does not match variable annotation");
        }
        if (declared == Type::Void) {
            report("SEM023", "variables cannot have type void", variable.name);
            declared = Type::Error;
        }
        Symbol symbol;
        symbol.type = declared;
        if (variable.initializer && inferred == Type::Array) {
            const auto shape = model_.array_expression_types.find(variable.initializer.get());
            if (shape != model_.array_expression_types.end())
                symbol.array_type = shape->second;
        }
        symbol.immutable = variable.immutable;
        symbol.declaration = variable.name;
        SemanticBindingId binding_id = 0;
        if (define(spelling(variable.name), std::move(symbol),
                   variable.name, &binding_id)) {
            model_.variable_bindings[&variable] = binding_id;
        }
    }

    void analyze_function(const FunctionDeclStmt& function,
                          const Statement& statement) {
        const auto found = signatures_.find(&function);
        Symbol signature;
        if (found != signatures_.end()) signature = found->second;
        else signature.type = Type::Void;

        scopes_.emplace_back();
        const std::size_t count = std::min(function.parameters.size(),
                                           signature.parameters.size());
        for (std::size_t index = 0; index < function.parameters.size(); ++index) {
            const Parameter& parameter = function.parameters[index];
            Symbol symbol;
            symbol.type = index < count ? signature.parameters[index] : Type::Unknown;
            symbol.declaration = parameter.name;
            SemanticBindingId binding_id = 0;
            if (define(spelling(parameter.name), std::move(symbol),
                       parameter.name, &binding_id)) {
                model_.parameter_bindings[&parameter] = binding_id;
            }
        }

        const bool was_in_function = inside_function_;
        const Type previous_return = current_return_type_;
        inside_function_ = true;
        current_return_type_ = signature.type;

        if (function.body) {
            analyze_block(*function.body, false);
            if (signature.type != Type::Void && signature.type != Type::Error &&
                control_flow(*function.body).falls_through) {
                report("SEM024",
                       "a non-void function can reach its end without returning a value",
                       statement.span);
            }
        }

        current_return_type_ = previous_return;
        inside_function_ = was_in_function;
        scopes_.pop_back();
    }

    struct FlowSummary {
        bool falls_through{true};
        bool returns{false};
        bool breaks{false};
        bool continues{false};
    };

    [[nodiscard]] std::optional<bool> constant_boolean(const Expr* expression) const {
        if (!expression) return std::nullopt;
        if (const auto* literal = std::get_if<LiteralExpr>(&expression->data)) {
            if (literal->token_kind == TokenKind::KwTrue) return true;
            if (literal->token_kind == TokenKind::KwFalse) return false;
            return std::nullopt;
        }
        if (const auto* group = std::get_if<GroupExpr>(&expression->data))
            return constant_boolean(group->expression.get());
        if (const auto* unary = std::get_if<UnaryExpr>(&expression->data)) {
            if (unary->op != TokenKind::Bang) return std::nullopt;
            const auto operand = constant_boolean(unary->operand.get());
            if (operand) return !*operand;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::int64_t> constant_integer(
        const Expr* expression) const {
        if (!expression) return std::nullopt;
        if (const auto* group = std::get_if<GroupExpr>(&expression->data))
            return constant_integer(group->expression.get());

        if (const auto* literal = std::get_if<LiteralExpr>(&expression->data)) {
            if (literal->token_kind != TokenKind::Integer) return std::nullopt;
            const std::string_view text = source_.slice(expression->span);
            std::int64_t value = 0;
            const auto parsed = std::from_chars(
                text.data(), text.data() + text.size(), value);
            if (parsed.ec != std::errc{} ||
                parsed.ptr != text.data() + text.size()) return std::nullopt;
            return value;
        }

        if (const auto* unary = std::get_if<UnaryExpr>(&expression->data)) {
            const auto operand = constant_integer(unary->operand.get());
            if (!operand) return std::nullopt;
            if (unary->op == TokenKind::Plus) return operand;
            if (unary->op == TokenKind::Minus) {
                if (*operand == std::numeric_limits<std::int64_t>::min())
                    return std::nullopt;
                return -*operand;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<bool> for_iterable_is_nonempty(
        const ForStmt& loop) const {
        if (!loop.iterable) return std::nullopt;

        const auto array_shape =
            model_.array_expression_types.find(loop.iterable.get());
        if (array_shape != model_.array_expression_types.end())
            return array_shape->second.length != 0;

        const Expr* iterable = loop.iterable.get();
        while (iterable) {
            const auto* group = std::get_if<GroupExpr>(&iterable->data);
            if (!group || !group->expression) break;
            iterable = group->expression.get();
        }
        const auto* range = iterable
            ? std::get_if<RangeExpr>(&iterable->data) : nullptr;
        if (!range) return std::nullopt;

        const auto start = constant_integer(range->start.get());
        const auto end = constant_integer(range->end.get());
        if (!start || !end) return std::nullopt;
        return range->inclusive ? *start <= *end : *start < *end;
    }

    [[nodiscard]] static FlowSummary join_paths(
        const FlowSummary& left, const FlowSummary& right) {
        return FlowSummary{
            left.falls_through || right.falls_through,
            left.returns || right.returns,
            left.breaks || right.breaks,
            left.continues || right.continues
        };
    }

    [[nodiscard]] FlowSummary control_flow(const Statement& statement) const {
        if (std::holds_alternative<ReturnStmt>(statement.data))
            return FlowSummary{false, true, false, false};
        if (std::holds_alternative<BreakStmt>(statement.data))
            return FlowSummary{false, false, true, false};
        if (std::holds_alternative<ContinueStmt>(statement.data))
            return FlowSummary{false, false, false, true};

        if (const auto* block = std::get_if<BlockStmt>(&statement.data)) {
            FlowSummary result;
            for (const StmtPtr& child : block->statements) {
                if (!child || !result.falls_through) continue;
                const FlowSummary next = control_flow(*child);
                result.returns = result.returns || next.returns;
                result.breaks = result.breaks || next.breaks;
                result.continues = result.continues || next.continues;
                result.falls_through = next.falls_through;
            }
            return result;
        }

        if (const auto* conditional = std::get_if<IfStmt>(&statement.data)) {
            const auto condition = constant_boolean(conditional->condition.get());
            if (condition) {
                const StmtPtr& selected = *condition
                    ? conditional->then_branch : conditional->else_branch;
                return selected ? control_flow(*selected) : FlowSummary{};
            }
            const FlowSummary then_flow = conditional->then_branch
                ? control_flow(*conditional->then_branch) : FlowSummary{};
            const FlowSummary else_flow = conditional->else_branch
                ? control_flow(*conditional->else_branch) : FlowSummary{};
            return join_paths(then_flow, else_flow);
        }

        if (const auto* loop = std::get_if<WhileStmt>(&statement.data)) {
            const auto condition = constant_boolean(loop->condition.get());
            if (condition && !*condition) return FlowSummary{};

            const FlowSummary body = loop->body
                ? control_flow(*loop->body) : FlowSummary{};
            if (condition && *condition) {
                // A literal-true loop can leave only through a reachable break.
                // Falling through or continuing merely starts another iteration.
                return FlowSummary{body.breaks, body.returns, false, false};
            }
            // Unknown conditions may be false before the first iteration.
            return FlowSummary{true, body.returns, false, false};
        }

        if (const auto* repeat = std::get_if<RepeatStmt>(&statement.data)) {
            const auto count = constant_integer(repeat->count.get());
            if (count && *count <= 0) return FlowSummary{};

            const FlowSummary body = repeat->body
                ? control_flow(*repeat->body) : FlowSummary{};
            if (count && *count > 0) {
                // A known positive count runs the body at least once.
                return FlowSummary{
                    body.falls_through || body.breaks || body.continues,
                    body.returns, false, false
                };
            }
            // An unknown count may be zero, so the loop cannot prove a return.
            return FlowSummary{true, body.returns, false, false};
        }

        if (const auto* loop = std::get_if<ForStmt>(&statement.data)) {
            const FlowSummary body = loop->body
                ? control_flow(*loop->body) : FlowSummary{};
            const auto nonempty = for_iterable_is_nonempty(*loop);
            if (nonempty && *nonempty) {
                return FlowSummary{
                    body.falls_through || body.breaks || body.continues,
                    body.returns, false, false
                };
            }
            // If non-emptiness is not proven, the loop may execute zero times.
            return FlowSummary{true, body.returns, false, false};
        }

        // Declarations, expressions and nested function declarations do not
        // terminate the current function's control flow.
        return FlowSummary{};
    }

    void analyze_block(const Statement& statement, const bool create_scope) {
        if (create_scope) scopes_.emplace_back();
        const auto* block = std::get_if<BlockStmt>(&statement.data);
        if (block) {
            for (const StmtPtr& child : block->statements) {
                if (child) analyze_statement(*child);
            }
        }
        if (create_scope) scopes_.pop_back();
    }

    void analyze_statement(const Statement& statement) {
        if (const auto* function =
                std::get_if<FunctionDeclStmt>(&statement.data)) {
            analyze_function(*function, statement);
            return;
        }
        if (const auto* variable =
                std::get_if<VariableDeclStmt>(&statement.data)) {
            analyze_variable(*variable);
            return;
        }
        if (std::holds_alternative<BlockStmt>(statement.data)) {
            analyze_block(statement, true);
            return;
        }
        if (const auto* returned = std::get_if<ReturnStmt>(&statement.data)) {
            if (!inside_function_) {
                report("SEM025", "return is only valid inside a function",
                       statement.span);
                if (returned->value) {
                    static_cast<void>(analyze_expression(*returned->value));
                }
                return;
            }
            if (current_return_type_ == Type::Void) {
                if (returned->value) {
                    static_cast<void>(analyze_expression(*returned->value));
                    report("SEM026",
                           "a void function cannot return a value; add a return type annotation",
                           returned->value->span);
                }
            } else if (current_return_type_ != Type::Error) {
                if (!returned->value) {
                    report("SEM027", "a non-void function must return a value",
                           statement.span);
                } else {
                    const Type actual = analyze_expression(*returned->value);
                    require_type(actual, current_return_type_, returned->value->span,
                                 "SEM028", "return type does not match function");
                }
            }
            return;
        }
        if (std::holds_alternative<BreakStmt>(statement.data)) {
            if (loop_depth_ == 0)
                report("SEM032", "break is only valid inside a loop", statement.span);
            return;
        }
        if (std::holds_alternative<ContinueStmt>(statement.data)) {
            if (loop_depth_ == 0)
                report("SEM033", "continue is only valid inside a loop", statement.span);
            return;
        }
        if (const auto* conditional = std::get_if<IfStmt>(&statement.data)) {
            if (conditional->condition) {
                const Type condition = analyze_expression(*conditional->condition);
                require_type(condition, Type::Bool, conditional->condition->span,
                             "SEM029", "if condition must be boolean");
            }
            if (conditional->then_branch) analyze_statement(*conditional->then_branch);
            if (conditional->else_branch) analyze_statement(*conditional->else_branch);
            return;
        }
        if (const auto* loop = std::get_if<WhileStmt>(&statement.data)) {
            if (loop->condition) {
                const Type condition = analyze_expression(*loop->condition);
                require_type(condition, Type::Bool, loop->condition->span,
                             "SEM034", "while condition must be boolean");
            }
            ++loop_depth_;
            if (loop->body) analyze_statement(*loop->body);
            --loop_depth_;
            return;
        }
        if (const auto* repeat = std::get_if<RepeatStmt>(&statement.data)) {
            if (repeat->count) {
                const Type count = analyze_expression(*repeat->count);
                require_type(count, Type::Int, repeat->count->span,
                             "SEM030", "repeat count must be an integer");
            }
            ++loop_depth_;
            if (repeat->body) analyze_statement(*repeat->body);
            --loop_depth_;
            return;
        }
        if (const auto* loop = std::get_if<ForStmt>(&statement.data)) {
            const Expr* iterable_expression = loop->iterable.get();
            while (iterable_expression) {
                const auto* group = std::get_if<GroupExpr>(&iterable_expression->data);
                if (!group || !group->expression) break;
                iterable_expression = group->expression.get();
            }
            const bool is_range = iterable_expression &&
                std::holds_alternative<RangeExpr>(iterable_expression->data);
            const bool previous_permission = allow_range_expression_;
            allow_range_expression_ = is_range;
            const Type iterable = loop->iterable
                ? analyze_expression(*loop->iterable) : Type::Error;
            allow_range_expression_ = previous_permission;
            std::optional<SemanticArrayType> array_type;
            Type iteration_type = Type::Unknown;
            if (iterable == Type::Array && loop->iterable) {
                const auto shape = model_.array_expression_types.find(loop->iterable.get());
                if (shape != model_.array_expression_types.end()) {
                    array_type = shape->second;
                    iteration_type = array_type->element_type;
                }
            } else if (iterable == Type::Range) {
                iteration_type = Type::Int;
            } else if (iterable != Type::Error && iterable != Type::Unknown) {
                report("SEM031", "for-in requires a fixed-size array or integer range",
                       loop->iterable ? loop->iterable->span : loop->variable);
            }
            scopes_.emplace_back();
            Symbol iteration_variable;
            iteration_variable.type = iteration_type;
            iteration_variable.declaration = loop->variable;
            SemanticBindingId iteration_binding = 0;
            if (define(spelling(loop->variable), std::move(iteration_variable),
                       loop->variable, &iteration_binding))
                model_.for_bindings[loop] = iteration_binding;
            ++loop_depth_;
            if (loop->body) analyze_statement(*loop->body);
            --loop_depth_;
            scopes_.pop_back();
            return;
        }
        if (const auto* expression =
                std::get_if<ExpressionStmt>(&statement.data)) {
            if (expression->expression) {
                static_cast<void>(analyze_expression(*expression->expression));
            }
        }
    }

    const SourceFile& source_;
    const Program& program_;
    std::vector<Diagnostic> diagnostics_;
    std::vector<Scope> scopes_;
    std::unordered_map<const FunctionDeclStmt*, Symbol> signatures_;
    SemanticModel model_;
    SemanticBindingId next_binding_id_{1};
    bool inside_function_{false};
    std::size_t loop_depth_{0};
    bool allow_range_expression_{false};
    Type current_return_type_{Type::Void};
};

} // namespace

const char* semantic_type_name(const SemanticType type) noexcept {
    switch (type) {
    case SemanticType::Error: return "<error>";
    case SemanticType::Unknown: return "<unknown>";
    case SemanticType::Any: return "any";
    case SemanticType::Void: return "void";
    case SemanticType::Int: return "int";
    case SemanticType::Float: return "float";
    case SemanticType::Bool: return "bool";
    case SemanticType::String: return "string";
    case SemanticType::Array: return "array";
    case SemanticType::Range: return "range";
    }
    return "<unknown>";
}

bool SemanticResult::has_errors() const noexcept {
    return std::any_of(diagnostics.begin(), diagnostics.end(),
        [](const Diagnostic& diagnostic) {
            return diagnostic.severity == DiagnosticSeverity::Error;
        });
}

SemanticAnalyzer::SemanticAnalyzer(const SourceFile& source,
                                   const Program& program)
    : source_(source), program_(program) {}

SemanticResult SemanticAnalyzer::analyze() {
    return AnalyzerImpl(source_, program_).run();
}

} // namespace vex
