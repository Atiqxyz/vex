#pragma once

#include "vex/ast.hpp"
#include "vex/diagnostic.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vex {

enum class SemanticType {
    Error,
    Unknown,
    Any,
    Void,
    Int,
    Byte,
    Float,
    Bool,
    String,
    Struct,
    Enum,
    Array,
    Range,
    Pointer
};

struct SemanticArrayType {
    SemanticType element_type{SemanticType::Error};
    // Compatibility mirror of the outermost dimension. Zero means dynamic.
    std::uint32_t length{0};
    // Nonzero only when element_type is Struct. Structure identity is nominal.
    std::uint32_t element_struct_type_id{0};
    // Nonzero only when element_type is Enum. Enum identity is nominal.
    std::uint32_t element_enum_type_id{0};
    // Ordered outermost-first. A zero entry denotes a dynamic dimension.
    std::vector<std::uint32_t> dimensions;

    SemanticArrayType() = default;
    SemanticArrayType(const SemanticType element, const std::uint32_t outer_length,
                      const std::uint32_t struct_type_id = 0,
                      const std::uint32_t enum_type_id = 0)
        : element_type(element), length(outer_length),
          element_struct_type_id(struct_type_id), element_enum_type_id(enum_type_id),
          dimensions{outer_length} {}
    SemanticArrayType(const SemanticType element,
                      std::vector<std::uint32_t> dimension_lengths,
                      const std::uint32_t struct_type_id = 0,
                      const std::uint32_t enum_type_id = 0)
        : element_type(element),
          length(dimension_lengths.empty() ? 0u : dimension_lengths.front()),
          element_struct_type_id(struct_type_id), element_enum_type_id(enum_type_id),
          dimensions(std::move(dimension_lengths)) {}

    [[nodiscard]] bool is_nested() const noexcept {
        return dimensions.size() > 1u;
    }

    [[nodiscard]] bool same_shape(const SemanticArrayType& other) const noexcept {
        return element_type == other.element_type &&
               element_struct_type_id == other.element_struct_type_id &&
               element_enum_type_id == other.element_enum_type_id &&
               dimensions == other.dimensions;
    }

    [[nodiscard]] bool has_dynamic_dimension() const noexcept {
        return std::find(dimensions.begin(), dimensions.end(), 0u) != dimensions.end();
    }

    [[nodiscard]] bool is_fully_fixed() const noexcept {
        return !dimensions.empty() && !has_dynamic_dimension();
    }

    [[nodiscard]] SemanticArrayType immediate_array_element_shape() const {
        if (dimensions.size() <= 1u) return {};
        return SemanticArrayType(
            element_type,
            std::vector<std::uint32_t>(dimensions.begin() + 1, dimensions.end()),
            element_struct_type_id, element_enum_type_id);
    }
};

[[nodiscard]] const char* semantic_type_name(SemanticType type) noexcept;

using SemanticBindingId = std::uint32_t;

struct SemanticPointerType {
    SemanticType pointee_type{SemanticType::Error};
    SemanticBindingId pointee_binding_id{0};
    bool mutable_pointee{true};
    std::uint32_t pointee_struct_type_id{0};
    std::uint32_t pointee_enum_type_id{0};
    // One-based parameter origin when this pointer is a borrowed function parameter.
    std::uint32_t pointee_parameter_index{0};
    // Number of pointer indirections before the primitive or nominal pointee.
    std::uint32_t indirection_depth{1};
};

struct SemanticBinding {
    SemanticBindingId id{0};
    std::string name;
    SemanticType type{SemanticType::Unknown};
    bool mutable_binding{false};
    SourceSpan declaration{};
    std::optional<SemanticArrayType> array_type;
    std::uint32_t struct_type_id{0};
    std::uint32_t enum_type_id{0};
    std::optional<SemanticPointerType> pointer_type;
    std::uint32_t scope_depth{0};
    bool addressable{false};
};

