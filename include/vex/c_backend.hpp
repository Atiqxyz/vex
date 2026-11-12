#pragma once

#include "vex/ir.hpp"

#include <string>
#include <vector>

namespace vex {

struct CBackendResult {
    std::string source;
    std::vector<Diagnostic> diagnostics;

    [[nodiscard]] bool has_errors() const noexcept;
};

// Emits portable C11 from a verified VEX IR module. This pass does not invoke
// a C compiler or execute the generated program.
class CBackend {
public:
    [[nodiscard]] CBackendResult generate(const IRModule& module) const;
};

} // namespace vex
