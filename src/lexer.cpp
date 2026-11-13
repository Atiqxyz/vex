#include "vex/lexer.hpp"

#include <array>
#include <string>
#include <utility>

namespace vex {
namespace {

using Keyword = std::pair<std::string_view, TokenKind>;

constexpr std::array<Keyword, 20> keywords{{
    {"make", TokenKind::KwMake},
    {"struct", TokenKind::KwStruct},
    {"let", TokenKind::KwLet},
    {"const", TokenKind::KwConst},
    {"if", TokenKind::KwIf},
    {"else", TokenKind::KwElse},
    {"while", TokenKind::KwWhile},
    {"break", TokenKind::KwBreak},
    {"continue", TokenKind::KwContinue},
    {"repeat", TokenKind::KwRepeat},
    {"times", TokenKind::KwTimes},
    {"for", TokenKind::KwFor},
    {"in", TokenKind::KwIn},
    {"return", TokenKind::KwReturn},
    {"true", TokenKind::KwTrue},
    {"false", TokenKind::KwFalse},
    {"int", TokenKind::KwInt},
    {"float", TokenKind::KwFloat},
    {"bool", TokenKind::KwBool},
    {"string", TokenKind::KwString}
}};

[[nodiscard]] TokenKind keyword_kind(const std::string_view spelling) noexcept {
    for (const auto& [word, kind] : keywords) {
        if (word == spelling) {
            return kind;
        }
    }
    return TokenKind::Identifier;
}

} // namespace

bool LexResult::has_errors() const noexcept {
    for (const Diagnostic& diagnostic : diagnostics) {
        if (diagnostic.severity == DiagnosticSeverity::Error) {
            return true;
        }
    }
    return false;
}

Lexer::Lexer(const SourceFile& source)
    : text_(source.contents()) {}

LexResult Lexer::scan() {
    if (scanned_) {
        return LexResult{
            {},
            {{DiagnosticSeverity::Error, "LEX000",
              "a lexer instance can only be scanned once", {0, 0}}}
        };
    }
    scanned_ = true;

    while (!at_end()) {
        start_ = current_;
        scan_token();
    }

    add_token_at(TokenKind::EndOfFile, current_, current_);
    return LexResult{std::move(tokens_), diagnostics_.take()};
}

bool Lexer::at_end() const noexcept {
    return current_ >= text_.size();
}

char Lexer::advance() noexcept {
    return at_end() ? '\0' : text_[current_++];
}

bool Lexer::match(const char expected) noexcept {
    if (at_end() || text_[current_] != expected) {
        return false;
    }
    ++current_;
    return true;
}

char Lexer::peek(const std::size_t lookahead) const noexcept {
    return current_ + lookahead >= text_.size() ? '\0' : text_[current_ + lookahead];
}

bool Lexer::is_digit(const char value) noexcept {
    return value >= '0' && value <= '9';
}

bool Lexer::is_alpha(const char value) noexcept {
    return (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z') || value == '_';
}

bool Lexer::is_alphanumeric(const char value) noexcept {
    return is_alpha(value) || is_digit(value);
}

void Lexer::scan_token() {
    const char value = advance();

    switch (value) {
    case '(': add_token(TokenKind::LeftParen); break;
    case ')': add_token(TokenKind::RightParen); break;
    case '{': add_token(TokenKind::LeftBrace); break;
    case '}': add_token(TokenKind::RightBrace); break;
    case '[': add_token(TokenKind::LeftBracket); break;
    case ']': add_token(TokenKind::RightBracket); break;
    case ',': add_token(TokenKind::Comma); break;
    case ':': add_token(TokenKind::Colon); break;
    case '.':
        if (match('.')) {
            add_token(match('=') ? TokenKind::RangeInclusive : TokenKind::Range);
        } else {
            add_token(TokenKind::Dot);
        }
        break;
    case '+': add_token(match('=') ? TokenKind::PlusEqual : TokenKind::Plus); break;
    case '*': add_token(match('=') ? TokenKind::StarEqual : TokenKind::Star); break;
    case '%': add_token(match('=') ? TokenKind::PercentEqual : TokenKind::Percent); break;
    case '-':
        if (match('>')) add_token(TokenKind::Arrow);
        else add_token(match('=') ? TokenKind::MinusEqual : TokenKind::Minus);
        break;
    case '=': add_token(match('=') ? TokenKind::EqualEqual : TokenKind::Equal); break;
    case '!': add_token(match('=') ? TokenKind::BangEqual : TokenKind::Bang); break;
    case '<': add_token(match('=') ? TokenKind::LessEqual : TokenKind::Less); break;
    case '>': add_token(match('=') ? TokenKind::GreaterEqual : TokenKind::Greater); break;
    case '&':
        if (match('&')) {
            add_token(TokenKind::AndAnd);
        } else {
            report("LEX006", "expected '&' after '&'; use '&&' for logical AND",
                   start_, current_);
            add_token(TokenKind::Invalid);
        }
        break;
    case '|':
        if (match('|')) {
            add_token(TokenKind::OrOr);
        } else {
            report("LEX007", "expected '|' after '|'; use '||' for logical OR",
                   start_, current_);
            add_token(TokenKind::Invalid);
        }
        break;
    case '/':
        if (match('/')) {
            scan_line_comment();
        } else if (match('*')) {
            scan_block_comment();
        } else {
            add_token(match('=') ? TokenKind::SlashEqual : TokenKind::Slash);
        }
        break;
    case '\n':
        add_token(TokenKind::Newline);
        break;
    case '\r':
        if (peek() == '\n') {
            static_cast<void>(advance());
        }
        add_token(TokenKind::Newline);
        break;
    case ' ':
    case '\t':
        break;
    case '"':
        scan_string();
        break;
    case ';':
        report("LEX008",
               "semicolons are not supported in VEX; use a newline to separate statements",
               start_, current_);
        add_token(TokenKind::Invalid);
        break;
    default:
        if (is_digit(value)) {
            scan_number();
        } else if (is_alpha(value)) {
            scan_identifier();
        } else {
            report("LEX001", "unexpected character", start_, current_);
            add_token(TokenKind::Invalid);
        }
        break;
    }
}

void Lexer::scan_identifier() {
    while (is_alphanumeric(peek())) {
        static_cast<void>(advance());
    }

    const std::string_view spelling = text_.substr(start_, current_ - start_);
    add_token(keyword_kind(spelling));
}

void Lexer::scan_number() {
    while (is_digit(peek())) {
        static_cast<void>(advance());
    }

    bool is_float = false;
    if (peek() == '.' && is_digit(peek(1))) {
        is_float = true;
        static_cast<void>(advance());
        while (is_digit(peek())) {
            static_cast<void>(advance());
        }
    }

    if (peek() == 'e' || peek() == 'E') {
        is_float = true;
        static_cast<void>(advance());
        if (peek() == '+' || peek() == '-') {
            static_cast<void>(advance());
        }

        if (!is_digit(peek())) {
            report("LEX005", "expected a digit in the numeric exponent", start_, current_);
            add_token(TokenKind::Invalid);
            return;
        }

        while (is_digit(peek())) {
            static_cast<void>(advance());
        }
    }

    add_token(is_float ? TokenKind::Float : TokenKind::Integer);
}

void Lexer::scan_string() {
    bool valid = true;
    bool closed = false;
    bool stopped_at_newline = false;

    while (!at_end()) {
        const char value = peek();

        if (value == '\n' || value == '\r') {
            report("LEX004", "newline in string literal; use the \\n escape instead",
                   start_, current_);
            valid = false;
            stopped_at_newline = true;
            break;
        }

        if (value == '"') {
            static_cast<void>(advance());
            closed = true;
            break;
        }

        if (value == '\\') {
            const std::size_t escape_begin = current_;
            static_cast<void>(advance());
            if (at_end()) {
                report("LEX002", "incomplete escape sequence in string literal",
                       escape_begin, current_);
                valid = false;
                break;
            }

            const char escaped = advance();
            const bool supported = escaped == 'n' || escaped == 'r' ||
                escaped == 't' || escaped == '0' || escaped == '\\' ||
                escaped == '"' || escaped == '\'';
            if (!supported) {
                report("LEX002", "unsupported escape sequence in string literal",
                       escape_begin, current_);
                valid = false;
            }
            continue;
        }

        static_cast<void>(advance());
    }

    if (!closed && !stopped_at_newline) {
        report("LEX003", "unterminated string literal", start_, current_);
        valid = false;
    }

    add_token(valid ? TokenKind::String : TokenKind::Invalid);
}

void Lexer::scan_line_comment() {
    while (!at_end() && peek() != '\n' && peek() != '\r') {
        static_cast<void>(advance());
    }
}

void Lexer::scan_block_comment() {
    while (!at_end()) {
        if (peek() == '*' && peek(1) == '/') {
            static_cast<void>(advance());
            static_cast<void>(advance());
            return;
        }

        if (peek() == '\n' || peek() == '\r') {
            const std::size_t newline_begin = current_;
            const char first = advance();
            if (first == '\r' && peek() == '\n') {
                static_cast<void>(advance());
            }
            add_token_at(TokenKind::Newline, newline_begin, current_);
        } else {
            static_cast<void>(advance());
        }
    }

    report("LEX009", "unterminated block comment", start_, current_);
}

void Lexer::add_token(const TokenKind kind) {
    add_token_at(kind, start_, current_);
}

void Lexer::add_token_at(const TokenKind kind, const std::size_t begin,
                         const std::size_t end) {
    tokens_.push_back(Token{kind, SourceSpan{begin, end}});
}

void Lexer::report(std::string code, std::string message,
                   const std::size_t begin, const std::size_t end) {
    diagnostics_.error(std::move(code), std::move(message), SourceSpan{begin, end});
}

} // namespace vex
