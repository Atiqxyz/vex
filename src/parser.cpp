#include "vex/parser.hpp"
#include <algorithm>
#include <utility>
namespace vex {
bool ParseResult::has_errors() const noexcept {
    return std::any_of(diagnostics.begin(), diagnostics.end(),
        [](const Diagnostic& diagnostic) {
            return diagnostic.severity == DiagnosticSeverity::Error;
        });
}
Parser::Parser(const SourceFile& source, const std::vector<Token>& tokens)
    : source_(source), tokens_(tokens) {}
const Token& Parser::current() const noexcept {
    return tokens_[std::min(current_, tokens_.size() - 1)];
}
const Token& Parser::previous() const noexcept {
    return tokens_[current_ == 0 ? 0 : current_ - 1];
}
bool Parser::check(const TokenKind kind) const noexcept { return current().kind == kind; }
bool Parser::match(const TokenKind kind) noexcept {
    if (!check(kind)) return false;
    static_cast<void>(advance());
    return true;
}
Token Parser::advance() noexcept {
    const Token token = current();
    if (token.kind != TokenKind::EndOfFile && current_ + 1 < tokens_.size()) ++current_;
    return token;
}
Token Parser::expect(const TokenKind kind, const char* message) {
    if (check(kind)) return advance();
    report_current("PAR001", message);
    const std::size_t offset = current().span.begin;
    return Token{kind, SourceSpan{offset, offset}};
}
void Parser::report_current(const char* code, const char* message) {
    diagnostics_.error(code, message, current().span);
}
void Parser::skip_newlines() noexcept {
    while (match(TokenKind::Newline)) {}
}
void Parser::synchronize() noexcept {
    while (!check(TokenKind::EndOfFile) && !check(TokenKind::Newline) &&
           !check(TokenKind::RightBrace)) static_cast<void>(advance());
}
ParseResult Parser::parse() {
    Program program;
    if (tokens_.empty()) {
        diagnostics_.error("PAR000", "parser received an empty token stream", {0, 0});
        return ParseResult{std::move(program), diagnostics_.take()};
    }
    skip_newlines();
    while (!check(TokenKind::EndOfFile)) {
        const std::size_t before = current_;
        StmtPtr statement = parse_statement(true);
        if (statement) program.statements.push_back(std::move(statement));
        if (!check(TokenKind::EndOfFile) && !check(TokenKind::Newline) &&
            !check(TokenKind::RightBrace)) {
            report_current("PAR002", "expected a newline between statements");
            synchronize();
        }
        skip_newlines();
        if (current_ == before && !check(TokenKind::EndOfFile))
            static_cast<void>(advance());
    }
    return ParseResult{std::move(program), diagnostics_.take()};
}
StmtPtr Parser::parse_statement(const bool top_level) {
    if (match(TokenKind::KwLet)) return parse_variable(previous(), false);
    if (match(TokenKind::KwConst)) return parse_variable(previous(), true);
    if (match(TokenKind::KwReturn)) return parse_return(previous());
    if (match(TokenKind::KwIf)) return parse_if(previous());
    if (match(TokenKind::KwRepeat)) return parse_repeat(previous());
    if (match(TokenKind::KwFor)) return parse_for(previous());
    if (match(TokenKind::KwMake)) {
        if (!top_level) {
            report_current("PAR003", "function declarations are only allowed at file scope");
            synchronize();
            return nullptr;
        }
        return parse_function(previous());
    }
    if (match(TokenKind::LeftBrace)) return parse_block_after_open(previous());
    const std::size_t begin = current().span.begin;
    ExprPtr expression = parse_expression();
    if (!expression) {
        if (!check(TokenKind::EndOfFile) && !check(TokenKind::Newline) &&
            !check(TokenKind::RightBrace)) static_cast<void>(advance());
        return nullptr;
    }
    const std::size_t end = expression->span.end;
    return std::make_unique<Statement>(
        Statement{SourceSpan{begin, end}, ExpressionStmt{std::move(expression)}});
}
StmtPtr Parser::parse_function(const Token& start) {
    const Token name = expect(TokenKind::Identifier, "expected a function name after 'make'");
    static_cast<void>(expect(TokenKind::LeftParen, "expected '(' after function name"));
    std::vector<Parameter> parameters;
    if (!check(TokenKind::RightParen) && !check(TokenKind::EndOfFile)) {
        do {
            const Token parameter_name = expect(TokenKind::Identifier, "expected a parameter name");
            std::optional<SourceSpan> type_annotation;
            if (match(TokenKind::Colon)) type_annotation = parse_type_annotation();
            parameters.push_back(Parameter{parameter_name.span, type_annotation});
        } while (match(TokenKind::Comma) && !check(TokenKind::RightParen));
    }
    static_cast<void>(expect(TokenKind::RightParen, "expected ')' after function parameters"));
    std::optional<SourceSpan> return_type;
    if (match(TokenKind::Arrow)) return_type = parse_type_annotation();
    StmtPtr body = parse_block();
    if (!body) return nullptr;
    return std::make_unique<Statement>(Statement{
        SourceSpan{start.span.begin, body->span.end},
        FunctionDeclStmt{name.span, std::move(parameters), return_type, std::move(body)}});
}
StmtPtr Parser::parse_variable(const Token& start, const bool immutable) {
    const Token name = expect(TokenKind::Identifier,
                              "expected a variable name after declaration keyword");
    std::optional<SourceSpan> type_annotation;
    if (match(TokenKind::Colon)) type_annotation = parse_type_annotation();
    static_cast<void>(expect(TokenKind::Equal, "expected '=' before the variable initializer"));
    ExprPtr initializer = parse_expression();
    const std::size_t end = initializer ? initializer->span.end : previous().span.end;
    return std::make_unique<Statement>(Statement{
        SourceSpan{start.span.begin, end},
        VariableDeclStmt{name.span, immutable, type_annotation, std::move(initializer)}});
}
StmtPtr Parser::parse_return(const Token& start) {
    ExprPtr value;
    if (!check(TokenKind::Newline) && !check(TokenKind::RightBrace) &&
        !check(TokenKind::EndOfFile)) value = parse_expression();
    const std::size_t end = value ? value->span.end : start.span.end;
    return std::make_unique<Statement>(
        Statement{SourceSpan{start.span.begin, end}, ReturnStmt{std::move(value)}});
}
StmtPtr Parser::parse_if(const Token& start) {
    ExprPtr condition = parse_expression();
    StmtPtr then_branch = parse_block();
    if (!then_branch) return nullptr;
    while (match(TokenKind::Newline)) {}
    StmtPtr else_branch;
    if (match(TokenKind::KwElse)) {
        if (match(TokenKind::KwIf)) else_branch = parse_if(previous());
        else else_branch = parse_block();
    }
    const std::size_t end = else_branch ? else_branch->span.end : then_branch->span.end;
    return std::make_unique<Statement>(Statement{
        SourceSpan{start.span.begin, end},
        IfStmt{std::move(condition), std::move(then_branch), std::move(else_branch)}});
}
StmtPtr Parser::parse_repeat(const Token& start) {
    ExprPtr count = parse_expression();
    static_cast<void>(expect(TokenKind::KwTimes, "expected 'times' after repeat count"));
    StmtPtr body = parse_block();
    if (!body) return nullptr;
    return std::make_unique<Statement>(Statement{
        SourceSpan{start.span.begin, body->span.end},
        RepeatStmt{std::move(count), std::move(body)}});
}
StmtPtr Parser::parse_for(const Token& start) {
    const Token variable = expect(TokenKind::Identifier, "expected an iteration variable after 'for'");
    static_cast<void>(expect(TokenKind::KwIn, "expected 'in' after iteration variable"));
    ExprPtr iterable = parse_expression();
    StmtPtr body = parse_block();
    if (!body) return nullptr;
    return std::make_unique<Statement>(Statement{
        SourceSpan{start.span.begin, body->span.end},
        ForStmt{variable.span, std::move(iterable), std::move(body)}});
}
StmtPtr Parser::parse_block() {
    if (!check(TokenKind::LeftBrace)) {
        report_current("PAR004", "expected '{' to begin a block");
        return nullptr;
    }
    return parse_block_after_open(advance());
}
StmtPtr Parser::parse_block_after_open(const Token& opening) {
    std::vector<StmtPtr> statements;
    skip_newlines();
    while (!check(TokenKind::RightBrace) && !check(TokenKind::EndOfFile)) {
        const std::size_t before = current_;
        StmtPtr statement = parse_statement(false);
        if (statement) statements.push_back(std::move(statement));
        if (!check(TokenKind::RightBrace) && !check(TokenKind::EndOfFile) &&
            !check(TokenKind::Newline)) {
            report_current("PAR002", "expected a newline between statements");
            synchronize();
        }
        skip_newlines();
        if (current_ == before && !check(TokenKind::RightBrace) &&
            !check(TokenKind::EndOfFile)) static_cast<void>(advance());
    }
    const Token closing = expect(TokenKind::RightBrace, "expected '}' to close block");
    return std::make_unique<Statement>(Statement{
        SourceSpan{opening.span.begin, closing.span.end}, BlockStmt{std::move(statements)}});
}
std::optional<SourceSpan> Parser::parse_type_annotation() {
    if (check(TokenKind::Identifier) || check(TokenKind::KwInt) ||
        check(TokenKind::KwFloat) || check(TokenKind::KwBool) ||
        check(TokenKind::KwString)) return advance().span;
    report_current("PAR005", "expected a type name");
    return std::nullopt;
}
ExprPtr Parser::parse_expression(const bool allow_newlines, const int minimum_precedence) {
    if (allow_newlines) skip_newlines();
    ExprPtr left = parse_prefix(allow_newlines);
    if (!left) return nullptr;
    while (true) {
        if (allow_newlines) skip_newlines();
        if (check(TokenKind::LeftParen) && minimum_precedence <= 9) {
            static_cast<void>(advance());
            std::vector<ExprPtr> arguments;
            if (allow_newlines) skip_newlines();
            while (!check(TokenKind::RightParen) && !check(TokenKind::EndOfFile)) {
                ExprPtr argument = parse_expression(allow_newlines);
                if (!argument) break;
                arguments.push_back(std::move(argument));
                if (allow_newlines) skip_newlines();
                if (!match(TokenKind::Comma)) break;
                if (allow_newlines) skip_newlines();
            }
            const Token closing = expect(TokenKind::RightParen,
                                         "expected ')' after call arguments");
            const std::size_t begin = left->span.begin;
            left = make_expression(SourceSpan{begin, closing.span.end},
                                   CallExpr{std::move(left), std::move(arguments)});
            continue;
        }
        const int operator_precedence = precedence(current().kind);
        if (operator_precedence < minimum_precedence) break;
        const Token op = advance();
        const bool right_associative = op.kind == TokenKind::Equal;
        const int next_minimum = right_associative
            ? operator_precedence : operator_precedence + 1;
        ExprPtr right = parse_expression(allow_newlines, next_minimum);
        if (!right) {
            report_current("PAR006", "expected an expression after the operator");
            break;
        }
        const SourceSpan span{left->span.begin, right->span.end};
        if (op.kind == TokenKind::Equal) {
            if (!is_assignment_target(*left))
                diagnostics_.error("PAR007",
                    "the left side of an assignment must be a variable name", left->span);
            left = make_expression(span, AssignmentExpr{std::move(left), std::move(right)});
        } else {
            left = make_expression(span, BinaryExpr{op.kind, std::move(left), std::move(right)});
        }
    }
    return left;
}
ExprPtr Parser::parse_prefix(const bool allow_newlines) {
    if (match(TokenKind::Minus) || match(TokenKind::Plus) || match(TokenKind::Bang)) {
        const Token op = previous();
        ExprPtr operand = parse_expression(allow_newlines, 8);
        if (!operand) {
            report_current("PAR008", "expected an expression after the unary operator");
            return nullptr;
        }
        return make_expression(SourceSpan{op.span.begin, operand->span.end},
                               UnaryExpr{op.kind, std::move(operand)});
    }
    if (match(TokenKind::LeftParen)) {
        const Token opening = previous();
        ExprPtr expression = parse_expression(allow_newlines);
        const Token closing = expect(TokenKind::RightParen,
                                     "expected ')' after grouped expression");
        if (!expression) return nullptr;
        return make_expression(SourceSpan{opening.span.begin, closing.span.end},
                               GroupExpr{std::move(expression), false});
    }
    if (match(TokenKind::LeftBracket)) {
        const Token opening = previous();
        skip_newlines();
        ExprPtr expression = parse_expression(true);
        skip_newlines();
        const Token closing = expect(TokenKind::RightBracket,
                                     "expected ']' after multiline expression");
        if (!expression) {
            diagnostics_.error("PAR009", "multiline expression cannot be empty", opening.span);
            return nullptr;
        }
        return make_expression(SourceSpan{opening.span.begin, closing.span.end},
                               GroupExpr{std::move(expression), true});
    }
    if (check(TokenKind::Integer) || check(TokenKind::Float) ||
        check(TokenKind::String) || check(TokenKind::KwTrue) ||
        check(TokenKind::KwFalse)) {
        const Token literal = advance();
        return make_expression(literal.span, LiteralExpr{literal.kind});
    }
    if (match(TokenKind::Identifier)) {
        const Token name = previous();
        return make_expression(name.span, NameExpr{name.span});
    }
    if (!check(TokenKind::EndOfFile) && !check(TokenKind::Newline) &&
        !check(TokenKind::RightParen) && !check(TokenKind::RightBracket) &&
        !check(TokenKind::RightBrace) && !check(TokenKind::Comma)) {
        report_current("PAR010", "expected an expression");
        static_cast<void>(advance());
    } else {
        report_current("PAR010", "expected an expression");
    }
    return nullptr;
}
ExprPtr Parser::make_expression(
    const SourceSpan span,
    std::variant<LiteralExpr, NameExpr, UnaryExpr, BinaryExpr,
                 AssignmentExpr, CallExpr, GroupExpr> data) {
    return std::make_unique<Expr>(Expr{span, std::move(data)});
}
int Parser::precedence(const TokenKind kind) noexcept {
    switch (kind) {
    case TokenKind::Equal: return 1;
    case TokenKind::OrOr: return 2;
    case TokenKind::AndAnd: return 3;
    case TokenKind::EqualEqual:
    case TokenKind::BangEqual: return 4;
    case TokenKind::Less:
    case TokenKind::LessEqual:
    case TokenKind::Greater:
    case TokenKind::GreaterEqual: return 5;
    case TokenKind::Plus:
    case TokenKind::Minus: return 6;
    case TokenKind::Star:
    case TokenKind::Slash:
    case TokenKind::Percent: return 7;
    default: return 0;
    }
}
bool Parser::is_assignment_target(const Expr& expression) noexcept {
    return std::holds_alternative<NameExpr>(expression.data);
}
} // namespace vex
