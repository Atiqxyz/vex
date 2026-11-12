#pragma once

#include "vex/source.hpp"

#include <cstddef>
#include <iosfwd>
#include <string>
#include <vector>

namespace vex {

enum class DiagnosticSeverity {
    Note,
    Warning,
    Error
};

struct Diagnostic {
    DiagnosticSeverity severity{DiagnosticSeverity::Error};
    std::string code;
    std::string message;
    SourceSpan span;
};

class DiagnosticEngine {
public:
    void report(Diagnostic diagnostic);
    void error(std::string code, std::string message, SourceSpan span);

    [[nodiscard]] const std::vector<Diagnostic>& all() const noexcept;
    [[nodiscard]] std::vector<Diagnostic> take();
    [[nodiscard]] bool has_errors() const noexcept;
    [[nodiscard]] std::size_t error_count() const noexcept;
    void clear() noexcept;

private:
    std::vector<Diagnostic> diagnostics_;
};

[[nodiscard]] const char* diagnostic_severity_name(DiagnosticSeverity severity) noexcept;
void print_diagnostics(std::ostream& output, const SourceFile& source,
                       const std::vector<Diagnostic>& diagnostics);

} // namespace vex
