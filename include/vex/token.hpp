#pragma once

#include "vex/source.hpp"

#include <string_view>

namespace vex {

enum class TokenKind {
    Invalid,
    EndOfFile,
    Newline,
    Identifier,
    Integer,
    Float,
    String,

    KwMake,
    KwStruct,
    KwLet,
    KwConst,
    KwIf,
    KwElse,
    KwWhile,
    KwBreak,
    KwContinue,
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
    Dot,
    Arrow,
    Range,
    RangeInclusive,

    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    PlusEqual,
    MinusEqual,
    StarEqual,
    SlashEqual,
    PercentEqual,
    Equal,
    EqualEqual,
    Bang,
    BangEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    AndAnd,
    OrOr,

    Count
};

struct Token {
    TokenKind kind{TokenKind::Invalid};
    SourceSpan span{};

    [[nodiscard]] std::string_view text(const SourceFile& source) const noexcept;
};

[[nodiscard]] std::string_view token_kind_name(TokenKind kind) noexcept;
[[nodiscard]] bool token_has_lexeme_value(TokenKind kind) noexcept;

} // namespace vex
