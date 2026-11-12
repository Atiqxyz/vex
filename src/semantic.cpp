#include "vex/semantic.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vex {
namespace {

enum class Type {
    Error,
    Unknown,
    Any,
    Void,
    Int,
    Float,
    Bool,
    String
};

const char* type_name(const Type type) noexcept {
    switch (type) {
    case Type::Error: return "<error>";
    case Type::Unknown: return "<unknown>";
    case Type::Any: return "any";
    case Type::Void: return "void";
    case Type::Int: return "int";
    case Type::Float: return "float";
    case Type::Bool: return "bool";
    case Type::String: return "string";
    }
    return "<unknown>";
}

bool is_numeric(const Type type) noexcept {
    return type == Type::Int || type == Type::Float;
}

bool is_assignable(const Type destination, const Type source) noexcept {
    if (destination == Type::Error || source == Type::Error ||
        destination == Type::Unknown || source == Type::Unknown ||
        destination == Type::Any || source == Type::Any) {
        return true;
    }
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
    Type type{Type::Unknown};
    bool immutable{false};
    bool is_function{false};
    std::vector<Type> parameters;
    SourceSpan declaration{};
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
        return SemanticResult{std::move(diagnostics_)};
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
    }

    void collect_function_signatures() {
        for (const StmtPtr& statement : program_.statements) {
            if (!statement) continue;
            const auto* function =
                std::get_if<FunctionDeclStmt>(&statement->data);
            if (!function) continue;

            Symbol signature;
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
            signatures_.emplace(function, std::move(signature));
        }
    }

    [[nodiscard]] Symbol* lookup(const std::string& name) {
        for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
            const auto found = scope->find(name);
            if (found != scope->end()) return &found->second;
        }
        return nullptr;
    }

    [[nodiscard]] bool define(const std::string& name, Symbol symbol,
                              const SourceSpan declaration) {
        Scope& current = scopes_.back();
        if (current.find(name) != current.end()) {
            report("SEM002", "duplicate declaration of '" + name +
                   "' in the same scope", declaration);
            return false;
        }
        current.emplace(name, std::move(symbol));
        return true;
    }

    [[nodiscard]] Type analyze_expression(const Expr& expression) {
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
            return symbol->type;
        }

        if (const auto* group = std::get_if<GroupExpr>(&expression.data)) {
            return group->expression
                ? analyze_expression(*group->expression) : Type::Error;
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
                if (!is_assignable(left, right) || !is_assignable(right, left) ||
                    left == Type::Void || right == Type::Void) {
                    report("SEM014", "equality operands must have compatible types; got " +
                           std::string(type_name(left)) + " and " + type_name(right),
                           expression.span);
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
            const Type value = assignment->value
                ? analyze_expression(*assignment->value) : Type::Error;
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
        symbol.immutable = variable.immutable;
        symbol.declaration = variable.name;
        static_cast<void>(define(spelling(variable.name), std::move(symbol),
                                 variable.name));
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
            static_cast<void>(define(spelling(parameter.name), std::move(symbol),
                                     parameter.name));
        }

        const bool was_in_function = inside_function_;
        const Type previous_return = current_return_type_;
        inside_function_ = true;
        current_return_type_ = signature.type;

        if (function.body) {
            analyze_block(*function.body, false);
            if (signature.type != Type::Void && signature.type != Type::Error &&
                !definitely_returns(*function.body)) {
                report("SEM024", "not every control-flow path returns a value",
                       statement.span);
            }
        }

        current_return_type_ = previous_return;
        inside_function_ = was_in_function;
        scopes_.pop_back();
    }

    [[nodiscard]] bool definitely_returns(const Statement& statement) const {
        if (std::holds_alternative<ReturnStmt>(statement.data)) return true;
        if (const auto* block = std::get_if<BlockStmt>(&statement.data)) {
            for (const StmtPtr& child : block->statements) {
                if (child && definitely_returns(*child)) return true;
            }
        }
        if (const auto* conditional = std::get_if<IfStmt>(&statement.data)) {
            return conditional->then_branch && conditional->else_branch &&
                   definitely_returns(*conditional->then_branch) &&
                   definitely_returns(*conditional->else_branch);
        }
        return false;
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
        if (const auto* block = std::get_if<BlockStmt>(&statement.data)) {
            static_cast<void>(block);
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
        if (const auto* repeat = std::get_if<RepeatStmt>(&statement.data)) {
            if (repeat->count) {
                const Type count = analyze_expression(*repeat->count);
                require_type(count, Type::Int, repeat->count->span,
                             "SEM030", "repeat count must be an integer");
            }
            if (repeat->body) analyze_statement(*repeat->body);
            return;
        }
        if (const auto* loop = std::get_if<ForStmt>(&statement.data)) {
            const Type iterable = loop->iterable
                ? analyze_expression(*loop->iterable) : Type::Error;
            if (iterable != Type::Error) {
                report("SEM031",
                       "for-in is parsed but cannot be type-checked until collection types are implemented",
                       loop->iterable ? loop->iterable->span : loop->variable);
            }
            scopes_.emplace_back();
            Symbol iteration_variable;
            iteration_variable.type = Type::Unknown;
            iteration_variable.declaration = loop->variable;
            static_cast<void>(define(spelling(loop->variable),
                                     std::move(iteration_variable), loop->variable));
            if (loop->body) analyze_statement(*loop->body);
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
    bool inside_function_{false};
    Type current_return_type_{Type::Void};
};

} // namespace

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
