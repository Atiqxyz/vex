#pragma once
#include "vex/source.hpp"
#include "vex/token.hpp"
#include <memory>
#include <optional>
#include <ostream>
#include <variant>
#include <vector>
namespace vex {
struct Expr;
using ExprPtr = std::unique_ptr<Expr>;
struct LiteralExpr { TokenKind token_kind{TokenKind::Invalid}; };
struct NameExpr { SourceSpan name{}; };
struct UnaryExpr { TokenKind op{TokenKind::Invalid}; ExprPtr operand; };
struct BinaryExpr { TokenKind op{TokenKind::Invalid}; ExprPtr left; ExprPtr right; };
struct AssignmentExpr { ExprPtr target; ExprPtr value; };
struct CallExpr { ExprPtr callee; std::vector<ExprPtr> arguments; };
struct GroupExpr { ExprPtr expression; bool multiline{false}; };
struct ArrayLiteralExpr { std::vector<ExprPtr> elements; };
struct IndexExpr { ExprPtr object; ExprPtr index; };
struct RangeExpr { ExprPtr start; ExprPtr end; bool inclusive{false}; };
struct Expr {
    SourceSpan span{};
    std::variant<LiteralExpr, NameExpr, UnaryExpr, BinaryExpr,
                 AssignmentExpr, CallExpr, GroupExpr, ArrayLiteralExpr,
                 IndexExpr, RangeExpr> data;
};
struct Parameter { SourceSpan name{}; std::optional<SourceSpan> type_annotation; };
struct Statement;
using StmtPtr = std::unique_ptr<Statement>;
struct FunctionDeclStmt {
    SourceSpan name{};
    std::vector<Parameter> parameters;
    std::optional<SourceSpan> return_type;
    StmtPtr body;
};
struct VariableDeclStmt {
    SourceSpan name{};
    bool immutable{false};
    std::optional<SourceSpan> type_annotation;
    ExprPtr initializer;
};
struct ReturnStmt { ExprPtr value; };
struct ExpressionStmt { ExprPtr expression; };
struct BlockStmt { std::vector<StmtPtr> statements; };
struct IfStmt { ExprPtr condition; StmtPtr then_branch; StmtPtr else_branch; };
struct WhileStmt { ExprPtr condition; StmtPtr body; };
struct BreakStmt {};
struct ContinueStmt {};
struct RepeatStmt { ExprPtr count; StmtPtr body; };
struct ForStmt { SourceSpan variable{}; ExprPtr iterable; StmtPtr body; };
struct Statement {
    SourceSpan span{};
    std::variant<FunctionDeclStmt, VariableDeclStmt, ReturnStmt,
                 ExpressionStmt, BlockStmt, IfStmt, WhileStmt, BreakStmt,
                 ContinueStmt, RepeatStmt, ForStmt> data;
};
struct Program { std::vector<StmtPtr> statements; };
void print_ast(std::ostream& output, const SourceFile& source, const Program& program);
} // namespace vex
