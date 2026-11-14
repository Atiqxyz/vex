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
    if (match(TokenKind::KwWhile)) return parse_while(previous());
    if (match(TokenKind::KwBreak)) {
        const Token start = previous();
        return std::make_unique<Statement>(Statement{start.span, BreakStmt{}});
    }
    if (match(TokenKind::KwContinue)) {
        const Token start = previous();
        return std::make_unique<Statement>(Statement{start.span, ContinueStmt{}});
    }
    if (match(TokenKind::KwRepeat)) return parse_repeat(previous());
    if (match(TokenKind::KwFor)) return parse_for(previous());
    if (match(TokenKind::KwStruct)) {
        if (!top_level) {
            report_current("PAR012", "structure declarations are only allowed at file scope");
            synchronize();
            return nullptr;
        }
        return parse_struct(previous());
    }
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
StmtPtr Parser::parse_struct(const Token& start) {
    const Token name = expect(TokenKind::Identifier, "expected a structure name after 'struct'");
    static_cast<void>(expect(TokenKind::LeftBrace, "expected '{' after structure name"));
    std::vector<StructFieldDecl> fields;
    skip_newlines();
    while (!check(TokenKind::RightBrace) && !check(TokenKind::EndOfFile)) {
        const Token field = expect(TokenKind::Identifier, "expected a field name");
        static_cast<void>(expect(TokenKind::Colon, "expected ':' after structure field name"));
        const auto type = parse_type_annotation();
        fields.push_back(StructFieldDecl{field.span, type});
        skip_newlines();
        if (!match(TokenKind::Comma)) {
            if (!check(TokenKind::RightBrace)) {
                report_current("PAR013", "expected ',' between structure fields");
                synchronize();
                skip_newlines();
                if (!check(TokenKind::RightBrace) && !check(TokenKind::EndOfFile))
                    static_cast<void>(match(TokenKind::Comma));
            }
        }
        skip_newlines();
    }
    const Token closing = expect(TokenKind::RightBrace, "expected '}' to close structure declaration");
    return std::make_unique<Statement>(Statement{
        SourceSpan{start.span.begin, closing.span.end},
        StructDeclStmt{name.span, std::move(fields)}});
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
    const std::size_t after_then_branch = current_;
    while (match(TokenKind::Newline)) {}
    StmtPtr else_branch;
    if (match(TokenKind::KwElse)) {
        if (match(TokenKind::KwIf)) else_branch = parse_if(previous());
        else else_branch = parse_block();
    } else {
        // Newlines belong to the surrounding statement list unless they
        // introduce an else branch.
        current_ = after_then_branch;
    }
    const std::size_t end = else_branch ? else_branch->span.end : then_branch->span.end;
    return std::make_unique<Statement>(Statement{
        SourceSpan{start.span.begin, end},
        IfStmt{std::move(condition), std::move(then_branch), std::move(else_branch)}});
}
StmtPtr Parser::parse_while(const Token& start) {
    ExprPtr condition = parse_expression();
    StmtPtr body = parse_block();
    if (!body) return nullptr;
    return std::make_unique<Statement>(Statement{
        SourceSpan{start.span.begin, body->span.end},
        WhileStmt{std::move(condition), std::move(body)}});
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
    if (!(check(TokenKind::Identifier) || check(TokenKind::KwInt) ||
          check(TokenKind::KwFloat) || check(TokenKind::KwBool) ||
          check(TokenKind::KwString))) {
        report_current("PAR005", "expected a type name");
        return std::nullopt;
    }
    const Token type = advance();
    if (!match(TokenKind::LeftBracket)) return type.span;
    if (match(TokenKind::RightBracket))
        return SourceSpan{type.span.begin, previous().span.end};
    static_cast<void>(expect(TokenKind::Integer,
                             "expected a positive fixed-array length or ']' after '['"));
    const Token closing = expect(TokenKind::RightBracket,
                                 "expected ']' after fixed-array length");
    return SourceSpan{type.span.begin, closing.span.end};
}
ExprPtr Parser::parse_expression(const bool allow_newlines, const int minimum_precedence) {
    if (allow_newlines) skip_newlines();
    ExprPtr left = parse_prefix(allow_newlines);
    if (!left) return nullptr;
    while (true) {
        if (allow_newlines) skip_newlines();
        if (check(TokenKind::LeftBrace) && minimum_precedence <= 12) {
            // Newlines immediately after the opening brace are allowed in
            // multiline structure literals, so skip them during lookahead too.
            std::size_t field_token = current_ + 1;
            while (field_token < tokens_.size() &&
                   tokens_[field_token].kind == TokenKind::Newline) {
                ++field_token;
            }
            if (field_token + 1 < tokens_.size() &&
                tokens_[field_token].kind == TokenKind::Identifier &&
                tokens_[field_token + 1].kind == TokenKind::Colon) {
                const auto* type_name = std::get_if<NameExpr>(&left->data);
                if (type_name) {
                    static_cast<void>(advance());
                    std::vector<StructLiteralField> fields;
                    skip_newlines();
                    while (!check(TokenKind::RightBrace) &&
                           !check(TokenKind::EndOfFile)) {
                        const Token field = expect(
                            TokenKind::Identifier,
                            "expected a field name in structure literal");
                        static_cast<void>(expect(
                            TokenKind::Colon,
                            "expected ':' after structure literal field name"));
                        ExprPtr value = parse_expression(true);
                        fields.push_back(
                            StructLiteralField{field.span, std::move(value)});
                        skip_newlines();
                        if (!match(TokenKind::Comma)) break;
                        skip_newlines();
                    }
                    const Token closing = expect(
                        TokenKind::RightBrace,
                        "expected '}' after structure literal");
                    const std::size_t begin = left->span.begin;
                    left = make_expression(SourceSpan{begin, closing.span.end},
                        StructLiteralExpr{type_name->name, std::move(fields)});
                    continue;
                }
            }
        }
        if (check(TokenKind::Dot) && minimum_precedence <= 12) {
            static_cast<void>(advance());
            const Token member = expect(TokenKind::Identifier,
                                        "expected a field name after '.'");
            const std::size_t begin = left->span.begin;
            left = make_expression(SourceSpan{begin, member.span.end},
                                   MemberExpr{std::move(left), member.span});
            continue;
        }
        if (check(TokenKind::LeftBracket) && minimum_precedence <= 11) {
            static_cast<void>(advance());
            skip_newlines();
            ExprPtr index = parse_expression(true);
            const Token closing = expect(TokenKind::RightBracket,
                                         "expected ']' after array index");
            if (!index) return nullptr;
            const std::size_t begin = left->span.begin;
            left = make_expression(SourceSpan{begin, closing.span.end},
                                   IndexExpr{std::move(left), std::move(index)});
            continue;
        }
        if (check(TokenKind::LeftParen) && minimum_precedence <= 10) {
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
        const bool right_associative = op.kind == TokenKind::Equal ||
            op.kind == TokenKind::PlusEqual || op.kind == TokenKind::MinusEqual ||
            op.kind == TokenKind::StarEqual || op.kind == TokenKind::SlashEqual ||
            op.kind == TokenKind::PercentEqual;
        const int next_minimum = right_associative
            ? operator_precedence : operator_precedence + 1;
        ExprPtr right = parse_expression(allow_newlines, next_minimum);
        if (!right) {
            report_current("PAR006", "expected an expression after the operator");
            break;
        }
        const SourceSpan span{left->span.begin, right->span.end};
        const bool is_compound_assignment =
            op.kind == TokenKind::PlusEqual || op.kind == TokenKind::MinusEqual ||
            op.kind == TokenKind::StarEqual || op.kind == TokenKind::SlashEqual ||
            op.kind == TokenKind::PercentEqual;
        if (op.kind == TokenKind::Range || op.kind == TokenKind::RangeInclusive) {
            if (std::holds_alternative<RangeExpr>(left->data)) {
                diagnostics_.error("PAR011", "range operators cannot be chained", span);
                left = make_expression(span,
                    BinaryExpr{op.kind, std::move(left), std::move(right)});
            } else {
                left = make_expression(span, RangeExpr{
                    std::move(left), std::move(right),
                    op.kind == TokenKind::RangeInclusive});
            }
        } else if (op.kind == TokenKind::Equal || is_compound_assignment) {
            if (!is_assignment_target(*left)) {
                diagnostics_.error("PAR007",
                    "the left side of an assignment must be a variable, array element, or structure field",
                    left->span);
                left = make_expression(span, BinaryExpr{op.kind, std::move(left), std::move(right)});
            } else if (op.kind == TokenKind::Equal) {
                left = make_expression(span, AssignmentExpr{std::move(left), std::move(right)});
            } else if (const auto* target = std::get_if<NameExpr>(&left->data)) {
                const SourceSpan target_span = target->name;
                const TokenKind binary_op =
                    op.kind == TokenKind::PlusEqual ? TokenKind::Plus :
                    op.kind == TokenKind::MinusEqual ? TokenKind::Minus :
                    op.kind == TokenKind::StarEqual ? TokenKind::Star :
                    op.kind == TokenKind::SlashEqual ? TokenKind::Slash :
                    TokenKind::Percent;
                ExprPtr target_read = make_expression(target_span, NameExpr{target_span});
                ExprPtr combined = make_expression(span,
                    BinaryExpr{binary_op, std::move(target_read), std::move(right)});
                left = make_expression(span,
                    AssignmentExpr{std::move(left), std::move(combined)});
            } else {
                diagnostics_.error("PAR007",
                    "compound assignment to an array element is not supported yet",
                    left->span);
                left = make_expression(span, BinaryExpr{op.kind, std::move(left), std::move(right)});
            }
        } else {
            left = make_expression(span, BinaryExpr{op.kind, std::move(left), std::move(right)});
        }
    }
    return left;
}
ExprPtr Parser::parse_prefix(const bool allow_newlines) {
    if (match(TokenKind::Minus) || match(TokenKind::Plus) || match(TokenKind::Bang)) {
        const Token op = previous();
        ExprPtr operand = parse_expression(allow_newlines, 9);
        if (!operand) {
            report_current("PAR008", "expected an expression after the unary operator");
            return nullptr;
        }
        const SourceSpan span{op.span.begin, operand->span.end};
        return make_expression(span, UnaryExpr{op.kind, std::move(operand)});
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
        std::vector<ExprPtr> elements;
        if (match(TokenKind::RightBracket)) {
            return make_expression(SourceSpan{opening.span.begin, previous().span.end},
                                   ArrayLiteralExpr{std::move(elements)});
        }
        ExprPtr first = parse_expression(true);
        if (!first) return nullptr;
        if (match(TokenKind::Comma)) {
            elements.push_back(std::move(first));
            skip_newlines();
            while (!check(TokenKind::RightBracket) && !check(TokenKind::EndOfFile)) {
                ExprPtr element = parse_expression(true);
                if (!element) break;
                elements.push_back(std::move(element));
                skip_newlines();
                if (!match(TokenKind::Comma)) break;
                skip_newlines();
            }
            const Token closing = expect(TokenKind::RightBracket,
                                         "expected ']' after array elements");
            return make_expression(SourceSpan{opening.span.begin, closing.span.end},
                                   ArrayLiteralExpr{std::move(elements)});
        }
        skip_newlines();
        const Token closing = expect(TokenKind::RightBracket,
                                     "expected ']' after multiline expression");
        return make_expression(SourceSpan{opening.span.begin, closing.span.end},
                               GroupExpr{std::move(first), true});
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
                 AssignmentExpr, CallExpr, GroupExpr, ArrayLiteralExpr,
                 IndexExpr, RangeExpr, MemberExpr, StructLiteralExpr> data) {
    return std::make_unique<Expr>(Expr{span, std::move(data)});
}
int Parser::precedence(const TokenKind kind) noexcept {
    switch (kind) {
    case TokenKind::Equal:
    case TokenKind::PlusEqual:
    case TokenKind::MinusEqual:
    case TokenKind::StarEqual:
    case TokenKind::SlashEqual:
    case TokenKind::PercentEqual: return 1;
    case TokenKind::OrOr: return 2;
    case TokenKind::AndAnd: return 3;
    case TokenKind::Range:
    case TokenKind::RangeInclusive: return 4;
    case TokenKind::EqualEqual:
    case TokenKind::BangEqual: return 5;
    case TokenKind::Less:
    case TokenKind::LessEqual:
    case TokenKind::Greater:
    case TokenKind::GreaterEqual: return 6;
    case TokenKind::Plus:
    case TokenKind::Minus: return 7;
    case TokenKind::Star:
    case TokenKind::Slash:
    case TokenKind::Percent: return 8;
    default: return 0;
    }
}
bool Parser::is_assignment_target(const Expr& expression) noexcept {
    return std::holds_alternative<NameExpr>(expression.data) ||
           std::holds_alternative<IndexExpr>(expression.data) ||
           std::holds_alternative<MemberExpr>(expression.data);
}
} // namespace vex
