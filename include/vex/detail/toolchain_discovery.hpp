#pragma once

#include <filesystem>

namespace vex::detail {

// Returns the Clang path expected in a portable VEX installation whose layout
// is <root>/bin/vex and <root>/toolchain/bin/clang[.exe].
[[nodiscard]] std::filesystem::path bundled_llvm_compiler_path(
    const std::filesystem::path& executable_path);

} // namespace vex::detail
