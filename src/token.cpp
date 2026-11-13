#include "vex/token.hpp"

namespace vex {

std::string_view Token::text(const SourceFile& source) const noexcept {
    return source.slice(span);
}

std::string_view token_kind_name(const TokenKind kind) noexcept {
    switch (kind) {
    case TokenKind::Invalid: return "INVALID";
    case TokenKind::EndOfFile: return "EOF";
    case TokenKind::Newline: return "NEWLINE";
    case TokenKind::Identifier: return "IDENTIFIER";
    case TokenKind::Integer: return "INTEGER";
    case TokenKind::Float: return "FLOAT";
    case TokenKind::String: return "STRING";
    case TokenKind::KwMake: return "MAKE";
    case TokenKind::KwLet: return "LET";
    case TokenKind::KwConst: return "CONST";
    case TokenKind::KwIf: return "IF";
    case TokenKind::KwElse: return "ELSE";
    case TokenKind::KwWhile: return "WHILE";
    case TokenKind::KwBreak: return "BREAK";
    case TokenKind::KwContinue: return "CONTINUE";
    case TokenKind::KwRepeat: return "REPEAT";
    case TokenKind::KwTimes: return "TIMES";
    case TokenKind::KwFor: return "FOR";
    case TokenKind::KwIn: return "IN";
    case TokenKind::KwReturn: return "RETURN";
    case TokenKind::KwTrue: return "TRUE";
    case TokenKind::KwFalse: return "FALSE";
    case TokenKind::KwInt: return "TYPE_INT";
    case TokenKind::KwFloat: return "TYPE_FLOAT";
    case TokenKind::KwBool: return "TYPE_BOOL";
    case TokenKind::KwString: return "TYPE_STRING";
    case TokenKind::LeftParen: return "LEFT_PAREN";
    case TokenKind::RightParen: return "RIGHT_PAREN";
    case TokenKind::LeftBrace: return "LEFT_BRACE";
    case TokenKind::RightBrace: return "RIGHT_BRACE";
    case TokenKind::LeftBracket: return "LEFT_BRACKET";
    case TokenKind::RightBracket: return "RIGHT_BRACKET";
    case TokenKind::Comma: return "COMMA";
    case TokenKind::Colon: return "COLON";
    case TokenKind::Arrow: return "ARROW";
    case TokenKind::Plus: return "PLUS";
    case TokenKind::Minus: return "MINUS";
    case TokenKind::Star: return "STAR";
    case TokenKind::Slash: return "SLASH";
    case TokenKind::Percent: return "PERCENT";
    case TokenKind::PlusEqual: return "PLUS_EQUAL";
    case TokenKind::MinusEqual: return "MINUS_EQUAL";
    case TokenKind::StarEqual: return "STAR_EQUAL";
    case TokenKind::SlashEqual: return "SLASH_EQUAL";
    case TokenKind::PercentEqual: return "PERCENT_EQUAL";
    case TokenKind::Equal: return "EQUAL";
    case TokenKind::EqualEqual: return "EQUAL_EQUAL";
    case TokenKind::Bang: return "BANG";
    case TokenKind::BangEqual: return "BANG_EQUAL";
    case TokenKind::Less: return "LESS";
    case TokenKind::LessEqual: return "LESS_EQUAL";
    case TokenKind::Greater: return "GREATER";
    case TokenKind::GreaterEqual: return "GREATER_EQUAL";
    case TokenKind::AndAnd: return "AND_AND";
    case TokenKind::OrOr: return "OR_OR";
    case TokenKind::Count: return "COUNT";
    }
    return "UNKNOWN";
}

bool token_has_lexeme_value(const TokenKind kind) noexcept {
    return kind == TokenKind::Invalid || kind == TokenKind::Identifier ||
           kind == TokenKind::Integer || kind == TokenKind::Float ||
           kind == TokenKind::String;
}

} // namespace vex
