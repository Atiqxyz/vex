#pragma once

#include "vex/ast.hpp"
#include "vex/source.hpp"

#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace vex {

[[nodiscard]] std::optional<SourceFile> load_source_bundle_with_imports(
    const std::vector<std::string>& paths, std::ostream& diagnostics);
void strip_resolved_imports(Program& program);

} // namespace vex
