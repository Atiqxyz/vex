#pragma once
#include "vex/ast.hpp"
#include "vex/diagnostic.hpp"
#include "vex/token.hpp"
#include <cstddef>
#include <optional>
#include <variant>
#include <vector>
namespace vex {
struct ParseResult {
    Program program;
    std::vector<Diagnostic> diagnostics;
    [[nodiscard]] bool has_errors() const noexcept;
};
class Parser {
public:
    Parser(const SourceFile& source, const std::vector<Token>& tokens);
    // The token stream must end with EndOfFile and must outlive the Parser.
    [[nodiscard]] ParseResult parse();
private:
    [[nodiscard]] const Token& current() const noexcept;
    [[nodiscard]] const Token& previous() const noexcept;
    [[nodiscard]] bool check(TokenKind kind) const noexcept;
    [[nodiscard]] bool match(TokenKind kind) noexcept;
    [[nodiscard]] Token advance() noexcept;
    [[nodiscard]] Token expect(TokenKind kind, const char* message);
    void report_current(const char* code, const char* message);
    void skip_newlines() noexcept;
    void synchronize() noexcept;
    [[nodiscard]] StmtPtr parse_statement(bool top_level);
    [[nodiscard]] StmtPtr parse_function(const Token& start);
    [[nodiscard]] StmtPtr parse_variable(const Token& start, bool immutable);
    [[nodiscard]] StmtPtr parse_return(const Token& start);
    [[nodiscard]] StmtPtr parse_if(const Token& start);
    [[nodiscard]] StmtPtr parse_repeat(const Token& start);
    [[nodiscard]] StmtPtr parse_for(const Token& start);
    [[nodiscard]] StmtPtr parse_block();
    [[nodiscard]] StmtPtr parse_block_after_open(const Token& opening);
    [[nodiscard]] std::optional<SourceSpan> parse_type_annotation();
    [[nodiscard]] ExprPtr parse_expression(bool allow_newlines = false,
                                           int minimum_precedence = 1);
    [[nodiscard]] ExprPtr parse_prefix(bool allow_newlines);
    [[nodiscard]] ExprPtr make_expression(
        SourceSpan span,
        std::variant<LiteralExpr, NameExpr, UnaryExpr, BinaryExpr,
                     AssignmentExpr, CallExpr, GroupExpr> data);
    [[nodiscard]] static int precedence(TokenKind kind) noexcept;
    [[nodiscard]] static bool is_assignment_target(const Expr& expression) noexcept;
    const SourceFile& source_;
    const std::vector<Token>& tokens_;
    std::size_t current_{0};
    DiagnosticEngine diagnostics_;
};
} // namespace vex
