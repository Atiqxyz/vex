#include "vex/backend.hpp"

#include "vex/c_backend.hpp"

#include <algorithm>
#include <memory>

namespace vex {

bool BackendResult::has_errors() const noexcept {
    return std::any_of(diagnostics.begin(), diagnostics.end(),
        [](const Diagnostic& diagnostic) {
            return diagnostic.severity == DiagnosticSeverity::Error;
        });
}

std::unique_ptr<Backend> create_backend(const BackendKind kind) {
    switch (kind) {
    case BackendKind::C:
        return std::make_unique<CBackend>();
    }
    return nullptr;
}

} // namespace vex
