#pragma once

#include "vex/ast.hpp"
#include "vex/diagnostic.hpp"

#include <vector>

namespace vex {

struct SemanticResult {
    std::vector<Diagnostic> diagnostics;

    [[nodiscard]] bool has_errors() const noexcept;
};

class SemanticAnalyzer {
public:
    SemanticAnalyzer(const SourceFile& source, const Program& program);

    [[nodiscard]] SemanticResult analyze();

private:
    const SourceFile& source_;
    const Program& program_;
};

} // namespace vex
