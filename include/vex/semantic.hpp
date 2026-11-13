#pragma once

#include "vex/ast.hpp"
#include "vex/diagnostic.hpp"

#include <cstdint>
#include <optional>
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
    String,
    Struct,
    Array,
    Range
};

struct SemanticArrayType {
    SemanticType element_type{SemanticType::Error};
    std::uint32_t length{0};
};

[[nodiscard]] const char* semantic_type_name(SemanticType type) noexcept;

using SemanticBindingId = std::uint32_t;

struct SemanticBinding {
    SemanticBindingId id{0};
    std::string name;
    SemanticType type{SemanticType::Unknown};
    bool mutable_binding{false};
    SourceSpan declaration{};
    std::optional<SemanticArrayType> array_type;
    std::uint32_t struct_type_id{0};
};

struct SemanticFunctionSignature {
    std::string name;
    std::vector<SemanticType> parameter_types;
    std::vector<std::optional<SemanticArrayType>> parameter_array_types;
    std::vector<std::uint32_t> parameter_struct_type_ids;
    SemanticType return_type{SemanticType::Void};
    std::optional<SemanticArrayType> return_array_type;
    std::uint32_t return_struct_type_id{0};
    SourceSpan declaration{};
};

struct SemanticStructField {
    std::string name;
    SemanticType type{SemanticType::Error};
    std::uint32_t struct_type_id{0};
    SourceSpan declaration{};
};

struct SemanticStructType {
    std::uint32_t id{0};
    std::string name;
    std::vector<SemanticStructField> fields;
    SourceSpan declaration{};
};

// The model is tied to the AST passed to SemanticAnalyzer. Keep that AST alive
// for as long as pointers in these maps are used.
struct SemanticModel {
    std::unordered_map<const Expr*, SemanticType> expression_types;
    std::unordered_map<const Expr*, SemanticArrayType> array_expression_types;
    std::unordered_map<const Expr*, std::uint32_t> struct_expression_types;
    std::unordered_map<const StructLiteralExpr*, std::vector<std::uint32_t>> struct_literal_field_indices;
    std::unordered_map<const MemberExpr*, std::uint32_t> member_field_indices;
    std::unordered_map<const NameExpr*, SemanticBindingId> resolved_names;
    std::unordered_map<const VariableDeclStmt*, SemanticBindingId> variable_bindings;
    std::unordered_map<const Parameter*, SemanticBindingId> parameter_bindings;
    std::unordered_map<const AssignmentExpr*, SemanticBindingId> assignment_targets;
    std::unordered_map<const ForStmt*, SemanticBindingId> for_bindings;
    std::unordered_map<const CallExpr*, std::string> resolved_calls;
    std::unordered_map<const FunctionDeclStmt*, SemanticFunctionSignature> functions;
    std::unordered_map<SemanticBindingId, SemanticBinding> bindings;
    std::vector<SemanticStructType> struct_types;
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
