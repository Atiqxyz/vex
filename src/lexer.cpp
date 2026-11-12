#include "vex/lexer.hpp"

#include <unordered_map>
#include <utility>

namespace vex {
namespace {

const std::unordered_map<std::string, TokenKind> keywords{
    {"make", TokenKind::KwMake},
    {"let", TokenKind::KwLet},
    {"const", TokenKind::KwConst},
    {"if", TokenKind::KwIf},
    {"else", TokenKind::KwElse},
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
    {"string", TokenKind::KwString},
};

} // namespace

Lexer::Lexer(std::string source) : source_(std::move(source)) {}

LexResult Lexer::scan() {
    while (!is_at_end()) {
        start_ = current_;
        start_line_ = line_;
        start_column_ = column_;
        scan_token();
    }

    tokens_.push_back(Token{TokenKind::EndOfFile, "", line_, column_});
    return LexResult{std::move(tokens_), std::move(diagnostics_)};
}

bool Lexer::is_at_end() const noexcept {
    return current_ >= source_.size();
}

char Lexer::advance() {
    const char value = source_[current_++];
    if (value == '\n') {
        ++line_;
        column_ = 1;
    } else {
        ++column_;
    }
    return value;
}

bool Lexer::match(const char expected) {
    if (is_at_end() || source_[current_] != expected) {
        return false;
    }
    static_cast<void>(advance());
    return true;
}

char Lexer::peek() const noexcept {
    return is_at_end() ? '\0' : source_[current_];
}

char Lexer::peek_next() const noexcept {
    return current_ + 1 >= source_.size() ? '\0' : source_[current_ + 1];
}

bool Lexer::is_digit(const char c) const noexcept {
    return c >= '0' && c <= '9';
}

bool Lexer::is_alpha(const char c) const noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool Lexer::is_alphanumeric(const char c) const noexcept {
    return is_alpha(c) || is_digit(c);
}

void Lexer::scan_token() {
    const char c = advance();
    switch (c) {
    case '(': add_token(TokenKind::LeftParen); break;
    case ')': add_token(TokenKind::RightParen); break;
    case '{': add_token(TokenKind::LeftBrace); break;
    case '}': add_token(TokenKind::RightBrace); break;
    case '[': add_token(TokenKind::LeftBracket); break;
    case ']': add_token(TokenKind::RightBracket); break;
    case ',': add_token(TokenKind::Comma); break;
    case ':': add_token(TokenKind::Colon); break;
    case '+': add_token(TokenKind::Plus); break;
    case '*': add_token(TokenKind::Star); break;
    case '%': add_token(TokenKind::Percent); break;
    case '-': add_token(match('>') ? TokenKind::Arrow : TokenKind::Minus); break;
    case '=': add_token(match('=') ? TokenKind::EqualEqual : TokenKind::Equal); break;
    case '!': add_token(match('=') ? TokenKind::BangEqual : TokenKind::Bang); break;
    case '<': add_token(match('=') ? TokenKind::LessEqual : TokenKind::Less); break;
    case '>': add_token(match('=') ? TokenKind::GreaterEqual : TokenKind::Greater); break;
    case '&':
        if (match('&')) add_token(TokenKind::AndAnd);
        else report("expected '&' after '&'; use '&&' for logical AND");
        break;
    case '|':
        if (match('|')) add_token(TokenKind::OrOr);
        else report("expected '|' after '|'; use '||' for logical OR");
        break;
    case '/':
        if (match('/')) scan_line_comment();
        else if (match('*')) scan_block_comment();
        else add_token(TokenKind::Slash);
        break;
    case ' ':
    case '\r':
    case '\t':
        break;
    case '\n':
        add_token(TokenKind::Newline);
        break;
    case '"':
        scan_string();
        break;
    case ';':
        report("semicolons are not supported in VEX; use a newline to separate statements");
        break;
    default:
        if (is_digit(c)) scan_number();
        else if (is_alpha(c)) scan_identifier();
        else report("unexpected character");
        break;
    }
}

void Lexer::scan_identifier() {
    while (is_alphanumeric(peek())) {
        static_cast<void>(advance());
    }

    const std::string text = source_.substr(start_, current_ - start_);
    const auto found = keywords.find(text);
    add_token(found == keywords.end() ? TokenKind::Identifier : found->second);
}

void Lexer::scan_number() {
    while (is_digit(peek())) {
        static_cast<void>(advance());
    }

    bool is_float_literal = false;
    if (peek() == '.' && is_digit(peek_next())) {
        is_float_literal = true;
        static_cast<void>(advance());
        while (is_digit(peek())) {
            static_cast<void>(advance());
        }
    }

    if (peek() == 'e' || peek() == 'E') {
        is_float_literal = true;
        static_cast<void>(advance());
        if (peek() == '+' || peek() == '-') {
            static_cast<void>(advance());
        }
        if (!is_digit(peek())) {
            report("expected a digit in the numeric exponent");
            return;
        }
        while (is_digit(peek())) {
            static_cast<void>(advance());
        }
    }

    add_token(is_float_literal ? TokenKind::Float : TokenKind::Integer);
}

void Lexer::scan_string() {
    bool valid = true;
    bool closed = false;

    while (!is_at_end()) {
        const char c = peek();
        if (c == '\n') {
            report("newline in string literal; use the \\n escape instead");
            valid = false;
            break;
        }
        if (c == '"') {
            static_cast<void>(advance());
            closed = true;
            break;
        }
        if (c == '\\') {
            static_cast<void>(advance());
            if (is_at_end()) break;
            const char escaped = advance();
            if (escaped != 'n' && escaped != 'r' && escaped != 't' &&
                escaped != '0' && escaped != '\\' && escaped != '"' && escaped != '\'') {
                report("unsupported escape sequence in string literal");
                valid = false;
            }
            continue;
        }
        static_cast<void>(advance());
    }

    if (!closed) {
        report("unterminated string literal");
        valid = false;
    }
    if (valid) {
        add_token(TokenKind::String);
    }
}

void Lexer::scan_line_comment() {
    while (!is_at_end() && peek() != '\n') {
        static_cast<void>(advance());
    }
}

void Lexer::scan_block_comment() {
    while (!is_at_end()) {
        if (peek() == '*' && peek_next() == '/') {
            static_cast<void>(advance());
            static_cast<void>(advance());
            return;
        }
        if (peek() == '\n') {
            const std::size_t newline_line = line_;
            const std::size_t newline_column = column_;
            const std::size_t newline_start = current_;
            static_cast<void>(advance());
            add_token_at(TokenKind::Newline, newline_line, newline_column,
                         newline_start, current_);
        } else {
            static_cast<void>(advance());
        }
    }
    report("unterminated block comment");
}

void Lexer::add_token(const TokenKind kind) {
    tokens_.push_back(Token{kind, source_.substr(start_, current_ - start_),
                            start_line_, start_column_});
}

void Lexer::add_token_at(const TokenKind kind, const std::size_t line,
                         const std::size_t column, const std::size_t start,
                         const std::size_t end) {
    tokens_.push_back(Token{kind, source_.substr(start, end - start), line, column});
}

void Lexer::report(std::string message) {
    diagnostics_.push_back(Diagnostic{std::move(message), start_line_, start_column_});
}

} // namespace vex
