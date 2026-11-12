#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace vex {

enum class TokenKind {
    EndOfFile,
    Newline,
    Identifier,
    Integer,
    Float,
    String,

    KwMake,
    KwLet,
    KwConst,
    KwIf,
    KwElse,
    KwRepeat,
    KwTimes,
    KwFor,
    KwIn,
    KwReturn,
    KwTrue,
    KwFalse,
    KwInt,
    KwFloat,
    KwBool,
    KwString,

    LeftParen,
    RightParen,
    LeftBrace,
    RightBrace,
    LeftBracket,
    RightBracket,
    Comma,
    Colon,
    Arrow,

    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    Equal,
    EqualEqual,
    Bang,
    BangEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    AndAnd,
    OrOr
};

struct Token {
    TokenKind kind{TokenKind::EndOfFile};
    std::string lexeme;
    std::size_t line{1};
    std::size_t column{1};
};

[[nodiscard]] std::string_view token_kind_name(TokenKind kind) noexcept;
[[nodiscard]] bool token_has_lexeme_value(TokenKind kind) noexcept;

} // namespace vex
