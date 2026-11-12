#pragma once

#include "vex/diagnostic.hpp"
#include "vex/token.hpp"

#include <string>
#include <vector>

namespace vex {

struct LexResult {
    std::vector<Token> tokens;
    std::vector<Diagnostic> diagnostics;
};

class Lexer {
public:
    explicit Lexer(std::string source);
    [[nodiscard]] LexResult scan();

private:
    [[nodiscard]] bool is_at_end() const noexcept;
    [[nodiscard]] char advance();
    [[nodiscard]] bool match(char expected);
    [[nodiscard]] char peek() const noexcept;
    [[nodiscard]] char peek_next() const noexcept;
    [[nodiscard]] bool is_digit(char c) const noexcept;
    [[nodiscard]] bool is_alpha(char c) const noexcept;
    [[nodiscard]] bool is_alphanumeric(char c) const noexcept;

    void scan_token();
    void scan_identifier();
    void scan_number();
    void scan_string();
    void scan_line_comment();
    void scan_block_comment();
    void add_token(TokenKind kind);
    void add_token_at(TokenKind kind, std::size_t line, std::size_t column,
                      std::size_t start, std::size_t end);
    void report(std::string message);

    std::string source_;
    std::vector<Token> tokens_;
    std::vector<Diagnostic> diagnostics_;
    std::size_t start_{0};
    std::size_t current_{0};
    std::size_t line_{1};
    std::size_t column_{1};
    std::size_t start_line_{1};
    std::size_t start_column_{1};
};

} // namespace vex