struct SemanticFunctionSignature {
    std::string name;
    std::vector<SemanticType> parameter_types;
    std::vector<std::optional<SemanticArrayType>> parameter_array_types;
    std::vector<std::uint32_t> parameter_struct_type_ids;
    std::vector<std::uint32_t> parameter_enum_type_ids;
    std::vector<std::optional<SemanticPointerType>> parameter_pointer_types;
    SemanticType return_type{SemanticType::Void};
    std::optional<SemanticArrayType> return_array_type;
    std::uint32_t return_struct_type_id{0};
    std::uint32_t return_enum_type_id{0};
    std::optional<SemanticPointerType> return_pointer_type;
    std::optional<std::uint32_t> return_pointer_parameter_index;
    // Direct pointer-cell stores: (destination **T parameter, source *T parameter), one-based.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> pointer_parameter_stores;
    SourceSpan declaration{};
};

struct SemanticStructField {
    std::string name;
    SemanticType type{SemanticType::Error};
    std::uint32_t struct_type_id{0};
    std::uint32_t enum_type_id{0};
    std::optional<SemanticArrayType> array_type;
    SourceSpan declaration{};
};

struct SemanticStructType {
    std::uint32_t id{0};
    std::string name;
    std::vector<SemanticStructField> fields;
    SourceSpan declaration{};
};

struct SemanticEnumPayloadField {
    SemanticType type{SemanticType::Error};
    std::uint32_t struct_type_id{0};
    std::uint32_t enum_type_id{0};
    std::optional<SemanticArrayType> array_type;
    SourceSpan declaration{};
};

struct SemanticEnumVariant {
    std::string name;
    std::vector<SemanticEnumPayloadField> fields;
    SourceSpan declaration{};
};

struct SemanticEnumType {
    std::uint32_t id{0};
    std::string name;
    std::vector<SemanticEnumVariant> variants;
    SourceSpan declaration{};
};

struct SemanticMatchArmInfo {
    bool wildcard{false};
    std::uint32_t variant_index{0};
    std::vector<SemanticBindingId> payload_bindings;
};

struct SemanticMatchInfo {
    std::uint32_t enum_type_id{0};
    std::vector<SemanticMatchArmInfo> arms;
};

// The model is tied to the AST passed to SemanticAnalyzer. Keep that AST alive
// for as long as pointers in these maps are used.
struct SemanticModel {
    std::unordered_map<const Expr*, SemanticType> expression_types;
    std::unordered_map<const Expr*, SemanticArrayType> array_expression_types;
    std::unordered_map<const Expr*, std::uint32_t> struct_expression_types;
    std::unordered_map<const Expr*, std::uint32_t> enum_expression_types;
    std::unordered_map<const Expr*, SemanticPointerType> pointer_expression_types;
    std::unordered_map<const Expr*, std::uint32_t> enum_expression_variants;
    std::unordered_map<const StructLiteralExpr*, std::vector<std::uint32_t>> struct_literal_field_indices;
    std::unordered_map<const MemberExpr*, std::uint32_t> member_field_indices;
    std::unordered_map<const NameExpr*, SemanticBindingId> resolved_names;
    std::unordered_map<const VariableDeclStmt*, SemanticBindingId> variable_bindings;
    std::unordered_map<const Parameter*, SemanticBindingId> parameter_bindings;
    std::unordered_map<const AssignmentExpr*, SemanticBindingId> assignment_targets;
    // Aggregate mutations rooted at *pointer retain the pointer expression for lowering.
    std::unordered_map<const AssignmentExpr*, const Expr*> assignment_pointer_targets;
    std::unordered_map<const ForStmt*, SemanticBindingId> for_bindings;
    std::unordered_map<const CallExpr*, std::string> resolved_calls;
    std::unordered_map<const MatchStmt*, SemanticMatchInfo> match_statements;
    std::unordered_map<const FunctionDeclStmt*, SemanticFunctionSignature> functions;
    std::unordered_map<SemanticBindingId, SemanticBinding> bindings;
    std::vector<SemanticStructType> struct_types;
    std::vector<SemanticEnumType> enum_types;
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
