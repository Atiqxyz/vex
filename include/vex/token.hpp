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
    KwEnum,
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
    KwNull,
    KwInt,
    KwByte,
    KwFloat,
    KwBool,
    KwString,
    KwImport,
    KwMatch,

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
    FatArrow,
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
    ShiftLeft,
    ShiftRight,
    AndAnd,
    OrOr,
    BitAnd,
    BitOr,
    BitXor,
    BitNot,

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
