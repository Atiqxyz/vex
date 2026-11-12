#include "vex/diagnostic.hpp"

#include <algorithm>
#include <ostream>
#include <string>
#include <utility>

namespace vex {

void DiagnosticEngine::report(Diagnostic diagnostic) {
    diagnostics_.push_back(std::move(diagnostic));
}

void DiagnosticEngine::error(std::string code, std::string message, const SourceSpan span) {
    report(Diagnostic{DiagnosticSeverity::Error, std::move(code), std::move(message), span});
}

const std::vector<Diagnostic>& DiagnosticEngine::all() const noexcept {
    return diagnostics_;
}

std::vector<Diagnostic> DiagnosticEngine::take() {
    return std::move(diagnostics_);
}

bool DiagnosticEngine::has_errors() const noexcept {
    return error_count() != 0;
}

std::size_t DiagnosticEngine::error_count() const noexcept {
    return static_cast<std::size_t>(std::count_if(
        diagnostics_.begin(), diagnostics_.end(),
        [](const Diagnostic& diagnostic) {
            return diagnostic.severity == DiagnosticSeverity::Error;
        }));
}

void DiagnosticEngine::clear() noexcept {
    diagnostics_.clear();
}

const char* diagnostic_severity_name(const DiagnosticSeverity severity) noexcept {
    switch (severity) {
    case DiagnosticSeverity::Note: return "note";
    case DiagnosticSeverity::Warning: return "warning";
    case DiagnosticSeverity::Error: return "error";
    }
    return "unknown";
}

void print_diagnostics(std::ostream& output, const SourceFile& source,
                       const std::vector<Diagnostic>& diagnostics) {
    for (const Diagnostic& diagnostic : diagnostics) {
        const SourcePosition position = source.position(diagnostic.span.begin);
        const std::string& path = source.path();
        output << (path.empty() ? "<source>" : path) << ':'
               << position.line << ':' << position.column << ": "
               << diagnostic_severity_name(diagnostic.severity)
               << '[' << diagnostic.code << "]: " << diagnostic.message << '\n';

        const std::string_view line = source.line_text(position.line);
        output << "  " << position.line << " | " << line << '\n';
        output << "    | " << std::string(position.column > 0 ? position.column - 1 : 0, ' ');

        const std::size_t available = position.column <= line.size()
            ? line.size() - (position.column - 1)
            : 0;
        const std::size_t width = std::max<std::size_t>(
            1, std::min(diagnostic.span.size(), available));
        output << std::string(width, '^') << '\n';
    }
}

} // namespace vex
