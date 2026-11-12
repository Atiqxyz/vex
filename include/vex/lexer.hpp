#pragma once

#include "vex/diagnostic.hpp"
#include "vex/token.hpp"

#include <cstddef>
#include <string_view>
#include <vector>

namespace vex {

struct LexResult {
    std::vector<Token> tokens;
    std::vector<Diagnostic> diagnostics;

    [[nodiscard]] bool has_errors() const noexcept;
};

class Lexer {
public:
    explicit Lexer(const SourceFile& source);

    // A Lexer is a single-use scanner. The returned tokens refer to source spans,
    // so the SourceFile must outlive the LexResult.
    [[nodiscard]] LexResult scan();

private:
    [[nodiscard]] bool at_end() const noexcept;
    [[nodiscard]] char advance() noexcept;
    [[nodiscard]] bool match(char expected) noexcept;
    [[nodiscard]] char peek(std::size_t lookahead = 0) const noexcept;

    [[nodiscard]] static bool is_digit(char value) noexcept;
    [[nodiscard]] static bool is_alpha(char value) noexcept;
    [[nodiscard]] static bool is_alphanumeric(char value) noexcept;

    void scan_token();
    void scan_identifier();
    void scan_number();
    void scan_string();
    void scan_line_comment();
    void scan_block_comment();
    void add_token(TokenKind kind);
    void add_token_at(TokenKind kind, std::size_t begin, std::size_t end);
    void report(std::string code, std::string message,
                std::size_t begin, std::size_t end);

    std::string_view text_;
    std::vector<Token> tokens_;
    DiagnosticEngine diagnostics_;
    std::size_t start_{0};
    std::size_t current_{0};
    bool scanned_{false};
};

} // namespace vex
