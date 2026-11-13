#pragma once

#include "vex/backend.hpp"

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
class CBackend final : public Backend {
public:
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] BackendResult emit(const IRModule& module) const override;

    // Retained as the C-specific API for callers that need the generated
    // source field rather than the backend-neutral output field.
    [[nodiscard]] CBackendResult generate(const IRModule& module) const;
};

} // namespace vex
