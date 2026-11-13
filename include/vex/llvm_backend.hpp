#pragma once

#include "vex/backend.hpp"

namespace vex {

// Emits opaque-pointer LLVM IR from verified VEX IR.
class LLVMBackend final : public Backend {
public:
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] BackendResult emit(const IRModule& module) const override;
};

} // namespace vex
