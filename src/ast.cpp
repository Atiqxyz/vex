#include "vex/ast.hpp"
#include <type_traits>
namespace vex {
namespace {
void indent(std::ostream& output, const std::size_t depth) {
    for (std::size_t index = 0; index < depth; ++index) output << "  ";
}
void print_expression(std::ostream& output, const SourceFile& source,
                      const Expr& expression, const std::size_t depth) {
    std::visit([&](const auto& node) {
        using Node = std::decay_t<decltype(node)>;
        indent(output, depth);
        if constexpr (std::is_same_v<Node, LiteralExpr>) {
            output << token_kind_name(node.token_kind) << " "
                   << source.slice(expression.span) << '\n';
        } else if constexpr (std::is_same_v<Node, NameExpr>) {
            output << "Name " << source.slice(node.name) << '\n';
        } else if constexpr (std::is_same_v<Node, UnaryExpr>) {
            output << "Unary " << token_kind_name(node.op) << '\n';
            if (node.operand) print_expression(output, source, *node.operand, depth + 1);
        } else if constexpr (std::is_same_v<Node, BinaryExpr>) {
            output << "Binary " << token_kind_name(node.op) << '\n';
            if (node.left) print_expression(output, source, *node.left, depth + 1);
            if (node.right) print_expression(output, source, *node.right, depth + 1);
        } else if constexpr (std::is_same_v<Node, AssignmentExpr>) {
            output << "Assignment\n";
            if (node.target) print_expression(output, source, *node.target, depth + 1);
            if (node.value) print_expression(output, source, *node.value, depth + 1);
        } else if constexpr (std::is_same_v<Node, CallExpr>) {
            output << "Call\n";
            if (node.callee) print_expression(output, source, *node.callee, depth + 1);
            for (const ExprPtr& argument : node.arguments)
                if (argument) print_expression(output, source, *argument, depth + 1);
        } else if constexpr (std::is_same_v<Node, GroupExpr>) {
            output << (node.multiline ? "MultilineExpression\n" : "Grouping\n");
            if (node.expression)
                print_expression(output, source, *node.expression, depth + 1);
        } else if constexpr (std::is_same_v<Node, ArrayLiteralExpr>) {
            output << "ArrayLiteral (" << node.elements.size() << " element(s))\n";
            for (const ExprPtr& element : node.elements)
                if (element) print_expression(output, source, *element, depth + 1);
        } else if constexpr (std::is_same_v<Node, IndexExpr>) {
            output << "Index\n";
            if (node.object) print_expression(output, source, *node.object, depth + 1);
            if (node.index) print_expression(output, source, *node.index, depth + 1);
        }
    }, expression.data);
}
void print_statement(std::ostream& output, const SourceFile& source,
                     const Statement& statement, const std::size_t depth) {
    std::visit([&](const auto& node) {
        using Node = std::decay_t<decltype(node)>;
        indent(output, depth);
        if constexpr (std::is_same_v<Node, FunctionDeclStmt>) {
            output << "Function " << source.slice(node.name);
            if (node.return_type) output << " -> " << source.slice(*node.return_type);
            output << '\n';
            for (const Parameter& parameter : node.parameters) {
                indent(output, depth + 1);
                output << "Parameter " << source.slice(parameter.name);
                if (parameter.type_annotation)
                    output << ": " << source.slice(*parameter.type_annotation);
                output << '\n';
            }
            if (node.body) print_statement(output, source, *node.body, depth + 1);
        } else if constexpr (std::is_same_v<Node, VariableDeclStmt>) {
            output << (node.immutable ? "Const " : "Let ") << source.slice(node.name);
            if (node.type_annotation) output << ": " << source.slice(*node.type_annotation);
            output << '\n';
            if (node.initializer)
                print_expression(output, source, *node.initializer, depth + 1);
        } else if constexpr (std::is_same_v<Node, ReturnStmt>) {
            output << "Return\n";
            if (node.value) print_expression(output, source, *node.value, depth + 1);
        } else if constexpr (std::is_same_v<Node, ExpressionStmt>) {
            output << "ExpressionStatement\n";
            if (node.expression)
                print_expression(output, source, *node.expression, depth + 1);
        } else if constexpr (std::is_same_v<Node, BlockStmt>) {
            output << "Block\n";
            for (const StmtPtr& child : node.statements)
                if (child) print_statement(output, source, *child, depth + 1);
        } else if constexpr (std::is_same_v<Node, IfStmt>) {
            output << "If\n";
            if (node.condition)
                print_expression(output, source, *node.condition, depth + 1);
            if (node.then_branch)
                print_statement(output, source, *node.then_branch, depth + 1);
            if (node.else_branch) {
                indent(output, depth);
                output << "Else\n";
                print_statement(output, source, *node.else_branch, depth + 1);
            }
        } else if constexpr (std::is_same_v<Node, WhileStmt>) {
            output << "While\n";
            if (node.condition) print_expression(output, source, *node.condition, depth + 1);
            if (node.body) print_statement(output, source, *node.body, depth + 1);
        } else if constexpr (std::is_same_v<Node, BreakStmt>) {
            output << "Break\n";
        } else if constexpr (std::is_same_v<Node, ContinueStmt>) {
            output << "Continue\n";
        } else if constexpr (std::is_same_v<Node, RepeatStmt>) {
            output << "Repeat\n";
            if (node.count) print_expression(output, source, *node.count, depth + 1);
            if (node.body) print_statement(output, source, *node.body, depth + 1);
        } else if constexpr (std::is_same_v<Node, ForStmt>) {
            output << "For " << source.slice(node.variable) << '\n';
            if (node.iterable)
                print_expression(output, source, *node.iterable, depth + 1);
            if (node.body) print_statement(output, source, *node.body, depth + 1);
        }
    }, statement.data);
}
} // namespace
void print_ast(std::ostream& output, const SourceFile& source, const Program& program) {
    output << "Program\n";
    for (const StmtPtr& statement : program.statements)
        if (statement) print_statement(output, source, *statement, 1);
}
} // namespace vex
