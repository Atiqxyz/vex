#pragma once

#include "vex/ir.hpp"
#include "vex/semantic.hpp"

namespace vex {

struct IRLoweringResult {
    IRModule module;
    std::vector<Diagnostic> diagnostics;

    [[nodiscard]] bool has_errors() const noexcept;
};

class IRLowerer {
public:
    IRLowerer(const SourceFile& source, const Program& program,
              const SemanticModel& semantic_model);

    [[nodiscard]] IRLoweringResult lower();

private:
    const SourceFile& source_;
    const Program& program_;
    const SemanticModel& semantic_model_;
};

} // namespace vex
