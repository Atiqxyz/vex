#pragma once

#include "vex/ast.hpp"
#include "vex/diagnostic.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace vex {

enum class SemanticType {
    Error,
    Unknown,
    Any,
    Void,
    Int,
    Float,
    Bool,
    String
};

[[nodiscard]] const char* semantic_type_name(SemanticType type) noexcept;

using SemanticBindingId = std::uint32_t;

struct SemanticBinding {
    SemanticBindingId id{0};
    std::string name;
    SemanticType type{SemanticType::Unknown};
    bool mutable_binding{false};
    SourceSpan declaration{};
};

struct SemanticFunctionSignature {
    std::string name;
    std::vector<SemanticType> parameter_types;
    SemanticType return_type{SemanticType::Void};
    SourceSpan declaration{};
};

// The model is tied to the AST passed to SemanticAnalyzer. Keep that AST alive
// for as long as pointers in these maps are used.
struct SemanticModel {
    std::unordered_map<const Expr*, SemanticType> expression_types;
    std::unordered_map<const NameExpr*, SemanticBindingId> resolved_names;
    std::unordered_map<const VariableDeclStmt*, SemanticBindingId> variable_bindings;
    std::unordered_map<const Parameter*, SemanticBindingId> parameter_bindings;
    std::unordered_map<const AssignmentExpr*, SemanticBindingId> assignment_targets;
    std::unordered_map<const CallExpr*, std::string> resolved_calls;
    std::unordered_map<const FunctionDeclStmt*, SemanticFunctionSignature> functions;
    std::unordered_map<SemanticBindingId, SemanticBinding> bindings;
};

struct SemanticResult {
    std::vector<Diagnostic> diagnostics;
    SemanticModel model;

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
