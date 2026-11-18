#include "vex/semantic.hpp"
#include "vex/stack_policy.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>
#include <string_view>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vex {
namespace {

using Type = SemanticType;

constexpr std::string_view int64_min_magnitude = "9223372036854775808";

const Expr* unwrap_grouped_expression(const Expr* expression) noexcept {
    while (expression) {
        const auto* group = std::get_if<GroupExpr>(&expression->data);
        if (!group) break;
        expression = group->expression.get();
    }
    return expression;
}

bool is_int64_min_magnitude_literal(
    const Expr* expression, const SourceFile& source) {
    expression = unwrap_grouped_expression(expression);
    const auto* literal = expression
        ? std::get_if<LiteralExpr>(&expression->data) : nullptr;
    return literal && literal->token_kind == TokenKind::Integer &&
        source.slice(expression->span) == int64_min_magnitude;
}

const char* type_name(const Type type) noexcept {
    return semantic_type_name(type);
}

bool is_numeric(const Type type) noexcept {
    return type == Type::Int || type == Type::Float;
}

bool is_assignable(const Type destination, const Type source) noexcept {
    if (destination == Type::Error || source == Type::Error ||
        destination == Type::Unknown || source == Type::Unknown) return true;
    if (source == Type::Void) return false;
    if (destination == Type::Any) return true;
    return destination == source ||
           (destination == Type::Float && source == Type::Int);
}

bool nested_fixed_dimension_runs_fit_minimum_size(const std::vector<std::uint32_t>& dimensions) {
    std::uint64_t minimum_bytes = 1u;
    std::size_t fixed_run = 0u;
    for (const std::uint32_t dimension : dimensions) {
        if (dimension == 0u) { minimum_bytes = 1u; fixed_run = 0u; continue; }
        if (minimum_bytes > max_nested_fixed_aggregate_bytes / dimension) return false;
        minimum_bytes *= dimension;
        ++fixed_run;
        if (fixed_run >= 2u && minimum_bytes > max_nested_fixed_aggregate_bytes) return false;
    }
    return true;
}

Type common_numeric_type(const Type left, const Type right) noexcept {
    if (left == Type::Error || right == Type::Error) return Type::Error;
    if (left == Type::Unknown || right == Type::Unknown) return Type::Unknown;
    if (!is_numeric(left) || !is_numeric(right)) return Type::Error;
    return left == Type::Float || right == Type::Float ? Type::Float : Type::Int;
}

Type compound_assignment_result(const TokenKind assignment,const Type left,const Type right) noexcept {
    switch(assignment) {
    case TokenKind::PlusEqual:
        if(left==Type::String&&right==Type::String)return Type::String;
        return common_numeric_type(left,right);
    case TokenKind::MinusEqual: case TokenKind::StarEqual: case TokenKind::SlashEqual:
        return common_numeric_type(left,right);
    case TokenKind::PercentEqual:
        if(left==Type::Error||right==Type::Error)return Type::Error;
        if(left==Type::Unknown||right==Type::Unknown)return Type::Unknown;
        return left==Type::Int&&right==Type::Int?Type::Int:Type::Error;
    default:return Type::Error;
    }
}

bool compatible_array_literal_context(
    const SemanticArrayType& expected, const SemanticArrayType& actual) {
    if (expected.element_type != actual.element_type &&
        !(expected.element_type == Type::Float && actual.element_type == Type::Int)) {
        return false;
    }
    if (expected.element_type == Type::Struct &&
        expected.element_struct_type_id != actual.element_struct_type_id) {
        return false;
    }
    if (expected.element_type == Type::Enum &&
        expected.element_enum_type_id != actual.element_enum_type_id) {
        return false;
    }
    if (expected.dimensions.size() != actual.dimensions.size()) return false;
    for (std::size_t index = 0; index < expected.dimensions.size(); ++index) {
        if (expected.dimensions[index] != 0u &&
            expected.dimensions[index] != actual.dimensions[index]) return false;
    }
    return true;
}

struct Symbol {
    SemanticBindingId id{0};
    Type type{Type::Unknown};
    bool immutable{false};
    bool is_function{false};
    std::vector<Type> parameters;
    std::vector<std::optional<SemanticArrayType>> parameter_array_types;
    std::vector<std::optional<SemanticPointerType>> parameter_pointer_types;
    SourceSpan declaration{};
    std::optional<SemanticArrayType> array_type;
    std::optional<SemanticArrayType> return_array_type;
    std::uint32_t struct_type_id{0};
    std::uint32_t enum_type_id{0};
    std::optional<SemanticPointerType> pointer_type;
    std::vector<std::uint32_t> parameter_struct_type_ids;
    std::vector<std::uint32_t> parameter_enum_type_ids;
    std::uint32_t return_struct_type_id{0};
    std::uint32_t return_enum_type_id{0};
    std::optional<SemanticPointerType> return_pointer_type;
    std::optional<std::uint32_t> return_pointer_parameter_index;
};

using Scope = std::unordered_map<std::string, Symbol>;

class AnalyzerImpl {
public:
    AnalyzerImpl(const SourceFile& source, const Program& program)
        : source_(source), program_(program) {
        scopes_.emplace_back();
    }

    SemanticResult run() {
        install_builtins();
        collect_struct_type_names();
        collect_enum_type_names();
        collect_struct_types();
        collect_enum_types();
        collect_function_signatures();
        for (const StmtPtr& statement : program_.statements) {
            if (statement && !std::holds_alternative<StructDeclStmt>(statement->data) &&
                !std::holds_alternative<EnumDeclStmt>(statement->data))
                analyze_statement(*statement);
        }
        for (const Expr* null_literal : null_pointer_literals_) {
            const auto pointer = null_literal
                ? model_.pointer_expression_types.find(null_literal)
                : model_.pointer_expression_types.end();
            if (pointer == model_.pointer_expression_types.end() ||
                pointer->second.pointee_type == Type::Unknown ||
                pointer->second.pointee_type == Type::Error) {
                report("SEM073", "null requires an explicit or otherwise unambiguous typed pointer context",
                       null_literal ? null_literal->span : SourceSpan{});
            }
        }
        return SemanticResult{std::move(diagnostics_), std::move(model_)};
    }

private:
    void report(const char* code, std::string message, const SourceSpan span) {
        diagnostics_.push_back(Diagnostic{
            DiagnosticSeverity::Error, code, std::move(message), span
        });
    }

    [[nodiscard]] std::string spelling(const SourceSpan span) const {
        return std::string(source_.slice(span));
    }

    [[nodiscard]] Type annotation_type(
        const SourceSpan span, const bool allow_void,
        std::optional<SemanticArrayType>* array_shape = nullptr,
        std::uint32_t* struct_type_id = nullptr,
        std::uint32_t* enum_type_id = nullptr,
        std::optional<SemanticPointerType>* pointer_type = nullptr) {
        const std::string name = spelling(span);
        if (!name.empty() && name.front() == '*') {
            std::size_t depth = 0u;
            while (depth < name.size() && name[depth] == '*') ++depth;
            std::string pointee_name = name.substr(depth);
            const auto trim = [](std::string& value) {
                while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
                while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();
            };
            trim(pointee_name);
            Type pointee = Type::Error;
            std::uint32_t pointee_struct_type_id = 0u;
            std::uint32_t pointee_enum_type_id = 0u;
            if (depth == 0u || depth > 8u || pointee_name.empty()) {
                report("SEM070", "pointer type depth must be between one and eight", span);
                return Type::Error;
            }
            if (pointee_name == "int") pointee = Type::Int;
            else if (pointee_name == "byte") pointee = Type::Byte;
            else if (pointee_name == "float") pointee = Type::Float;
            else if (pointee_name == "bool") pointee = Type::Bool;
            else if (pointee_name == "string") pointee = Type::String;
            else if (const auto structure = struct_ids_.find(pointee_name);
                     structure != struct_ids_.end()) {
                pointee = Type::Struct;
                pointee_struct_type_id = structure->second;
            } else if (const auto enumeration = enum_ids_.find(pointee_name);
                       enumeration != enum_ids_.end()) {
                pointee = Type::Enum;
                pointee_enum_type_id = enumeration->second;
            } else {
                report("SEM070", "pointer pointee must be a supported primitive or declared nominal type", span);
                return Type::Error;
            }
            if (!pointer_type) {
                report("SEM070", "pointer parameters and return types are not supported yet", span);
                return Type::Error;
            }
            SemanticPointerType descriptor{
                pointee, 0u, true, pointee_struct_type_id, pointee_enum_type_id
            };
            descriptor.indirection_depth = static_cast<std::uint32_t>(depth);
            *pointer_type = descriptor;
            return Type::Pointer;
        }
        const std::size_t open = name.find('[');
        if (open != std::string::npos) {
            std::string element_name = name.substr(0, open);
            const auto trim = [](std::string& value) {
                const auto whitespace = [](const char character) {
                    return character == ' ' || character == '\t' ||
                           character == '\r' || character == '\n';
                };
                while (!value.empty() && whitespace(value.front())) value.erase(value.begin());
                while (!value.empty() && whitespace(value.back())) value.pop_back();
            };
            trim(element_name);

            Type element_type = Type::Error;
            std::uint32_t element_struct_type_id = 0;
            std::uint32_t element_enum_type_id = 0;
            if (element_name == "int") element_type = Type::Int;
            else if (element_name == "byte") element_type = Type::Byte;
            else if (element_name == "float") element_type = Type::Float;
            else if (element_name == "bool") element_type = Type::Bool;
            else if (element_name == "string") element_type = Type::String;
            else {
                const auto structure = struct_ids_.find(element_name);
                const auto enumeration = enum_ids_.find(element_name);
                if (structure != struct_ids_.end()) {
                    element_type = Type::Struct;
                    element_struct_type_id = structure->second;
                } else if (enumeration != enum_ids_.end()) {
                    element_type = Type::Enum;
                    element_enum_type_id = enumeration->second;
                } else {
                    report("SEM003", "array element type must be a primitive or a declared nominal type", span);
                    return Type::Error;
                }
            }

            std::vector<std::uint32_t> dimensions;
            std::size_t cursor = open;
            while (cursor < name.size()) {
                while (cursor < name.size() &&
                       (name[cursor] == ' ' || name[cursor] == '\t' ||
                        name[cursor] == '\r' || name[cursor] == '\n')) {
                    ++cursor;
                }
                if (cursor >= name.size() || name[cursor] != '[') {
                    report("SEM003", "malformed array type annotation", span);
                    return Type::Error;
                }
                const std::size_t close = name.find(']', cursor + 1u);
                if (close == std::string::npos) {
                    report("SEM003", "malformed array type annotation", span);
                    return Type::Error;
                }
                std::string length_text = name.substr(cursor + 1u, close - cursor - 1u);
                trim(length_text);
                if (length_text.empty()) {
                    dimensions.push_back(0u);
                } else {
                    std::uint32_t length = 0;
                    const auto parsed = std::from_chars(length_text.data(),
                        length_text.data() + length_text.size(), length);
                    if (parsed.ec != std::errc{} ||
                        parsed.ptr != length_text.data() + length_text.size() ||
                        length == 0u || length > 65536u) {
                        report("SEM041",
                            "fixed-array lengths must be between 1 and 65,536; use [] for a dynamic array",
                            span);
                        return Type::Error;
                    }
                    dimensions.push_back(length);
                }
                cursor = close + 1u;
            }
            if (dimensions.empty()) {
                report("SEM003", "malformed array type annotation", span);
                return Type::Error;
            }
            if (dimensions.size() > 64u) {
                report("SEM060", "array types may contain at most 64 dimensions", span);
                return Type::Error;
            }
            if (!nested_fixed_dimension_runs_fit_minimum_size(dimensions)) {
                report("SEM060",
                    "nested fixed dimension run exceeds the 256 KiB minimum object-size limit; use a dynamic dimension or a dynamic array",
                    span);
                return Type::Error;
            }
            if (array_shape) {
                *array_shape = SemanticArrayType{
                    element_type, std::move(dimensions), element_struct_type_id,
                    element_enum_type_id
                };
            }
            return Type::Array;
        }
        if (name == "int") return Type::Int;
        if (name == "byte") return Type::Byte;
        if (name == "float") return Type::Float;
        if (name == "bool") return Type::Bool;
        if (name == "string") return Type::String;
        const auto structure = struct_ids_.find(name);
        if (structure != struct_ids_.end()) {
            if (struct_type_id) *struct_type_id = structure->second;
            return Type::Struct;
        }
        const auto enumeration = enum_ids_.find(name);
        if (enumeration != enum_ids_.end()) {
            if (enum_type_id) *enum_type_id = enumeration->second;
            return Type::Enum;
        }
        if (allow_void && name == "void") return Type::Void;
        report("SEM003", "unknown or unsupported type name '" + name + "'", span);
        return Type::Error;
    }
    void install_builtins() {
        Symbol say;
        say.type = Type::Void;
        say.is_function = true;
        say.parameters.push_back(Type::Any);
        say.parameter_array_types.emplace_back(std::nullopt);
        say.declaration = {0, 0};
        scopes_.front().emplace("say", std::move(say));

        const auto add_math = [this](const char* name, const std::size_t arity) {
            Symbol function;
            function.type = Type::Float;
            function.is_function = true;
            function.parameters.assign(arity, Type::Float);
            function.parameter_array_types.resize(arity);
            function.declaration = {0, 0};
            scopes_.front().emplace(name, std::move(function));
        };
        add_math("sqrt", 1);
        add_math("sin", 1);
        add_math("cos", 1);
        add_math("pow", 2);
        add_math("abs", 1);

        const auto add_conversion = [this](const char* name, const Type result,
                                           const Type parameter) {
            Symbol function;
            function.type = result;
            function.is_function = true;
            function.parameters = {parameter};
            function.parameter_array_types = {std::nullopt};
            function.declaration = {0, 0};
            scopes_.front().emplace(name, std::move(function));
        };
        add_conversion("to_byte", Type::Byte, Type::Int);
        add_conversion("to_int", Type::Int, Type::Byte);

        const auto add_rotation = [this](const char* name) {
            Symbol function;
            function.type = Type::Any;
            function.is_function = true;
            function.parameters = {Type::Any, Type::Int};
            function.parameter_array_types = {std::nullopt, std::nullopt};
            function.declaration = {0, 0};
            scopes_.front().emplace(name, std::move(function));
        };
        add_rotation("rotate_left");
        add_rotation("rotate_right");

        Symbol length;
        length.type = Type::Int;
        length.is_function = true;
        length.parameters = {Type::Array};
        length.parameter_array_types = {SemanticArrayType{Type::Unknown, 0}};
        length.declaration = {0, 0};
        scopes_.front().emplace("length", std::move(length));

        Symbol capacity;
        capacity.type = Type::Int;
        capacity.is_function = true;
        capacity.parameters = {Type::Array};
        capacity.parameter_array_types = {SemanticArrayType{Type::Unknown, 0}};
        capacity.declaration = {0, 0};
        scopes_.front().emplace("capacity", std::move(capacity));

        Symbol join;
        join.type = Type::String;
        join.is_function = true;
        join.parameters = {Type::Array};
        join.parameter_array_types = {SemanticArrayType{Type::String, 0}};
        join.declaration = {0, 0};
        scopes_.front().emplace("join", std::move(join));

        Symbol push;
        push.type = Type::Void;
        push.is_function = true;
        push.parameters = {Type::Array, Type::Any};
        push.parameter_array_types = {
            SemanticArrayType{Type::Unknown, 0}, std::nullopt
        };
        push.declaration = {0, 0};
        scopes_.front().emplace("push", std::move(push));

        Symbol pop;
        pop.type = Type::Array;
        pop.is_function = true;
        pop.parameters = {Type::Array};
        pop.parameter_array_types = {SemanticArrayType{Type::Unknown, 0}};
        pop.declaration = {0, 0};
        scopes_.front().emplace("pop", std::move(pop));

        Symbol reserve;
        reserve.type = Type::Void;
        reserve.is_function = true;
        reserve.parameters = {Type::Array, Type::Int};
        reserve.parameter_array_types = {
            SemanticArrayType{Type::Unknown, 0}, std::nullopt
        };
        reserve.declaration = {0, 0};
        scopes_.front().emplace("reserve", std::move(reserve));

        Symbol byte_slice;
        byte_slice.type = Type::Array;
        byte_slice.is_function = true;
        byte_slice.parameters = {Type::Array, Type::Int, Type::Int};
        byte_slice.parameter_array_types = {
            SemanticArrayType{Type::Byte, 0}, std::nullopt, std::nullopt
        };
        byte_slice.return_array_type = SemanticArrayType{Type::Byte, 0};
        byte_slice.declaration = {0, 0};
        scopes_.front().emplace("byte_slice", std::move(byte_slice));

        Symbol read_file;
        read_file.type = Type::Int;
        read_file.is_function = true;
        read_file.parameters = {Type::String, Type::Array};
        read_file.parameter_array_types = {
            std::nullopt, SemanticArrayType{Type::Byte, 0}
        };
        read_file.declaration = {0, 0};
        scopes_.front().emplace("read_file", std::move(read_file));

        Symbol write_file;
        write_file.type = Type::Int;
        write_file.is_function = true;
        write_file.parameters = {Type::String, Type::Array};
        write_file.parameter_array_types = {
            std::nullopt, SemanticArrayType{Type::Byte, 0}
        };
        write_file.declaration = {0, 0};
        scopes_.front().emplace("write_file", std::move(write_file));

        for (const char* name : {"read_u16_le", "read_u16_be",
                                 "read_u32_le", "read_u32_be"}) {
            Symbol reader;
            reader.type = Type::Int;
            reader.is_function = true;
            reader.parameters = {Type::Array, Type::Int};
            reader.parameter_array_types = {
                SemanticArrayType{Type::Byte, 0}, std::nullopt
            };
            reader.declaration = {0, 0};
            scopes_.front().emplace(name, std::move(reader));
        }
        for (const char* name : {"write_u16_le", "write_u16_be",
                                 "write_u32_le", "write_u32_be"}) {
            Symbol writer;
            writer.type = Type::Void;
            writer.is_function = true;
            writer.parameters = {Type::Array, Type::Int, Type::Int};
            writer.parameter_array_types = {
                SemanticArrayType{Type::Byte, 0}, std::nullopt, std::nullopt
            };
            writer.declaration = {0, 0};
            scopes_.front().emplace(name, std::move(writer));
        }
    }

    void collect_struct_type_names() {
        for (const StmtPtr& statement : program_.statements) {
            if (!statement) continue;
            const auto* structure = std::get_if<StructDeclStmt>(&statement->data);
            if (!structure) continue;
            const std::string name = spelling(structure->name);
            if (name == "int" || name == "byte" || name == "float" || name == "bool" ||
                name == "string" || name == "void") {
                report("SEM002", "structure name '" + name + "' conflicts with a built-in type",
                       structure->name);
                continue;
            }
            if (struct_ids_.find(name) != struct_ids_.end() ||
                scopes_.front().find(name) != scopes_.front().end()) {
                report("SEM002", "duplicate global declaration of '" + name + "'",
                       structure->name);
                continue;
            }
            const std::uint32_t id = static_cast<std::uint32_t>(model_.struct_types.size() + 1u);
            struct_ids_.emplace(name, id);
            model_.struct_types.push_back(SemanticStructType{id, name, {}, structure->name});
            struct_declarations_.push_back(structure);
        }
    }

    void collect_struct_types() {
        for (const StructDeclStmt* declaration : struct_declarations_) {
            const std::string name = spelling(declaration->name);
            const auto found = struct_ids_.find(name);
            if (found == struct_ids_.end()) continue;
            SemanticStructType& structure = model_.struct_types[found->second - 1u];
            if (declaration->fields.empty())
                report("SEM048", "structures must declare at least one field", declaration->name);
            std::unordered_map<std::string, bool> field_names;
            for (const StructFieldDecl& field : declaration->fields) {
                const std::string field_name = spelling(field.name);
                if (!field_names.emplace(field_name, true).second) {
                    report("SEM049", "duplicate field '" + field_name +
                           "' in structure '" + name + "'", field.name);
                    continue;
                }
                std::uint32_t field_struct_id = 0;
                std::uint32_t field_enum_id = 0;
                std::optional<SemanticArrayType> field_array_type;
                const Type field_type = field.type_annotation
                    ? annotation_type(*field.type_annotation, false,
                                      &field_array_type, &field_struct_id,
                                      &field_enum_id)
                    : Type::Error;
                if (!field.type_annotation || field_type == Type::Error ||
                    field_type == Type::Void || field_type == Type::Unknown ||
                    field_type == Type::Any ||
                    (field_type == Type::Array && !field_array_type)) {
                    report("SEM048", "structure fields require a primitive or a declared nominal type, optionally in an array",
                           field.type_annotation.value_or(field.name));
                    continue;
                }
                const std::uint32_t contained_struct_id =
                    field_type == Type::Array && field_array_type
                        ? field_array_type->element_struct_type_id : field_struct_id;
                // A fixed array embeds its elements and therefore participates in
                // by-value recursion. A dynamic array is a pointer-backed edge and
                // may safely refer to the containing structure.
                const bool contains_structure =
                    field_type == Type::Struct ||
                    (field_type == Type::Array && field_array_type &&
                     field_array_type->is_fully_fixed() &&
                     field_array_type->element_type == Type::Struct);
                const bool forward_dynamic_structure =
                    field_type == Type::Array && field_array_type &&
                    field_array_type->has_dynamic_dimension() &&
                    field_array_type->element_type == Type::Struct &&
                    contained_struct_id > structure.id;
                if (forward_dynamic_structure) {
                    report("SEM050", "a dynamic structure-array field may reference its own structure or an earlier declaration, not a later declaration",
                           field.type_annotation.value_or(field.name));
                    continue;
                }
                if (contains_structure && contained_struct_id >= structure.id) {
                    report("SEM050", "a structure field may only contain a previously declared structure; recursive by-value fields are not supported",
                           field.type_annotation.value_or(field.name));
                    continue;
                }
                structure.fields.push_back(SemanticStructField{
                    field_name, field_type, field_struct_id, field_enum_id,
                    field_array_type, field.name
                });
            }
        }
    }

    void collect_enum_type_names() {
        for (const StmtPtr& statement : program_.statements) {
            if (!statement) continue;
            const auto* enumeration = std::get_if<EnumDeclStmt>(&statement->data);
            if (!enumeration) continue;
            const std::string name = spelling(enumeration->name);
            if (name == "int" || name == "byte" || name == "float" || name == "bool" ||
                name == "string" || name == "void") {
                report("SEM061", "enum name '" + name + "' conflicts with a built-in type",
                       enumeration->name);
                continue;
            }
            if (struct_ids_.find(name) != struct_ids_.end() ||
                enum_ids_.find(name) != enum_ids_.end() ||
                scopes_.front().find(name) != scopes_.front().end()) {
                report("SEM002", "duplicate global declaration of '" + name + "'",
                       enumeration->name);
                continue;
            }
            const std::uint32_t id =
                static_cast<std::uint32_t>(model_.enum_types.size() + 1u);
            enum_ids_.emplace(name, id);
            model_.enum_types.push_back(SemanticEnumType{id, name, {}, enumeration->name});
            enum_declarations_.push_back(enumeration);
        }
    }

    void collect_enum_types() {
        for (const EnumDeclStmt* declaration : enum_declarations_) {
            const std::string name = spelling(declaration->name);
            const auto found = enum_ids_.find(name);
            if (found == enum_ids_.end()) continue;
            SemanticEnumType& enumeration = model_.enum_types[found->second - 1u];
            if (declaration->variants.empty()) {
                report("SEM062", "enums must declare at least one variant", declaration->name);
                continue;
            }
            std::unordered_map<std::string, bool> variant_names;
            for (const EnumVariantDecl& variant : declaration->variants) {
                const std::string variant_name = spelling(variant.name);
                if (!variant_names.emplace(variant_name, true).second) {
                    report("SEM063", "duplicate variant '" + variant_name +
                           "' in enum '" + name + "'", variant.name);
                    continue;
                }
                SemanticEnumVariant semantic_variant{variant_name, {}, variant.name};
                for (const SourceSpan payload_span : variant.payload_types) {
                    std::optional<SemanticArrayType> array_type;
                    std::uint32_t payload_struct_id = 0u;
                    std::uint32_t payload_enum_id = 0u;
                    const Type payload_type = annotation_type(
                        payload_span, false, &array_type, &payload_struct_id,
                        &payload_enum_id);
                    if (payload_type == Type::Error || payload_type == Type::Void ||
                        payload_type == Type::Unknown || payload_type == Type::Any ||
                        (payload_type == Type::Array && !array_type)) {
                        report("SEM064", "enum payload fields require a primitive or a declared nominal type, optionally in an array",
                               payload_span);
                        continue;
                    }
                    semantic_variant.fields.push_back(SemanticEnumPayloadField{
                        payload_type, payload_struct_id, payload_enum_id,
                        std::move(array_type), payload_span
                    });
                }
                enumeration.variants.push_back(std::move(semantic_variant));
            }
        }
    }

    [[nodiscard]] const SemanticStructField* find_struct_field(
        const std::uint32_t structure_id, const std::string& field_name,
        std::uint32_t* index = nullptr) const {
        if (structure_id == 0 || structure_id > model_.struct_types.size())
            return nullptr;
        const auto& fields = model_.struct_types[structure_id - 1u].fields;
        for (std::size_t position = 0; position < fields.size(); ++position) {
            if (fields[position].name == field_name) {
                if (index) *index = static_cast<std::uint32_t>(position);
                return &fields[position];
            }
        }
        return nullptr;
    }

    [[nodiscard]] const NameExpr* member_root_name(const Expr* expression) const {
        while (expression) {
            if (const auto* name = std::get_if<NameExpr>(&expression->data))
                return name;
            if (const auto* member = std::get_if<MemberExpr>(&expression->data)) {
                expression = member->object.get();
                continue;
            }
            if (const auto* index = std::get_if<IndexExpr>(&expression->data)) {
                expression = index->object.get();
                continue;
            }
            if (const auto* group = std::get_if<GroupExpr>(&expression->data)) {
                expression = group->expression.get();
                continue;
            }
            return nullptr;
        }
        return nullptr;
    }

    [[nodiscard]] bool is_null_pointer_literal(const Expr* expression) const {
        const auto* literal = expression
            ? std::get_if<LiteralExpr>(&expression->data) : nullptr;
        return literal && literal->token_kind == TokenKind::KwNull;
    }

    [[nodiscard]] const UnaryExpr* member_root_dereference(const Expr* expression) const {
        while (expression) {
            if (const auto* member = std::get_if<MemberExpr>(&expression->data)) {
                expression = member->object.get();
                continue;
            }
            if (const auto* index = std::get_if<IndexExpr>(&expression->data)) {
                expression = index->object.get();
                continue;
            }
            if (const auto* group = std::get_if<GroupExpr>(&expression->data)) {
                expression = group->expression.get();
                continue;
            }
            const auto* unary = std::get_if<UnaryExpr>(&expression->data);
            return unary && unary->op == TokenKind::Star ? unary : nullptr;
        }
        return nullptr;
    }

    [[nodiscard]] std::optional<std::uint32_t> direct_pointer_return_parameter(
        const FunctionDeclStmt& function) const {
        std::optional<std::uint32_t> origin;
        bool saw_return = false;
        bool valid = true;
        const auto visit = [&](const auto& self, const Statement* statement) -> void {
            if (!statement || !valid) return;
            if (const auto* returned = std::get_if<ReturnStmt>(&statement->data)) {
                if (!returned->value) { valid = false; return; }
                if (is_null_pointer_literal(returned->value.get())) {
                    saw_return = true;
                    return;
                }
                const auto* name = std::get_if<NameExpr>(&returned->value->data);
                if (!name) { valid = false; return; }
                const std::string returned_name = spelling(name->name);
                for (std::size_t index = 0; index < function.parameters.size(); ++index) {
                    const Parameter& parameter = function.parameters[index];
                    if (spelling(parameter.name) != returned_name ||
                        !parameter.type_annotation) continue;
                    const std::string annotation = spelling(*parameter.type_annotation);
                    if (annotation.empty() || annotation.front() != '*') {
                        valid = false;
                        return;
                    }
                    const std::uint32_t candidate = static_cast<std::uint32_t>(index + 1u);
                    if (origin && *origin != candidate) valid = false;
                    else origin = candidate;
                    saw_return = true;
                    return;
                }
                valid = false;
                return;
            }
            if (const auto* block = std::get_if<BlockStmt>(&statement->data)) {
                for (const StmtPtr& child : block->statements) self(self, child.get());
            } else if (const auto* conditional = std::get_if<IfStmt>(&statement->data)) {
                self(self, conditional->then_branch.get());
                self(self, conditional->else_branch.get());
            } else if (const auto* loop = std::get_if<WhileStmt>(&statement->data)) {
                self(self, loop->body.get());
            } else if (const auto* loop = std::get_if<RepeatStmt>(&statement->data)) {
                self(self, loop->body.get());
            } else if (const auto* loop = std::get_if<ForStmt>(&statement->data)) {
                self(self, loop->body.get());
            } else if (const auto* matched = std::get_if<MatchStmt>(&statement->data)) {
                for (const MatchArm& arm : matched->arms) self(self, arm.body.get());
            }
        };
        visit(visit, function.body.get());
        return valid && saw_return
            ? std::optional<std::uint32_t>{origin.value_or(0u)}
            : std::nullopt;
    }

    void collect_function_signatures() {
        for (const StmtPtr& statement : program_.statements) {
            if (!statement) continue;
            const auto* function =
                std::get_if<FunctionDeclStmt>(&statement->data);
            if (!function) continue;

            Symbol signature;
            signature.id = next_binding_id_++;
            signature.is_function = true;
            signature.declaration = function->name;
            signature.type = function->return_type
                ? annotation_type(*function->return_type, true,
                                  &signature.return_array_type,
                                  &signature.return_struct_type_id,
                                  &signature.return_enum_type_id,
                                  &signature.return_pointer_type)
                : Type::Void;
            if (signature.type == Type::Pointer)
                signature.return_pointer_parameter_index =
                    direct_pointer_return_parameter(*function);

            for (const Parameter& parameter : function->parameters) {
                Type parameter_type = Type::Unknown;
                std::optional<SemanticArrayType> parameter_array_type;
                std::uint32_t parameter_struct_type_id = 0;
                std::uint32_t parameter_enum_type_id = 0;
                std::optional<SemanticPointerType> parameter_pointer_type;
                if (parameter.type_annotation) {
                    parameter_type = annotation_type(
                        *parameter.type_annotation, false, &parameter_array_type,
                        &parameter_struct_type_id, &parameter_enum_type_id,
                        &parameter_pointer_type);
                } else {
                    report("SEM004",
                           "function parameters require an explicit type annotation",
                           parameter.name);
                }
                signature.parameters.push_back(parameter_type);
                signature.parameter_array_types.push_back(parameter_array_type);
                signature.parameter_struct_type_ids.push_back(parameter_struct_type_id);
                signature.parameter_enum_type_ids.push_back(parameter_enum_type_id);
                signature.parameter_pointer_types.push_back(parameter_pointer_type);
            }

            const std::string name = spelling(function->name);
            const auto existing = scopes_.front().find(name);
            if (struct_ids_.find(name) != struct_ids_.end() ||
                enum_ids_.find(name) != enum_ids_.end() ||
                existing != scopes_.front().end()) {
                report("SEM002", "duplicate global declaration of '" + name + "'",
                       function->name);
            } else {
                scopes_.front().emplace(name, signature);
            }
            SemanticFunctionSignature public_signature;
            public_signature.name = name;
            public_signature.parameter_types = signature.parameters;
            public_signature.parameter_array_types = signature.parameter_array_types;
            public_signature.return_type = signature.type;
            public_signature.return_array_type = signature.return_array_type;
            public_signature.parameter_struct_type_ids = signature.parameter_struct_type_ids;
            public_signature.parameter_enum_type_ids = signature.parameter_enum_type_ids;
            public_signature.parameter_pointer_types = signature.parameter_pointer_types;
            public_signature.return_struct_type_id = signature.return_struct_type_id;
            public_signature.return_enum_type_id = signature.return_enum_type_id;
            public_signature.return_pointer_type = signature.return_pointer_type;
            public_signature.return_pointer_parameter_index =
                signature.return_pointer_parameter_index;
            public_signature.declaration = function->name;
            model_.functions.emplace(function, std::move(public_signature));
            signatures_.emplace(function, std::move(signature));
        }
    }

    [[nodiscard]] Symbol* lookup_binding_symbol(const SemanticBindingId id) {
        for (std::size_t index = scopes_.size(); index > 0; --index) {
            for (auto& entry : scopes_[index - 1]) {
                if (entry.second.id == id) return &entry.second;
            }
        }
        return nullptr;
    }

    [[nodiscard]] Symbol* lookup(const std::string& name) {
        for (std::size_t index = scopes_.size(); index > 0; --index) {
            Scope& scope = scopes_[index - 1];
            const auto found = scope.find(name);
            if (found == scope.end()) continue;

            // File-scope values belong to implicit top-level execution. They
            // are not globals that separately declared functions can capture.
            if (inside_function_ && index == 1 && !found->second.is_function) {
                return nullptr;
            }
            return &found->second;
        }
        return nullptr;
    }

    [[nodiscard]] bool define(const std::string& name, Symbol symbol,
                              const SourceSpan declaration,
                              SemanticBindingId* assigned_id = nullptr) {
        Scope& current = scopes_.back();
        if (current.find(name) != current.end()) {
            report("SEM002", "duplicate declaration of '" + name +
                   "' in the same scope", declaration);
            return false;
        }
        if (symbol.id == 0) symbol.id = next_binding_id_++;
        if (assigned_id) *assigned_id = symbol.id;
        model_.bindings.emplace(symbol.id, SemanticBinding{
            symbol.id, name, symbol.type, !symbol.immutable && !symbol.is_function,
            declaration, symbol.array_type, symbol.struct_type_id, symbol.enum_type_id,
            symbol.pointer_type, static_cast<std::uint32_t>(scopes_.size()), false
        });
        current.emplace(name, std::move(symbol));
        return true;
    }

    [[nodiscard]] Type analyze_expression(const Expr& expression) {
        const Type result = analyze_expression_impl(expression);
        model_.expression_types[&expression] = result;
        return result;
    }

    [[nodiscard]] Type analyze_expression_impl(const Expr& expression) {
        if (const auto* literal = std::get_if<LiteralExpr>(&expression.data)) {
            switch (literal->token_kind) {
            case TokenKind::Integer: {
                const std::string_view text = source_.slice(expression.span);
                std::int64_t value = 0;
                const auto parsed = std::from_chars(
                    text.data(), text.data() + text.size(), value);
                if (parsed.ec != std::errc{} ||
                    parsed.ptr != text.data() + text.size()) {
                    report("SEM074",
                        "integer literal is outside the supported signed 64-bit range",
                        expression.span);
                    return Type::Error;
                }
                return Type::Int;
            }
            case TokenKind::Float: return Type::Float;
            case TokenKind::String: return Type::String;
            case TokenKind::KwTrue:
            case TokenKind::KwFalse: return Type::Bool;
            case TokenKind::KwNull:
                if (std::find(null_pointer_literals_.begin(), null_pointer_literals_.end(),
                              &expression) == null_pointer_literals_.end())
                    null_pointer_literals_.push_back(&expression);
                if (model_.pointer_expression_types.find(&expression) ==
                    model_.pointer_expression_types.end()) {
                    model_.pointer_expression_types[&expression] =
                        SemanticPointerType{Type::Unknown, 0u, true};
                }
                return Type::Pointer;
            default: return Type::Error;
            }
        }

        if (const auto* name = std::get_if<NameExpr>(&expression.data)) {
            const std::string identifier = spelling(name->name);
            Symbol* symbol = lookup(identifier);
            if (!symbol) {
                report("SEM001", "undefined identifier '" + identifier + "'",
                       name->name);
                return Type::Error;
            }
            if (symbol->is_function) {
                report("SEM010",
                       "function names can only be used as function call targets",
                       name->name);
                return Type::Error;
            }
            if (symbol->id != 0) model_.resolved_names[name] = symbol->id;
            if (symbol->array_type)
                model_.array_expression_types[&expression] = *symbol->array_type;
            if (symbol->type == Type::Struct)
                model_.struct_expression_types[&expression] = symbol->struct_type_id;
            if (symbol->type == Type::Enum)
                model_.enum_expression_types[&expression] = symbol->enum_type_id;
            if (symbol->pointer_type)
                model_.pointer_expression_types[&expression] = *symbol->pointer_type;
            return symbol->type;
        }

        if (const auto* group = std::get_if<GroupExpr>(&expression.data)) {
            if (!group->expression) return Type::Error;
            const Type grouped_type = analyze_expression(*group->expression);
            const auto shape = model_.array_expression_types.find(group->expression.get());
            if (shape != model_.array_expression_types.end())
                model_.array_expression_types[&expression] = shape->second;
            const auto structure = model_.struct_expression_types.find(group->expression.get());
            if (structure != model_.struct_expression_types.end())
                model_.struct_expression_types[&expression] = structure->second;
            const auto enumeration = model_.enum_expression_types.find(group->expression.get());
            if (enumeration != model_.enum_expression_types.end())
                model_.enum_expression_types[&expression] = enumeration->second;
            const auto pointer = model_.pointer_expression_types.find(group->expression.get());
            if (pointer != model_.pointer_expression_types.end())
                model_.pointer_expression_types[&expression] = pointer->second;
            return grouped_type;
        }

        if (const auto* array = std::get_if<ArrayLiteralExpr>(&expression.data)) {
            const auto contextual_shape = model_.array_expression_types.find(&expression);
            const bool has_context =
                contextual_shape != model_.array_expression_types.end() &&
                contextual_shape->second.element_type != Type::Unknown &&
                !contextual_shape->second.dimensions.empty();
            const SemanticArrayType expected = has_context
                ? contextual_shape->second : SemanticArrayType{};

            if (has_context && expected.dimensions.size() > 1u) {
                const SemanticArrayType nested_expected =
                    expected.immediate_array_element_shape();
                for (const ExprPtr& element : array->elements) {
                    if (element && std::holds_alternative<ArrayLiteralExpr>(element->data))
                        model_.array_expression_types[element.get()] = nested_expected;
                }
            }

            if (array->elements.empty()) {
                if (has_context && expected.dimensions.front() == 0u)
                    return Type::Array;
                report("SEM041",
                       "empty array literals require a contextual dynamic-array type such as int[]",
                       expression.span);
                return Type::Error;
            }
            if (array->elements.size() > 65536u) {
                report("SEM041", "array literals are limited to 65,536 elements per dimension",
                       expression.span);
                return Type::Error;
            }

            bool saw_nested_elements = false;
            bool saw_scalar_elements = false;
            std::optional<SemanticArrayType> nested_element_shape;
            Type element_type = Type::Unknown;
            std::uint32_t element_struct_type_id = 0;
            std::uint32_t element_enum_type_id = 0;

            for (const ExprPtr& element : array->elements) {
                const Type actual = element ? analyze_expression(*element) : Type::Error;
                if (actual == Type::Error || actual == Type::Void || actual == Type::Any) {
                    report("SEM041",
                           "array elements must use one supported primitive, structure, or nested-array type",
                           element ? element->span : expression.span);
                    return Type::Error;
                }

                if (actual == Type::Array) {
                    saw_nested_elements = true;
                    if (saw_scalar_elements || !element) {
                        report("SEM041", "an array cannot mix nested-array and scalar elements",
                               element ? element->span : expression.span);
                        return Type::Error;
                    }
                    const auto nested_shape = model_.array_expression_types.find(element.get());
                    if (nested_shape == model_.array_expression_types.end()) {
                        report("SEM041", "nested array element is missing its complete dimension shape",
                               element->span);
                        return Type::Error;
                    }
                    if (nested_element_shape &&
                        !nested_element_shape->same_shape(nested_shape->second)) {
                        report("SEM041",
                               "nested array elements must have the same element type and remaining dimensions",
                               element->span);
                        return Type::Error;
                    }
                    if (has_context && expected.dimensions.size() > 1u &&
                        !std::holds_alternative<ArrayLiteralExpr>(element->data) &&
                        !nested_shape->second.same_shape(
                            expected.immediate_array_element_shape())) {
                        report("SEM041",
                               "a nested array value must match the declared immediate element shape",
                               element->span);
                        return Type::Error;
                    }
                    if (!nested_element_shape) nested_element_shape = nested_shape->second;
                    continue;
                }

                saw_scalar_elements = true;
                if (saw_nested_elements) {
                    report("SEM041", "an array cannot mix scalar and nested-array elements",
                           element ? element->span : expression.span);
                    return Type::Error;
                }
                std::uint32_t actual_struct_type_id = 0;
                std::uint32_t actual_enum_type_id = 0;
                if (actual == Type::Struct && element) {
                    const auto found = model_.struct_expression_types.find(element.get());
                    if (found != model_.struct_expression_types.end())
                        actual_struct_type_id = found->second;
                }
                if (actual == Type::Enum && element) {
                    const auto found = model_.enum_expression_types.find(element.get());
                    if (found != model_.enum_expression_types.end())
                        actual_enum_type_id = found->second;
                }
                if (element_type == Type::Unknown) {
                    element_type = actual;
                    element_struct_type_id = actual_struct_type_id;
                    element_enum_type_id = actual_enum_type_id;
                } else if (element_type == Type::Struct || actual == Type::Struct) {
                    if (element_type != actual ||
                        element_struct_type_id != actual_struct_type_id) {
                        report("SEM041", "all structure array elements must have the same nominal type",
                               element ? element->span : expression.span);
                        return Type::Error;
                    }
                } else if (element_type == Type::Enum || actual == Type::Enum) {
                    if (element_type != actual ||
                        element_enum_type_id != actual_enum_type_id) {
                        report("SEM041", "all enum array elements must have the same nominal type",
                               element ? element->span : expression.span);
                        return Type::Error;
                    }
                } else if (is_numeric(element_type) && is_numeric(actual)) {
                    if (element_type == Type::Float || actual == Type::Float)
                        element_type = Type::Float;
                } else if (element_type != actual) {
                    report("SEM041", "all array elements must have compatible types",
                           element ? element->span : expression.span);
                    return Type::Error;
                }
            }

            SemanticArrayType inferred_shape;
            if (saw_nested_elements) {
                if (!nested_element_shape) return Type::Error;
                std::vector<std::uint32_t> dimensions;
                dimensions.reserve(nested_element_shape->dimensions.size() + 1u);
                dimensions.push_back(static_cast<std::uint32_t>(array->elements.size()));
                dimensions.insert(dimensions.end(),
                                  nested_element_shape->dimensions.begin(),
                                  nested_element_shape->dimensions.end());
                inferred_shape = SemanticArrayType{
                    nested_element_shape->element_type, std::move(dimensions),
                    nested_element_shape->element_struct_type_id,
                    nested_element_shape->element_enum_type_id};
            } else {
                inferred_shape = SemanticArrayType{
                    element_type, static_cast<std::uint32_t>(array->elements.size()),
                    element_struct_type_id, element_enum_type_id};
            }

            if (has_context) {
                const bool context_is_nested = expected.dimensions.size() > 1u;
                const bool element_kind_matches = context_is_nested == saw_nested_elements;
                const bool compatible_shape =
                    element_kind_matches &&
                    compatible_array_literal_context(expected, inferred_shape);
                model_.array_expression_types[&expression] =
                    compatible_shape ? expected : inferred_shape;
            } else {
                model_.array_expression_types[&expression] = inferred_shape;
            }

            const SemanticArrayType& result_shape =
                model_.array_expression_types.at(&expression);
            if (!nested_fixed_dimension_runs_fit_minimum_size(result_shape.dimensions)) {
                report("SEM060",
                       "nested fixed array literal exceeds the 256 KiB minimum object-size limit",
                       expression.span);
                return Type::Error;
            }
            return Type::Array;
        }

        if (const auto* literal = std::get_if<StructLiteralExpr>(&expression.data)) {
            const std::string type_name = spelling(literal->type_name);
            const auto structure_id = struct_ids_.find(type_name);
            if (structure_id == struct_ids_.end()) {
                report("SEM001", "undefined structure type '" + type_name + "'",
                       literal->type_name);
                for (const StructLiteralField& field : literal->fields)
                    if (field.value) static_cast<void>(analyze_expression(*field.value));
                return Type::Error;
            }
            const SemanticStructType& structure = model_.struct_types[structure_id->second - 1u];
            std::vector<bool> seen(structure.fields.size(), false);
            std::vector<std::uint32_t> field_indices;
            field_indices.reserve(literal->fields.size());
            for (const StructLiteralField& field : literal->fields) {
                std::uint32_t field_index = 0;
                const auto* expected = find_struct_field(
                    structure_id->second, spelling(field.name), &field_index);
                if (!expected) {
                    report("SEM051", "structure '" + type_name +
                           "' has no field named '" + spelling(field.name) + "'",
                           field.name);
                    if (field.value) static_cast<void>(analyze_expression(*field.value));
                    continue;
                }
                if (seen[field_index]) {
                    report("SEM049", "field '" + spelling(field.name) +
                           "' is initialized more than once", field.name);
                    if (field.value) static_cast<void>(analyze_expression(*field.value));
                    continue;
                }
                seen[field_index] = true;
                field_indices.push_back(field_index);
                if (field.value && expected->type == Type::Array && expected->array_type &&
                    std::holds_alternative<ArrayLiteralExpr>(field.value->data)) {
                    model_.array_expression_types[field.value.get()] = *expected->array_type;
                }
                const Type actual = field.value
                    ? analyze_expression(*field.value) : Type::Error;
                const std::uint32_t actual_struct = field.value &&
                    model_.struct_expression_types.find(field.value.get()) !=
                        model_.struct_expression_types.end()
                    ? model_.struct_expression_types.at(field.value.get()) : 0u;
                const std::uint32_t actual_enum = field.value &&
                    model_.enum_expression_types.find(field.value.get()) !=
                        model_.enum_expression_types.end()
                    ? model_.enum_expression_types.at(field.value.get()) : 0u;
                const auto actual_array = field.value
                    ? model_.array_expression_types.find(field.value.get())
                    : model_.array_expression_types.end();
                const bool array_shape_matches = expected->type != Type::Array ||
                    (expected->array_type && actual_array != model_.array_expression_types.end() &&
                     expected->array_type->same_shape(actual_array->second));
                if (!is_assignable(expected->type, actual) ||
                    (expected->type == Type::Struct && expected->struct_type_id != actual_struct) ||
                    (expected->type == Type::Enum && expected->enum_type_id != actual_enum) ||
                    !array_shape_matches) {
                    report("SEM052", "initializer for field '" + expected->name +
                           "' has an incompatible type or array shape",
                           field.value ? field.value->span : field.name);
                }
            }
            for (std::size_t index = 0; index < seen.size(); ++index) {
                if (!seen[index])
                    report("SEM053", "missing initializer for field '" +
                           structure.fields[index].name + "'", expression.span);
            }
            model_.struct_expression_types[&expression] = structure_id->second;
            model_.struct_literal_field_indices[literal] = std::move(field_indices);
            return Type::Struct;
        }

        if (const auto* member = std::get_if<MemberExpr>(&expression.data)) {
            const auto* enum_name = member->object
                ? std::get_if<NameExpr>(&member->object->data) : nullptr;
            if (enum_name) {
                const auto enum_id = enum_ids_.find(spelling(enum_name->name));
                if (enum_id != enum_ids_.end()) {
                    const SemanticEnumType& enumeration =
                        model_.enum_types[enum_id->second - 1u];
                    const std::string variant_name = spelling(member->member);
                    const auto variant = std::find_if(
                        enumeration.variants.begin(), enumeration.variants.end(),
                        [&](const SemanticEnumVariant& candidate) {
                            return candidate.name == variant_name;
                        });
                    if (variant == enumeration.variants.end()) {
                        report("SEM066", "enum '" + enumeration.name +
                               "' has no variant named '" + variant_name + "'",
                               member->member);
                        return Type::Error;
                    }
                    const std::uint32_t variant_index =
                        static_cast<std::uint32_t>(variant - enumeration.variants.begin());
                    if (!variant->fields.empty()) {
                        report("SEM065", "enum variant '" + enumeration.name + "." +
                               variant_name + "' requires payload constructor arguments",
                               expression.span);
                        return Type::Error;
                    }
                    model_.enum_expression_types[&expression] = enumeration.id;
                    model_.enum_expression_variants[&expression] = variant_index;
                    return Type::Enum;
                }
            }
            const Type object_type = member->object
                ? analyze_expression(*member->object) : Type::Error;
            if (object_type != Type::Struct) {
                if (object_type != Type::Error && object_type != Type::Unknown)
                    report("SEM054", "field access requires a structure value",
                           expression.span);
                return Type::Error;
            }
            const auto object_structure = member->object
                ? model_.struct_expression_types.find(member->object.get())
                : model_.struct_expression_types.end();
            if (object_structure == model_.struct_expression_types.end()) {
                report("SEM054", "structure type information is unavailable for this field access",
                       expression.span);
                return Type::Error;
            }
            std::uint32_t field_index = 0;
            const auto* field = find_struct_field(object_structure->second,
                                                  spelling(member->member), &field_index);
            if (!field) {
                report("SEM051", "structure has no field named '" +
                       spelling(member->member) + "'", member->member);
                return Type::Error;
            }
            model_.member_field_indices[member] = field_index;
            if (field->type == Type::Struct)
                model_.struct_expression_types[&expression] = field->struct_type_id;
            if (field->type == Type::Enum)
                model_.enum_expression_types[&expression] = field->enum_type_id;
            if (field->type == Type::Array && field->array_type)
                model_.array_expression_types[&expression] = *field->array_type;
            return field->type;
        }

        if (const auto* range = std::get_if<RangeExpr>(&expression.data)) {
            if (!allow_range_expression_) {
                report("SEM046", "integer ranges can only be used directly in a for-in loop",
                       expression.span);
            }
            const bool previous_permission = allow_range_expression_;
            allow_range_expression_ = false;
            const Type start_type = range->start
                ? analyze_expression(*range->start) : Type::Error;
            const Type end_type = range->end
                ? analyze_expression(*range->end) : Type::Error;
            allow_range_expression_ = previous_permission;
            if (start_type != Type::Error && start_type != Type::Unknown &&
                start_type != Type::Int) {
                report("SEM047", "range start must have type int",
                       range->start ? range->start->span : expression.span);
            }
            if (end_type != Type::Error && end_type != Type::Unknown &&
                end_type != Type::Int) {
                report("SEM047", "range end must have type int",
                       range->end ? range->end->span : expression.span);
            }
            return Type::Range;
        }

        if (const auto* index = std::get_if<IndexExpr>(&expression.data)) {
            const Type object_type = index->object
                ? analyze_expression(*index->object) : Type::Error;
            const Type index_type = index->index
                ? analyze_expression(*index->index) : Type::Error;
            if (index_type != Type::Error && index_type != Type::Unknown &&
                index_type != Type::Int) {
                report("SEM042", "array index must have type int",
                       index->index ? index->index->span : expression.span);
            }
            if (object_type != Type::Array) {
                if (object_type != Type::Error && object_type != Type::Unknown)
                    report("SEM043", "indexing requires an array value",
                           expression.span);
                return Type::Error;
            }
            const auto shape = index->object
                ? model_.array_expression_types.find(index->object.get())
                : model_.array_expression_types.end();
            if (shape == model_.array_expression_types.end()) {
                report("SEM043", "array type information is unavailable for this expression",
                       expression.span);
                return Type::Error;
            }
            if (shape->second.is_nested()) {
                model_.array_expression_types[&expression] =
                    shape->second.immediate_array_element_shape();
                return Type::Array;
            }
            if (shape->second.element_type == Type::Struct)
                model_.struct_expression_types[&expression] =
                    shape->second.element_struct_type_id;
            if (shape->second.element_type == Type::Enum)
                model_.enum_expression_types[&expression] =
                    shape->second.element_enum_type_id;
            return shape->second.element_type;
        }

        if (const auto* unary = std::get_if<UnaryExpr>(&expression.data)) {
            if (!unary->operand) return Type::Error;
            if (unary->op == TokenKind::Minus &&
                is_int64_min_magnitude_literal(unary->operand.get(), source_)) {
                return Type::Int;
            }
            const Type operand = analyze_expression(*unary->operand);
            if (operand == Type::Error || operand == Type::Unknown) return operand;
            if (unary->op == TokenKind::BitAnd) {
                const auto* named = std::get_if<NameExpr>(&unary->operand->data);
                if (!named) {
                    report("SEM070", "address-of currently requires a mutable named local or parameter", unary->operand->span);
                    return Type::Error;
                }
                const auto resolved = model_.resolved_names.find(named);
                const auto binding = resolved == model_.resolved_names.end()
                    ? model_.bindings.end() : model_.bindings.find(resolved->second);
                if (binding == model_.bindings.end() || !binding->second.addressable ||
                    !binding->second.mutable_binding) {
                    report("SEM070", "address-of requires a mutable, addressable scalar local or parameter", unary->operand->span);
                    return Type::Error;
                }
                SemanticPointerType addressed;
                if (binding->second.type == Type::Pointer) {
                    if (!binding->second.pointer_type ||
                        binding->second.pointer_type->indirection_depth >= 8u) {
                        report("SEM070", "cannot take the address of an unsupported or maximally nested pointer binding",
                               unary->operand->span);
                        return Type::Error;
                    }
                    addressed = *binding->second.pointer_type;
                    ++addressed.indirection_depth;
                } else {
                    addressed = SemanticPointerType{
                        binding->second.type, binding->second.id, true,
                        binding->second.struct_type_id, binding->second.enum_type_id
                    };
                }
                addressed.pointee_binding_id = binding->second.id;
                addressed.pointee_parameter_index = 0u;
                addressed.mutable_pointee = true;
                model_.pointer_expression_types[&expression] = addressed;
                return Type::Pointer;
            }
            if (unary->op == TokenKind::Star) {
                const auto pointer = model_.pointer_expression_types.find(unary->operand.get());
                if (operand != Type::Pointer || pointer == model_.pointer_expression_types.end()) {
                    report("SEM070", "dereference requires a supported typed pointer", unary->operand->span);
                    return Type::Error;
                }
                if (pointer->second.indirection_depth == 0u) {
                    report("SEM070", "pointer descriptor has zero indirection depth", unary->operand->span);
                    return Type::Error;
                }
                if (pointer->second.indirection_depth > 1u) {
                    SemanticPointerType dereferenced = pointer->second;
                    --dereferenced.indirection_depth;
                    dereferenced.pointee_binding_id = 0u;
                    dereferenced.pointee_parameter_index = 0u;
                    if (pointer->second.pointee_binding_id != 0u) {
                        const auto origin = model_.bindings.find(
                            pointer->second.pointee_binding_id);
                        if (origin != model_.bindings.end() && origin->second.pointer_type)
                            dereferenced = *origin->second.pointer_type;
                    }
                    model_.pointer_expression_types[&expression] = dereferenced;
                    return Type::Pointer;
                }
                if (pointer->second.pointee_type == Type::Struct)
                    model_.struct_expression_types[&expression] =
                        pointer->second.pointee_struct_type_id;
                if (pointer->second.pointee_type == Type::Enum)
                    model_.enum_expression_types[&expression] =
                        pointer->second.pointee_enum_type_id;
                return pointer->second.pointee_type;
            }
            if (unary->op == TokenKind::Bang) {
                if (operand != Type::Bool) {
                    report("SEM011", "operator '!' requires a bool operand; got " +
                           std::string(type_name(operand)), unary->operand->span);
                    return Type::Error;
                }
                return Type::Bool;
            }
            if (unary->op == TokenKind::BitNot) {
                if (operand != Type::Int && operand != Type::Byte) {
                    report("SEM011", "bitwise NOT requires an int or byte operand; got " +
                           std::string(type_name(operand)), unary->operand->span);
                    return Type::Error;
                }
                return operand;
            }
            if (unary->op == TokenKind::Plus || unary->op == TokenKind::Minus) {
                if (!is_numeric(operand)) {
                    report("SEM011", "unary numeric operators require int or float; got " +
                           std::string(type_name(operand)), unary->operand->span);
                    return Type::Error;
                }
                return operand;
            }
            return Type::Error;
        }

        if (const auto* binary = std::get_if<BinaryExpr>(&expression.data)) {
            if (!binary->left || !binary->right) return Type::Error;
            const Type left = analyze_expression(*binary->left);
            const Type right = analyze_expression(*binary->right);
            if (left == Type::Error || right == Type::Error) return Type::Error;
            if ((binary->op == TokenKind::EqualEqual ||
                 binary->op == TokenKind::BangEqual) &&
                left == Type::Pointer && right == Type::Pointer) {
                const bool left_null = is_null_pointer_literal(binary->left.get());
                const bool right_null = is_null_pointer_literal(binary->right.get());
                const auto left_found = model_.pointer_expression_types.find(binary->left.get());
                const auto right_found = model_.pointer_expression_types.find(binary->right.get());
                const std::optional<SemanticPointerType> left_type =
                    left_found == model_.pointer_expression_types.end()
                        ? std::nullopt : std::optional<SemanticPointerType>{left_found->second};
                const std::optional<SemanticPointerType> right_type =
                    right_found == model_.pointer_expression_types.end()
                        ? std::nullopt : std::optional<SemanticPointerType>{right_found->second};
                const auto typed = [](const std::optional<SemanticPointerType>& pointer) {
                    return pointer && pointer->pointee_type != Type::Unknown &&
                           pointer->pointee_type != Type::Error;
                };
                if (left_null && !right_null && typed(right_type))
                    model_.pointer_expression_types[binary->left.get()] = *right_type;
                if (right_null && !left_null && typed(left_type))
                    model_.pointer_expression_types[binary->right.get()] = *left_type;
                const auto resolved_left = model_.pointer_expression_types.find(binary->left.get());
                const auto resolved_right = model_.pointer_expression_types.find(binary->right.get());
                if (typed(resolved_left == model_.pointer_expression_types.end()
                              ? std::nullopt
                              : std::optional<SemanticPointerType>{resolved_left->second}) &&
                    typed(resolved_right == model_.pointer_expression_types.end()
                              ? std::nullopt
                              : std::optional<SemanticPointerType>{resolved_right->second}) &&
                    (resolved_left->second.pointee_type != resolved_right->second.pointee_type ||
                     resolved_left->second.pointee_struct_type_id != resolved_right->second.pointee_struct_type_id ||
                     resolved_left->second.pointee_enum_type_id != resolved_right->second.pointee_enum_type_id ||
                     resolved_left->second.indirection_depth != resolved_right->second.indirection_depth)) {
                    report("SEM014", "pointer equality requires identical pointee types", expression.span);
                    return Type::Error;
                }
            }
            if (left == Type::Unknown || right == Type::Unknown) return Type::Unknown;
            if(binary->op==TokenKind::Plus&&left==Type::String&&right==Type::String)
                return Type::String;

            switch (binary->op) {
            case TokenKind::Plus:
            case TokenKind::Minus:
            case TokenKind::Star:
            case TokenKind::Slash: {
                const Type result = common_numeric_type(left, right);
                if (result == Type::Error) {
                    report("SEM012", "operator '" +
                           std::string(token_kind_name(binary->op)) +
                           "' requires numeric operands; got " +
                           type_name(left) + " and " + type_name(right),
                           expression.span);
                }
                return result;
            }
            case TokenKind::Percent:
                if (left != Type::Int || right != Type::Int) {
                    report("SEM012", "operator '%' requires two int operands",
                           expression.span);
                    return Type::Error;
                }
                return Type::Int;
            case TokenKind::BitAnd:
            case TokenKind::BitOr:
            case TokenKind::BitXor:
                if (left != right || (left != Type::Int && left != Type::Byte)) {
                    report("SEM012", "bitwise operators require two int operands or two byte operands",
                           expression.span);
                    return Type::Error;
                }
                return left;
            case TokenKind::ShiftLeft:
            case TokenKind::ShiftRight:
                if ((left != Type::Int && left != Type::Byte) || right != Type::Int) {
                    report("SEM012", "bit shifts require an int or byte left operand and an int count",
                           expression.span);
                    return Type::Error;
                }
                return left;
            case TokenKind::Less:
            case TokenKind::LessEqual:
            case TokenKind::Greater:
            case TokenKind::GreaterEqual:
                if (!((is_numeric(left) && is_numeric(right)) ||
                      (left == Type::Byte && right == Type::Byte))) {
                    report("SEM013", "comparison operators require numeric operands or two byte operands",
                           expression.span);
                    return Type::Error;
                }
                return Type::Bool;
            case TokenKind::EqualEqual:
            case TokenKind::BangEqual:
                if (left == Type::Array || right == Type::Array) {
                    report("SEM014", "fixed arrays cannot be compared as whole values", expression.span);
                    return Type::Error;
                }
                if (left == Type::Struct || right == Type::Struct) {
                    report("SEM014", "structure values cannot be compared until an explicit equality model is defined", expression.span);
                    return Type::Error;
                }
                if (left == Type::Pointer && right == Type::Pointer) {
                    const auto left_pointer = model_.pointer_expression_types.find(binary->left.get());
                    const auto right_pointer = model_.pointer_expression_types.find(binary->right.get());
                    if (left_pointer == model_.pointer_expression_types.end() ||
                        right_pointer == model_.pointer_expression_types.end() ||
                        left_pointer->second.pointee_type == Type::Unknown ||
                        right_pointer->second.pointee_type == Type::Unknown) {
                        report("SEM073", "pointer equality requires a typed pointer; two untyped null values are ambiguous",
                               expression.span);
                        return Type::Error;
                    }
                }
                if (left == Type::Enum && right == Type::Enum) {
                    const std::uint32_t left_enum =
                        model_.enum_expression_types.count(binary->left.get())
                            ? model_.enum_expression_types.at(binary->left.get()) : 0u;
                    const std::uint32_t right_enum =
                        model_.enum_expression_types.count(binary->right.get())
                            ? model_.enum_expression_types.at(binary->right.get()) : 0u;
                    if (left_enum == 0u || left_enum != right_enum) {
                        report("SEM014", "enum equality requires values of the same nominal enum type",
                               expression.span);
                        return Type::Error;
                    }
                }
                if ((!is_numeric(left) || !is_numeric(right)) && left != right) {
                    report("SEM014", "equality operands must have compatible types; got " +
                           std::string(type_name(left)) + " and " + type_name(right),
                           expression.span);
                    return Type::Error;
                }
                if (left == Type::Void || right == Type::Void) {
                    report("SEM014", "void values cannot be compared", expression.span);
                    return Type::Error;
                }
                return Type::Bool;
            case TokenKind::AndAnd:
            case TokenKind::OrOr:
                if (left != Type::Bool || right != Type::Bool) {
                    report("SEM015", "logical operators require two bool operands",
                           expression.span);
                    return Type::Error;
                }
                return Type::Bool;
            default:
                return Type::Error;
            }
        }

        if (const auto* assignment = std::get_if<AssignmentExpr>(&expression.data)) {
            const bool compound=assignment->op!=TokenKind::Equal;
            if (const auto* dereference = assignment->target
                    ? std::get_if<UnaryExpr>(&assignment->target->data) : nullptr;
                dereference && dereference->op == TokenKind::Star) {
                const Type target_type = analyze_expression(*assignment->target);
                const auto pointer = dereference->operand
                    ? model_.pointer_expression_types.find(dereference->operand.get())
                    : model_.pointer_expression_types.end();
                const bool mutable_pointer = pointer != model_.pointer_expression_types.end() &&
                    pointer->second.mutable_pointee;
                const SemanticBindingId pointer_cell_binding_id =
                    pointer == model_.pointer_expression_types.end()
                        ? 0u : pointer->second.pointee_binding_id;
                const auto target_pointer = model_.pointer_expression_types.find(
                    assignment->target.get());
                const std::optional<SemanticPointerType> target_pointer_type =
                    target_pointer == model_.pointer_expression_types.end()
                        ? std::nullopt
                        : std::optional<SemanticPointerType>{target_pointer->second};
                if (target_type == Type::Pointer && target_pointer_type &&
                    assignment->value && is_null_pointer_literal(assignment->value.get())) {
                    model_.pointer_expression_types[assignment->value.get()] =
                        *target_pointer_type;
                }
                const Type value_type = assignment->value
                    ? analyze_expression(*assignment->value) : Type::Error;
                if (!mutable_pointer) {
                    report("SEM071", "dereferenced pointer assignment requires a mutable typed pointer",
                           expression.span);
                    return Type::Error;
                }
                if (compound) {
                    const Type operation_type =
                        compound_assignment_result(assignment->op, target_type, value_type);
                    if (operation_type == Type::Error && target_type != Type::Error &&
                        value_type != Type::Error && target_type != Type::Unknown &&
                        value_type != Type::Unknown) {
                        report("SEM012", "compound assignment has incompatible pointer-pointee operands",
                               expression.span);
                    } else if (operation_type != Type::Error &&
                               operation_type != Type::Unknown &&
                               !is_assignable(target_type, operation_type)) {
                        report("SEM018", "compound pointer assignment result cannot be stored in the pointee type",
                               expression.span);
                    }
                } else if (!is_assignable(target_type, value_type)) {
                    report("SEM071", "dereferenced pointer assignment has an incompatible pointee value",
                           expression.span);
                } else if (target_type == Type::Pointer) {
                    const auto actual_pointer = assignment->value
                        ? model_.pointer_expression_types.find(assignment->value.get())
                        : model_.pointer_expression_types.end();
                    if (!target_pointer_type ||
                        actual_pointer == model_.pointer_expression_types.end() ||
                        target_pointer_type->pointee_type != actual_pointer->second.pointee_type ||
                        target_pointer_type->pointee_struct_type_id != actual_pointer->second.pointee_struct_type_id ||
                        target_pointer_type->pointee_enum_type_id != actual_pointer->second.pointee_enum_type_id ||
                        target_pointer_type->indirection_depth != actual_pointer->second.indirection_depth) {
                        report("SEM071", "pointer-to-pointer store requires an identical nested pointer type",
                               expression.span);
                    } else if (pointer_cell_binding_id != 0u) {
                        // A store through **T can retarget the pointer variable that
                        // the outer pointer addresses. Apply the same lexical escape
                        // check as a direct assignment to that pointer variable.
                        const auto origin = model_.bindings.find(
                            actual_pointer->second.pointee_binding_id);
                        const auto destination = model_.bindings.find(
                            pointer_cell_binding_id);
                        if (origin != model_.bindings.end() &&
                            destination != model_.bindings.end() &&
                            origin->second.scope_depth > destination->second.scope_depth) {
                            report("SEM072",
                                   "pointer assignment would outlive the local variable it points to",
                                   expression.span);
                        }
                        const auto cell = model_.bindings.find(pointer_cell_binding_id);
                        if (cell != model_.bindings.end()) {
                            cell->second.pointer_type = actual_pointer->second;
                            if (Symbol* symbol = lookup_binding_symbol(pointer_cell_binding_id))
                                symbol->pointer_type = actual_pointer->second;
                        }
                    }
                }
                return target_type;
            }
            if(assignment->target&&(std::holds_alternative<MemberExpr>(assignment->target->data)||
                                    std::holds_alternative<IndexExpr>(assignment->target->data))) {
                const Type target_type=analyze_expression(*assignment->target);
                const UnaryExpr* pointer_root =
                    member_root_dereference(assignment->target.get());
                if (pointer_root && pointer_root->operand) {
                    const auto pointer = model_.pointer_expression_types.find(
                        pointer_root->operand.get());
                    if (pointer == model_.pointer_expression_types.end() ||
                        !pointer->second.mutable_pointee) {
                        report("SEM055", "aggregate pointer assignment requires a mutable typed pointer",
                               assignment->target->span);
                        if (assignment->value) static_cast<void>(analyze_expression(*assignment->value));
                        return Type::Error;
                    }
                    model_.assignment_pointer_targets[assignment] =
                        pointer_root->operand.get();
                } else {
                    const NameExpr* root=member_root_name(assignment->target.get());
                    if(!root) {
                        report("SEM055","aggregate elements and fields can only be assigned through a named binding or mutable pointer",assignment->target->span);
                        if(assignment->value)static_cast<void>(analyze_expression(*assignment->value));
                        return Type::Error;
                    }
                    Symbol* root_symbol=lookup(spelling(root->name));
                    if(!root_symbol||(root_symbol->type!=Type::Struct&&root_symbol->type!=Type::Array)) {
                        report("SEM055","aggregate assignment requires a named structure or array binding",root->name);
                        if(assignment->value)static_cast<void>(analyze_expression(*assignment->value));
                        return Type::Error;
                    }
                    model_.assignment_targets[assignment]=root_symbol->id;
                    if(root_symbol->immutable)
                        report("SEM017","cannot modify an aggregate through immutable binding '"+spelling(root->name)+"'",root->name);
                }
                const auto expected_array=model_.array_expression_types.find(assignment->target.get());
                if(target_type==Type::Array&&expected_array!=model_.array_expression_types.end()&&assignment->value&&
                   std::holds_alternative<ArrayLiteralExpr>(assignment->value->data))
                    model_.array_expression_types[assignment->value.get()]=expected_array->second;
                const Type actual=assignment->value?analyze_expression(*assignment->value):Type::Error;
                if(compound) {
                    const Type operation_type=compound_assignment_result(assignment->op,target_type,actual);
                    if(operation_type==Type::Error&&target_type!=Type::Error&&actual!=Type::Error&&
                       target_type!=Type::Unknown&&actual!=Type::Unknown)
                        report("SEM012","compound assignment has incompatible operand types; '%=' requires two ints and string concatenation is only supported by '+='",expression.span);
                    else if(operation_type!=Type::Error&&operation_type!=Type::Unknown&&
                            !is_assignable(target_type,operation_type))
                        report("SEM018","compound assignment result cannot be stored in the target type",expression.span);
                    return target_type;
                }
                const std::uint32_t target_struct=model_.struct_expression_types.count(assignment->target.get())
                    ?model_.struct_expression_types.at(assignment->target.get()):0u;
                const std::uint32_t actual_struct=assignment->value&&model_.struct_expression_types.count(assignment->value.get())
                    ?model_.struct_expression_types.at(assignment->value.get()):0u;
                const auto actual_array=assignment->value?model_.array_expression_types.find(assignment->value.get())
                    :model_.array_expression_types.end();
                const bool array_shape_matches=target_type!=Type::Array||
                    (expected_array!=model_.array_expression_types.end()&&actual_array!=model_.array_expression_types.end()&&
                     expected_array->second.same_shape(actual_array->second));
                if(!is_assignable(target_type,actual)||(target_type==Type::Struct&&target_struct!=actual_struct)||!array_shape_matches)
                    report("SEM018","cannot assign an incompatible value to an aggregate field or element",expression.span);
                if(target_type==Type::Struct&&target_struct!=0)model_.struct_expression_types[&expression]=target_struct;
                if(target_type==Type::Array&&expected_array!=model_.array_expression_types.end())
                    model_.array_expression_types[&expression]=expected_array->second;
                return target_type;
            }
            const auto* target=assignment->target?std::get_if<NameExpr>(&assignment->target->data):nullptr;
            if(!target)return Type::Error;
            const std::string identifier=spelling(target->name);
            Symbol* symbol=lookup(identifier);
            if(!symbol) {
                report("SEM001","undefined identifier '"+identifier+"'",target->name);
                if(assignment->value)static_cast<void>(analyze_expression(*assignment->value));
                return Type::Error;
            }
            if(symbol->is_function) {
                report("SEM016","cannot assign to function '"+identifier+"'",target->name);
                if(assignment->value)static_cast<void>(analyze_expression(*assignment->value));
                return Type::Error;
            }
            model_.assignment_targets[assignment]=symbol->id;
            if (assignment->value && symbol->type == Type::Array &&
                symbol->array_type &&
                std::holds_alternative<ArrayLiteralExpr>(assignment->value->data)) {
                model_.array_expression_types[assignment->value.get()] = *symbol->array_type;
            }
            if (symbol->type == Type::Pointer && symbol->pointer_type &&
                assignment->value && is_null_pointer_literal(assignment->value.get())) {
                model_.pointer_expression_types[assignment->value.get()] =
                    *symbol->pointer_type;
            }
            const Type value=assignment->value?analyze_expression(*assignment->value):Type::Error;
            if (symbol->type == Type::Pointer) {
                const auto pointer = assignment->value
                    ? model_.pointer_expression_types.find(assignment->value.get())
                    : model_.pointer_expression_types.end();
                if (pointer == model_.pointer_expression_types.end() || !symbol->pointer_type ||
                    pointer->second.pointee_type != symbol->pointer_type->pointee_type ||
                    pointer->second.pointee_struct_type_id != symbol->pointer_type->pointee_struct_type_id ||
                    pointer->second.pointee_enum_type_id != symbol->pointer_type->pointee_enum_type_id ||
                    pointer->second.indirection_depth != symbol->pointer_type->indirection_depth) {
                    report("SEM071", "pointer assignment requires a pointer with the same pointee type", expression.span);
                } else {
                    const auto origin = model_.bindings.find(pointer->second.pointee_binding_id);
                    const auto destination = model_.bindings.find(symbol->id);
                    if (origin != model_.bindings.end() && destination != model_.bindings.end() &&
                        origin->second.scope_depth > destination->second.scope_depth)
                        report("SEM072", "pointer assignment would outlive the local variable it points to", expression.span);
                    // Later reads of this pointer binding must observe its current target origin,
                    // otherwise a subsequent copy could inherit a stale lifetime origin.
                    symbol->pointer_type = pointer->second;
                    const auto binding = model_.bindings.find(symbol->id);
                    if (binding != model_.bindings.end())
                        binding->second.pointer_type = pointer->second;
                    model_.pointer_expression_types[&expression] = pointer->second;
                }
            }
            if(symbol->immutable)report("SEM017","cannot assign to immutable binding '"+identifier+"'",target->name);
            if(compound) {
                const Type operation_type=compound_assignment_result(assignment->op,symbol->type,value);
                if(operation_type==Type::Error&&symbol->type!=Type::Error&&value!=Type::Error&&
                   symbol->type!=Type::Unknown&&value!=Type::Unknown)
                    report("SEM012","compound assignment has incompatible operand types; '%=' requires two ints and string concatenation is only supported by '+='",expression.span);
                else if(operation_type!=Type::Error&&operation_type!=Type::Unknown&&!is_assignable(symbol->type,operation_type))
                    report("SEM018","compound assignment result cannot be stored in the target type",expression.span);
                return symbol->type;
            }
            if(symbol->type==Type::Array&&assignment->value) {
                const auto shape=model_.array_expression_types.find(assignment->value.get());
                if(symbol->array_type&&shape!=model_.array_expression_types.end()&&
                   (!symbol->array_type->same_shape(shape->second)))
                    report("SEM045","array assignment requires the same element type and complete dimension shape",assignment->value->span);
            }
            const std::uint32_t assigned_struct=assignment->value&&model_.struct_expression_types.count(assignment->value.get())
                ?model_.struct_expression_types.at(assignment->value.get()):0u;
            const std::uint32_t assigned_enum=assignment->value&&model_.enum_expression_types.count(assignment->value.get())
                ?model_.enum_expression_types.at(assignment->value.get()):0u;
            if(!is_assignable(symbol->type,value)||
               (symbol->type==Type::Struct&&symbol->struct_type_id!=assigned_struct)||
               (symbol->type==Type::Enum&&symbol->enum_type_id!=assigned_enum))
                report("SEM018","cannot assign "+std::string(type_name(value))+" to "+type_name(symbol->type)+
                       " binding '"+identifier+"'",expression.span);
            if(symbol->type==Type::Struct)model_.struct_expression_types[&expression]=symbol->struct_type_id;
            if(symbol->type==Type::Enum)model_.enum_expression_types[&expression]=symbol->enum_type_id;
            return symbol->type;
        }

        if (const auto* call = std::get_if<CallExpr>(&expression.data)) {
            const Expr* callee = call->callee.get();
            while (callee) {
                const auto* group = std::get_if<GroupExpr>(&callee->data);
                if (!group) break;
                callee = group->expression.get();
            }

            const auto* name = callee ? std::get_if<NameExpr>(&callee->data) : nullptr;
            const auto* enum_variant = callee
                ? std::get_if<MemberExpr>(&callee->data) : nullptr;
            const auto* enum_name = enum_variant && enum_variant->object
                ? std::get_if<NameExpr>(&enum_variant->object->data) : nullptr;
            if (enum_variant && enum_name) {
                const auto enum_id = enum_ids_.find(spelling(enum_name->name));
                if (enum_id != enum_ids_.end()) {
                    const SemanticEnumType& enumeration =
                        model_.enum_types[enum_id->second - 1u];
                    const std::string variant_name = spelling(enum_variant->member);
                    const auto variant = std::find_if(
                        enumeration.variants.begin(), enumeration.variants.end(),
                        [&](const SemanticEnumVariant& candidate) {
                            return candidate.name == variant_name;
                        });
                    if (variant == enumeration.variants.end()) {
                        report("SEM066", "enum '" + enumeration.name +
                               "' has no variant named '" + variant_name + "'",
                               enum_variant->member);
                        for (const ExprPtr& argument : call->arguments)
                            if (argument) static_cast<void>(analyze_expression(*argument));
                        return Type::Error;
                    }
                    const std::uint32_t variant_index =
                        static_cast<std::uint32_t>(variant - enumeration.variants.begin());
                    if (call->arguments.size() != variant->fields.size()) {
                        report("SEM067", "enum variant '" + enumeration.name + "." +
                               variant_name + "' expects " +
                               std::to_string(variant->fields.size()) +
                               " payload argument(s), but got " +
                               std::to_string(call->arguments.size()), expression.span);
                    }
                    const std::size_t count = std::min(
                        call->arguments.size(), variant->fields.size());
                    for (std::size_t index = 0; index < call->arguments.size(); ++index) {
                        const ExprPtr& argument = call->arguments[index];
                        if (index < count && argument &&
                            variant->fields[index].type == Type::Array &&
                            variant->fields[index].array_type &&
                            std::holds_alternative<ArrayLiteralExpr>(argument->data)) {
                            model_.array_expression_types[argument.get()] =
                                *variant->fields[index].array_type;
                        }
                        const Type actual = argument
                            ? analyze_expression(*argument) : Type::Error;
                        if (index >= count || !argument) continue;
                        const SemanticEnumPayloadField& expected = variant->fields[index];
                        const std::uint32_t actual_struct =
                            model_.struct_expression_types.count(argument.get())
                                ? model_.struct_expression_types.at(argument.get()) : 0u;
                        const std::uint32_t actual_enum =
                            model_.enum_expression_types.count(argument.get())
                                ? model_.enum_expression_types.at(argument.get()) : 0u;
                        const auto actual_array =
                            model_.array_expression_types.find(argument.get());
                        const bool array_matches = expected.type != Type::Array ||
                            (expected.array_type &&
                             actual_array != model_.array_expression_types.end() &&
                             expected.array_type->same_shape(actual_array->second));
                        if (!is_assignable(expected.type, actual) ||
                            (expected.type == Type::Struct &&
                             expected.struct_type_id != actual_struct) ||
                            (expected.type == Type::Enum &&
                             expected.enum_type_id != actual_enum) ||
                            !array_matches) {
                            report("SEM068", "payload argument " +
                                   std::to_string(index + 1u) + " for '" +
                                   enumeration.name + "." + variant_name +
                                   "' has an incompatible type or nominal identity",
                                   argument->span);
                        }
                    }
                    model_.enum_expression_types[&expression] = enumeration.id;
                    model_.enum_expression_variants[&expression] = variant_index;
                    return Type::Enum;
                }
            }
            Symbol* function = nullptr;
            if (name) {
                const std::string identifier = spelling(name->name);
                function = lookup(identifier);
                if (!function) {
                    report("SEM001", "undefined identifier '" + identifier + "'",
                           name->name);
                } else if (!function->is_function) {
                    report("SEM019", "value '" + identifier + "' is not callable",
                           name->name);
                    function = nullptr;
                }
            } else if (call->callee) {
                static_cast<void>(analyze_expression(*call->callee));
                report("SEM019", "only named functions can be called in this VEX version",
                       call->callee->span);
            }

            std::vector<Type> argument_types;
            std::vector<std::uint32_t> argument_struct_type_ids;
            std::vector<std::uint32_t> argument_enum_type_ids;
            argument_types.reserve(call->arguments.size());
            for (std::size_t index = 0; index < call->arguments.size(); ++index) {
                const ExprPtr& argument = call->arguments[index];
                if (function && argument &&
                    index < function->parameter_pointer_types.size() &&
                    function->parameter_pointer_types[index] &&
                    is_null_pointer_literal(argument.get())) {
                    model_.pointer_expression_types[argument.get()] =
                        *function->parameter_pointer_types[index];
                }
                if (function && argument &&
                    index < function->parameter_array_types.size() &&
                    function->parameter_array_types[index] &&
                    std::holds_alternative<ArrayLiteralExpr>(argument->data)) {
                    model_.array_expression_types[argument.get()] =
                        *function->parameter_array_types[index];
                }
                if (name && spelling(name->name) == "push" && index == 1u &&
                    argument && std::holds_alternative<ArrayLiteralExpr>(argument->data) &&
                    !call->arguments.empty() && call->arguments[0]) {
                    const auto outer_shape =
                        model_.array_expression_types.find(call->arguments[0].get());
                    if (outer_shape != model_.array_expression_types.end() &&
                        outer_shape->second.is_nested()) {
                        model_.array_expression_types[argument.get()] =
                            outer_shape->second.immediate_array_element_shape();
                    }
                }
                argument_types.push_back(argument
                    ? analyze_expression(*argument) : Type::Error);
                const auto structure = argument
                    ? model_.struct_expression_types.find(argument.get())
                    : model_.struct_expression_types.end();
                argument_struct_type_ids.push_back(
                    structure == model_.struct_expression_types.end() ? 0u : structure->second);
                const auto enumeration = argument
                    ? model_.enum_expression_types.find(argument.get())
                    : model_.enum_expression_types.end();
                argument_enum_type_ids.push_back(
                    enumeration == model_.enum_expression_types.end() ? 0u : enumeration->second);
            }

            if (!function) return Type::Error;
            if (name) model_.resolved_calls[call] = spelling(name->name);

            const std::string builtin_name = name ? spelling(name->name) : std::string{};

            if (builtin_name == "rotate_left" || builtin_name == "rotate_right") {
                if (argument_types.size() != 2u) {
                    report("SEM020", builtin_name + " expects 2 arguments", expression.span);
                    return Type::Error;
                }
                if ((argument_types[0] != Type::Int && argument_types[0] != Type::Byte) ||
                    argument_types[1] != Type::Int) {
                    report("SEM021", builtin_name +
                           " requires an int or byte value and an int count", expression.span);
                    return Type::Error;
                }
                return argument_types[0];
            }

            const auto is_byte_array_argument = [&](const std::size_t index) {
                if (index >= argument_types.size() || argument_types[index] != Type::Array ||
                    index >= call->arguments.size() || !call->arguments[index]) return false;
                const auto shape = model_.array_expression_types.find(call->arguments[index].get());
                return shape != model_.array_expression_types.end() &&
                       shape->second.dimensions.size() == 1u &&
                       shape->second.element_type == Type::Byte &&
                       shape->second.length == 0 &&
                       shape->second.element_struct_type_id == 0;
            };
            const auto is_int_argument = [&](const std::size_t index) {
                return index < argument_types.size() &&
                       argument_types[index] == Type::Int;
            };
            const auto check_mutable_byte_array = [&](const std::size_t index,
                                                       const char* operation) {
                if (index >= call->arguments.size() || !call->arguments[index]) return;
                if (const NameExpr* array_name =
                        member_root_name(call->arguments[index].get())) {
                    Symbol* array_symbol = lookup(spelling(array_name->name));
                    if (array_symbol && array_symbol->immutable) {
                        report("SEM017", std::string(operation) +
                               " cannot modify a byte array through immutable binding '" +
                               spelling(array_name->name) + "'", array_name->name);
                    }
                }
            };

            if (builtin_name == "byte_slice") {
                if (argument_types.size() != 3) {
                    report("SEM020", "byte_slice expects 3 arguments", expression.span);
                    return Type::Error;
                }
                if (!is_byte_array_argument(0) || !is_int_argument(1) ||
                    !is_int_argument(2)) {
                    report("SEM021", "byte_slice requires byte[], int, int", expression.span);
                    return Type::Error;
                }
                model_.array_expression_types[&expression] =
                    SemanticArrayType{Type::Byte, 0, 0};
                return Type::Array;
            }
            if (builtin_name == "read_file" || builtin_name == "write_file") {
                if (argument_types.size() != 2) {
                    report("SEM020", builtin_name + " expects 2 arguments", expression.span);
                    return Type::Error;
                }
                if (argument_types[0] != Type::String || !is_byte_array_argument(1)) {
                    report("SEM021", builtin_name + " requires a string path and a dynamic byte array",
                           expression.span);
                    return Type::Error;
                }
                if (builtin_name == "read_file")
                    check_mutable_byte_array(1, "read_file");
                return Type::Int;
            }
            if (builtin_name == "read_u16_le" || builtin_name == "read_u16_be" ||
                builtin_name == "read_u32_le" || builtin_name == "read_u32_be") {
                if (argument_types.size() != 2) {
                    report("SEM020", builtin_name + " expects 2 arguments", expression.span);
                    return Type::Error;
                }
                if (!is_byte_array_argument(0) || !is_int_argument(1)) {
                    report("SEM021", builtin_name + " requires byte[] and an int offset",
                           expression.span);
                    return Type::Error;
                }
                return Type::Int;
            }
            if (builtin_name == "write_u16_le" || builtin_name == "write_u16_be" ||
                builtin_name == "write_u32_le" || builtin_name == "write_u32_be") {
                if (argument_types.size() != 3) {
                    report("SEM020", builtin_name + " expects 3 arguments", expression.span);
                    return Type::Error;
                }
                if (!is_byte_array_argument(0) || !is_int_argument(1) ||
                    !is_int_argument(2)) {
                    report("SEM021", builtin_name + " requires byte[], int offset, int value",
                           expression.span);
                    return Type::Error;
                }
                check_mutable_byte_array(0, builtin_name.c_str());
                return Type::Void;
            }

            if (builtin_name == "length" || builtin_name == "capacity" ||
                builtin_name == "join" || builtin_name == "push" ||
                builtin_name == "pop" || builtin_name == "reserve") {
                const std::size_t expected_count =
                    (builtin_name == "push" || builtin_name == "reserve") ? 2u : 1u;
                if (argument_types.size() != expected_count) {
                    report("SEM020", builtin_name + " expects " +
                           std::to_string(expected_count) + " argument(s)", expression.span);
                    return Type::Error;
                }
                if (builtin_name == "length" && argument_types[0] == Type::String)
                    return Type::Int;
                if (argument_types[0] != Type::Array || !call->arguments[0]) {
                    const std::string requirement = builtin_name == "length"
                        ? " requires a string or array as its first argument"
                        : " requires an array as its first argument";
                    report("SEM021", builtin_name + requirement, call->arguments[0]
                               ? call->arguments[0]->span : expression.span);
                    return Type::Error;
                }
                const auto shape = model_.array_expression_types.find(call->arguments[0].get());
                if (shape == model_.array_expression_types.end()) {
                    report("SEM021", builtin_name + " could not determine the array element type",
                           call->arguments[0]->span);
                    return Type::Error;
                }
                if (builtin_name == "length") return Type::Int;
                if (builtin_name == "join") {
                    if (shape->second.dimensions.size() != 1u ||
                        shape->second.element_type != Type::String) {
                        report("SEM021", "join requires a dynamic array of strings",
                               call->arguments[0]->span);
                        return Type::Error;
                    }
                    if (shape->second.length != 0) {
                        report("SEM021", "join requires a dynamic string array declared with type []",
                               call->arguments[0]->span);
                        return Type::Error;
                    }
                    return Type::String;
                }
                if (shape->second.length != 0) {
                    report("SEM021", builtin_name +
                           " requires a dynamic array declared with type []", call->arguments[0]->span);
                    return Type::Error;
                }
                const bool mutating_array_builtin =
                    builtin_name == "push" || builtin_name == "pop" ||
                    builtin_name == "reserve";
                if (mutating_array_builtin) {
                    if (const NameExpr* array_name =
                            member_root_name(call->arguments[0].get())) {
                        Symbol* array_symbol = lookup(spelling(array_name->name));
                        if (array_symbol && array_symbol->immutable) {
                            report("SEM017", builtin_name +
                                   " cannot modify an array through immutable binding '" +
                                   spelling(array_name->name) + "'", array_name->name);
                        }
                    }
                }
                if (builtin_name == "capacity") return Type::Int;
                if (builtin_name == "reserve") {
                    if (argument_types[1] != Type::Int &&
                        argument_types[1] != Type::Error &&
                        argument_types[1] != Type::Unknown) {
                        report("SEM021", "reserve capacity must have type int",
                               call->arguments[1]->span);
                        return Type::Error;
                    }
                    return Type::Void;
                }
                if (builtin_name == "push") {
                    if (shape->second.is_nested()) {
                        const auto value_shape = call->arguments[1]
                            ? model_.array_expression_types.find(call->arguments[1].get())
                            : model_.array_expression_types.end();
                        const SemanticArrayType expected_element =
                            shape->second.immediate_array_element_shape();
                        if (argument_types[1] != Type::Array ||
                            value_shape == model_.array_expression_types.end() ||
                            !value_shape->second.same_shape(expected_element)) {
                            report("SEM021", "push value does not match the nested array element shape",
                                   call->arguments[1]->span);
                            return Type::Error;
                        }
                    } else {
                        const std::uint32_t value_struct_type_id = call->arguments[1] &&
                            model_.struct_expression_types.find(call->arguments[1].get()) !=
                                model_.struct_expression_types.end()
                            ? model_.struct_expression_types.at(call->arguments[1].get()) : 0u;
                        const std::uint32_t value_enum_type_id = call->arguments[1] &&
                            model_.enum_expression_types.find(call->arguments[1].get()) !=
                                model_.enum_expression_types.end()
                            ? model_.enum_expression_types.at(call->arguments[1].get()) : 0u;
                        if (!is_assignable(shape->second.element_type, argument_types[1]) ||
                            (shape->second.element_type == Type::Struct &&
                             shape->second.element_struct_type_id != value_struct_type_id) ||
                            (shape->second.element_type == Type::Enum &&
                             shape->second.element_enum_type_id != value_enum_type_id)) {
                            report("SEM021", "push value has type " +
                                    std::string(type_name(argument_types[1])) +
                                    "; expected " + type_name(shape->second.element_type),
                                    call->arguments[1]->span);
                            return Type::Error;
                        }
                    }
                    return Type::Void;
                }
                if (shape->second.is_nested()) {
                    model_.array_expression_types[&expression] =
                        shape->second.immediate_array_element_shape();
                    return Type::Array;
                }
                if (shape->second.element_type == Type::Struct)
                    model_.struct_expression_types[&expression] =
                        shape->second.element_struct_type_id;
                if (shape->second.element_type == Type::Enum)
                    model_.enum_expression_types[&expression] =
                        shape->second.element_enum_type_id;
                return shape->second.element_type;
            }

            if (builtin_name == "say" &&
                std::find(argument_types.begin(), argument_types.end(), Type::Array) != argument_types.end()) {
                report("SEM046", "say does not accept array values; index an element first",
                       expression.span);
            }
            if (builtin_name == "say" &&
                std::find(argument_types.begin(), argument_types.end(), Type::Struct) != argument_types.end()) {
                report("SEM056", "say does not accept structure values; select a printable field",
                       expression.span);
            }
            if (builtin_name == "say" &&
                std::find(argument_types.begin(), argument_types.end(), Type::Enum) != argument_types.end()) {
                report("SEM069", "say does not accept enum values; match a variant first",
                       expression.span);
            }
            if (argument_types.size() != function->parameters.size()) {
                report("SEM020", "function expects " +
                       std::to_string(function->parameters.size()) +
                       " argument(s), but got " +
                       std::to_string(argument_types.size()), expression.span);
            }
            const std::size_t count =
                std::min(argument_types.size(), function->parameters.size());
            for (std::size_t index = 0; index < count; ++index) {
                if (!is_assignable(function->parameters[index], argument_types[index]) ||
                    (function->parameters[index] == Type::Struct &&
                     index < function->parameter_struct_type_ids.size() &&
                     function->parameter_struct_type_ids[index] != argument_struct_type_ids[index]) ||
                    (function->parameters[index] == Type::Enum &&
                     index < function->parameter_enum_type_ids.size() &&
                     function->parameter_enum_type_ids[index] != argument_enum_type_ids[index])) {
                    report("SEM021", "argument " + std::to_string(index + 1) +
                           " has type " + type_name(argument_types[index]) +
                           "; expected " + type_name(function->parameters[index]),
                           call->arguments[index]->span);
                    continue;
                }
                if (function->parameters[index] == Type::Pointer) {
                    const auto actual_pointer = call->arguments[index]
                        ? model_.pointer_expression_types.find(call->arguments[index].get())
                        : model_.pointer_expression_types.end();
                    const auto expected_pointer = index < function->parameter_pointer_types.size()
                        ? function->parameter_pointer_types[index] : std::nullopt;
                    if (!expected_pointer || actual_pointer == model_.pointer_expression_types.end() ||
                        expected_pointer->pointee_type != actual_pointer->second.pointee_type ||
                        expected_pointer->pointee_struct_type_id != actual_pointer->second.pointee_struct_type_id ||
                        expected_pointer->pointee_enum_type_id != actual_pointer->second.pointee_enum_type_id ||
                        expected_pointer->indirection_depth != actual_pointer->second.indirection_depth) {
                        report("SEM021", "pointer argument " + std::to_string(index + 1) +
                               " does not match the parameter's exact pointee type",
                               call->arguments[index]->span);
                    }
                }
                if (function->parameters[index] == Type::Array &&
                    index < function->parameter_array_types.size() &&
                    function->parameter_array_types[index]) {
                    const auto actual_shape = call->arguments[index]
                        ? model_.array_expression_types.find(call->arguments[index].get())
                        : model_.array_expression_types.end();
                    const SemanticArrayType& expected =
                        *function->parameter_array_types[index];
                    if (actual_shape == model_.array_expression_types.end() ||
                        !actual_shape->second.same_shape(expected)) {
                        report("SEM021", "array argument " +
                               std::to_string(index + 1) +
                               " does not match the required element type and complete dimension shape",
                               call->arguments[index]->span);
                    }
                }
            }
            if (function->type == Type::Array && function->return_array_type)
                model_.array_expression_types[&expression] =
                    *function->return_array_type;
            if (function->type == Type::Struct && function->return_struct_type_id != 0)
                model_.struct_expression_types[&expression] = function->return_struct_type_id;
            if (function->type == Type::Enum && function->return_enum_type_id != 0)
                model_.enum_expression_types[&expression] = function->return_enum_type_id;
            if (function->type == Type::Pointer && function->return_pointer_type &&
                function->return_pointer_parameter_index) {
                if (*function->return_pointer_parameter_index == 0u) {
                    model_.pointer_expression_types[&expression] =
                        *function->return_pointer_type;
                } else {
                    const std::size_t source_index =
                        static_cast<std::size_t>(*function->return_pointer_parameter_index - 1u);
                    if (source_index < call->arguments.size() && call->arguments[source_index]) {
                        const auto source_pointer = model_.pointer_expression_types.find(
                            call->arguments[source_index].get());
                        if (source_pointer != model_.pointer_expression_types.end())
                            model_.pointer_expression_types[&expression] = source_pointer->second;
                    }
                }
            }
            return function->type;
        }

        return Type::Error;
    }

    void require_type(const Type actual, const Type expected,
                      const SourceSpan span, const char* code,
                      const char* description) {
        if (actual == Type::Error || actual == Type::Unknown ||
            is_assignable(expected, actual)) return;
        report(code, std::string(description) + "; expected " +
               type_name(expected) + ", got " + type_name(actual), span);
    }

    void analyze_variable(const VariableDeclStmt& variable) {
        Type declared = Type::Unknown;
        std::optional<SemanticArrayType> declared_array_type;
        std::uint32_t declared_struct_type_id = 0;
        std::uint32_t declared_enum_type_id = 0;
        std::optional<SemanticPointerType> declared_pointer_type;
        if (variable.type_annotation) {
            declared = annotation_type(*variable.type_annotation, false,
                                       &declared_array_type, &declared_struct_type_id,
                                       &declared_enum_type_id, &declared_pointer_type);
        }
        const auto* array_initializer = variable.initializer
            ? std::get_if<ArrayLiteralExpr>(&variable.initializer->data) : nullptr;
        if (array_initializer && declared == Type::Array && declared_array_type)
            model_.array_expression_types[variable.initializer.get()] = *declared_array_type;

        const bool has_initializer = static_cast<bool>(variable.initializer);
        if (has_initializer && declared == Type::Pointer && declared_pointer_type &&
            is_null_pointer_literal(variable.initializer.get())) {
            model_.pointer_expression_types[variable.initializer.get()] =
                *declared_pointer_type;
        }
        const Type inferred = has_initializer
            ? analyze_expression(*variable.initializer) : declared;
        if (!variable.type_annotation) {
            declared = inferred;
            if (!has_initializer)
                report("SEM059",
                       "a variable without an initializer requires an explicit type annotation",
                       variable.name);
        } else if (has_initializer) {
            require_type(inferred, declared, variable.initializer->span,
                         "SEM022", "initializer type does not match variable annotation");
            const std::uint32_t inferred_struct =
                model_.struct_expression_types.count(variable.initializer.get())
                    ? model_.struct_expression_types.at(variable.initializer.get()) : 0u;
            const std::uint32_t inferred_enum =
                model_.enum_expression_types.count(variable.initializer.get())
                    ? model_.enum_expression_types.at(variable.initializer.get()) : 0u;
            if (declared == Type::Struct && inferred == Type::Struct &&
                declared_struct_type_id != inferred_struct) {
                report("SEM022", "initializer structure type does not match variable annotation",
                       variable.initializer->span);
            }
            if (declared == Type::Enum && inferred == Type::Enum &&
                declared_enum_type_id != inferred_enum) {
                report("SEM022", "initializer enum type does not match variable annotation",
                       variable.initializer->span);
            }
        }

        if (has_initializer && inferred == Type::Pointer) {
            const auto inferred_pointer = model_.pointer_expression_types.find(variable.initializer.get());
            if (inferred_pointer == model_.pointer_expression_types.end()) {
                report("SEM070", "pointer initializer is missing its pointee type", variable.initializer->span);
            } else if (declared_pointer_type &&
                       (declared_pointer_type->pointee_type != inferred_pointer->second.pointee_type ||
                        declared_pointer_type->pointee_struct_type_id != inferred_pointer->second.pointee_struct_type_id ||
                        declared_pointer_type->pointee_enum_type_id != inferred_pointer->second.pointee_enum_type_id ||
                        declared_pointer_type->indirection_depth != inferred_pointer->second.indirection_depth)) {
                report("SEM071", "pointer initializer has a different pointee type than its annotation", variable.initializer->span);
            } else {
                // Retain origin information even when the source wrote an explicit *T annotation.
                declared_pointer_type = inferred_pointer->second;
                const auto origin = model_.bindings.find(inferred_pointer->second.pointee_binding_id);
                if (origin != model_.bindings.end() && origin->second.scope_depth > scopes_.size())
                    report("SEM072", "pointer would outlive the local variable it points to", variable.initializer->span);
            }
        }
        if (declared == Type::Pointer && !declared_pointer_type)
            report("SEM070", "pointer declaration is missing a supported pointee type", variable.name);

        if (!has_initializer) {
            if (variable.immutable) {
                report("SEM058", "const declarations require an explicit initializer",
                       variable.name);
            } else if (declared != Type::Error && declared != Type::Unknown &&
                       declared != Type::Int && declared != Type::Byte &&
                       declared != Type::Float && declared != Type::Bool &&
                       declared != Type::String) {
                report("SEM057",
                       "default initialization is currently supported only for scalar primitive types; arrays and structures require an explicit initializer",
                       variable.name);
            }
        }

        if (declared == Type::Void) {
            report("SEM023", "variables cannot have type void", variable.name);
            declared = Type::Error;
        }
        Symbol symbol;
        symbol.type = declared;
        if (declared == Type::Array) symbol.array_type = declared_array_type;
        if (declared == Type::Enum) {
            symbol.enum_type_id = declared_enum_type_id;
            if (!variable.type_annotation && variable.initializer) {
                const auto enumeration = model_.enum_expression_types.find(variable.initializer.get());
                if (enumeration != model_.enum_expression_types.end())
                    symbol.enum_type_id = enumeration->second;
            }
        }
        if (declared == Type::Struct) {
            symbol.struct_type_id = declared_struct_type_id;
            if (!variable.type_annotation && variable.initializer) {
                const auto structure = model_.struct_expression_types.find(variable.initializer.get());
                if (structure != model_.struct_expression_types.end())
                    symbol.struct_type_id = structure->second;
            }
        }
        if (variable.initializer && inferred == Type::Array) {
            const auto shape = model_.array_expression_types.find(variable.initializer.get());
            if (shape != model_.array_expression_types.end()) {
                if (declared_array_type && !declared_array_type->same_shape(shape->second)) {
                    report("SEM045",
                           "array initializer does not match the annotated element type and complete dimension shape",
                           variable.initializer->span);
                }
                if (declared_array_type && declared_array_type->length == 0 &&
                    shape->second.element_type != Type::Unknown) {
                    model_.array_expression_types[variable.initializer.get()] = *declared_array_type;
                    symbol.array_type = *declared_array_type;
                } else if (!symbol.array_type) {
                    symbol.array_type = shape->second;
                }
            }
        }
        if (declared == Type::Pointer) symbol.pointer_type = declared_pointer_type;
        symbol.immutable = variable.immutable;
        symbol.declaration = variable.name;
        SemanticBindingId binding_id = 0;
        if (define(spelling(variable.name), std::move(symbol), variable.name, &binding_id)) {
            model_.variable_bindings[&variable] = binding_id;
            SemanticBinding& binding = model_.bindings.at(binding_id);
            binding.addressable = !variable.immutable &&
                (binding.type == Type::Int || binding.type == Type::Byte ||
                 binding.type == Type::Float || binding.type == Type::Bool ||
                 binding.type == Type::String || binding.type == Type::Struct ||
                 binding.type == Type::Enum || binding.type == Type::Pointer);
        }
    }

    void analyze_function(const FunctionDeclStmt& function,
                          const Statement& statement) {
        const auto found = signatures_.find(&function);
        Symbol signature;
        if (found != signatures_.end()) signature = found->second;
        else signature.type = Type::Void;

        scopes_.emplace_back();
        const std::size_t count = std::min(function.parameters.size(),
                                           signature.parameters.size());
        for (std::size_t index = 0; index < function.parameters.size(); ++index) {
            const Parameter& parameter = function.parameters[index];
            Symbol symbol;
            symbol.type = index < count ? signature.parameters[index] : Type::Unknown;
            if (index < signature.parameter_array_types.size())
                symbol.array_type = signature.parameter_array_types[index];
            if (index < signature.parameter_struct_type_ids.size())
                symbol.struct_type_id = signature.parameter_struct_type_ids[index];
            if (index < signature.parameter_enum_type_ids.size())
                symbol.enum_type_id = signature.parameter_enum_type_ids[index];
            if (index < signature.parameter_pointer_types.size())
                symbol.pointer_type = signature.parameter_pointer_types[index];
            if (symbol.pointer_type)
                symbol.pointer_type->pointee_parameter_index =
                    static_cast<std::uint32_t>(index + 1u);
            symbol.declaration = parameter.name;
            SemanticBindingId binding_id = 0;
            if (define(spelling(parameter.name), std::move(symbol),
                       parameter.name, &binding_id)) {
                model_.parameter_bindings[&parameter] = binding_id;
                SemanticBinding& binding = model_.bindings.at(binding_id);
                binding.addressable = binding.mutable_binding &&
                    (binding.type == Type::Int || binding.type == Type::Byte ||
                     binding.type == Type::Float || binding.type == Type::Bool ||
                     binding.type == Type::String || binding.type == Type::Struct ||
                     binding.type == Type::Enum || binding.type == Type::Pointer);
            }
        }

        const bool was_in_function = inside_function_;
        const Type previous_return = current_return_type_;
        const auto previous_return_array_type = current_return_array_type_;
        const std::uint32_t previous_return_struct_type_id = current_return_struct_type_id_;
        const std::uint32_t previous_return_enum_type_id = current_return_enum_type_id_;
        const auto previous_return_pointer_type = current_return_pointer_type_;
        const auto previous_return_pointer_parameter_index =
            current_return_pointer_parameter_index_;
        inside_function_ = true;
        current_return_type_ = signature.type;
        current_return_array_type_ = signature.return_array_type;
        current_return_struct_type_id_ = signature.return_struct_type_id;
        current_return_enum_type_id_ = signature.return_enum_type_id;
        current_return_pointer_type_ = signature.return_pointer_type;
        current_return_pointer_parameter_index_ =
            signature.return_pointer_parameter_index;

        if (function.body) {
            analyze_block(*function.body, false);
            if (signature.type != Type::Void && signature.type != Type::Error &&
                control_flow(*function.body).falls_through) {
                report("SEM024",
                       "a non-void function can reach its end without returning a value",
                       statement.span);
            }
        }

        current_return_type_ = previous_return;
        current_return_array_type_ = previous_return_array_type;
        current_return_struct_type_id_ = previous_return_struct_type_id;
        current_return_enum_type_id_ = previous_return_enum_type_id;
        current_return_pointer_type_ = previous_return_pointer_type;
        current_return_pointer_parameter_index_ =
            previous_return_pointer_parameter_index;
        inside_function_ = was_in_function;
        scopes_.pop_back();
    }

    struct FlowSummary {
        bool falls_through{true};
        bool returns{false};
        bool breaks{false};
        bool continues{false};
    };

    [[nodiscard]] std::optional<bool> constant_boolean(const Expr* expression) const {
        if (!expression) return std::nullopt;
        if (const auto* literal = std::get_if<LiteralExpr>(&expression->data)) {
            if (literal->token_kind == TokenKind::KwTrue) return true;
            if (literal->token_kind == TokenKind::KwFalse) return false;
            return std::nullopt;
        }
        if (const auto* group = std::get_if<GroupExpr>(&expression->data))
            return constant_boolean(group->expression.get());
        if (const auto* unary = std::get_if<UnaryExpr>(&expression->data)) {
            if (unary->op != TokenKind::Bang) return std::nullopt;
            const auto operand = constant_boolean(unary->operand.get());
            if (operand) return !*operand;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::int64_t> constant_integer(
        const Expr* expression) const {
        if (!expression) return std::nullopt;
        if (const auto* group = std::get_if<GroupExpr>(&expression->data))
            return constant_integer(group->expression.get());

        if (const auto* literal = std::get_if<LiteralExpr>(&expression->data)) {
            if (literal->token_kind != TokenKind::Integer) return std::nullopt;
            const std::string_view text = source_.slice(expression->span);
            std::int64_t value = 0;
            const auto parsed = std::from_chars(
                text.data(), text.data() + text.size(), value);
            if (parsed.ec != std::errc{} ||
                parsed.ptr != text.data() + text.size()) return std::nullopt;
            return value;
        }

        if (const auto* unary = std::get_if<UnaryExpr>(&expression->data)) {
            if (unary->op == TokenKind::Minus &&
                is_int64_min_magnitude_literal(unary->operand.get(), source_)) {
                return std::numeric_limits<std::int64_t>::min();
            }
            const auto operand = constant_integer(unary->operand.get());
            if (!operand) return std::nullopt;
            if (unary->op == TokenKind::Plus) return operand;
            if (unary->op == TokenKind::Minus) {
                if (*operand == std::numeric_limits<std::int64_t>::min())
                    return std::nullopt;
                return -*operand;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<bool> for_iterable_is_nonempty(
        const ForStmt& loop) const {
        if (!loop.iterable) return std::nullopt;

        const auto array_shape =
            model_.array_expression_types.find(loop.iterable.get());
        if (array_shape != model_.array_expression_types.end()) {
            if (array_shape->second.length == 0) return std::nullopt;
            return true;
        }

        const Expr* iterable = loop.iterable.get();
        while (iterable) {
            const auto* group = std::get_if<GroupExpr>(&iterable->data);
            if (!group || !group->expression) break;
            iterable = group->expression.get();
        }
        const auto* range = iterable
            ? std::get_if<RangeExpr>(&iterable->data) : nullptr;
        if (!range) return std::nullopt;

        const auto start = constant_integer(range->start.get());
        const auto end = constant_integer(range->end.get());
        if (!start || !end) return std::nullopt;
        return range->inclusive ? *start <= *end : *start < *end;
    }

    [[nodiscard]] static FlowSummary join_paths(
        const FlowSummary& left, const FlowSummary& right) {
        return FlowSummary{
            left.falls_through || right.falls_through,
            left.returns || right.returns,
            left.breaks || right.breaks,
            left.continues || right.continues
        };
    }

    [[nodiscard]] FlowSummary control_flow(const Statement& statement) const {
        if (std::holds_alternative<ReturnStmt>(statement.data))
            return FlowSummary{false, true, false, false};
        if (std::holds_alternative<BreakStmt>(statement.data))
            return FlowSummary{false, false, true, false};
        if (std::holds_alternative<ContinueStmt>(statement.data))
            return FlowSummary{false, false, false, true};

        if (const auto* block = std::get_if<BlockStmt>(&statement.data)) {
            FlowSummary result;
            for (const StmtPtr& child : block->statements) {
                if (!child || !result.falls_through) continue;
                const FlowSummary next = control_flow(*child);
                result.returns = result.returns || next.returns;
                result.breaks = result.breaks || next.breaks;
                result.continues = result.continues || next.continues;
                result.falls_through = next.falls_through;
            }
            return result;
        }

        if (const auto* conditional = std::get_if<IfStmt>(&statement.data)) {
            const auto condition = constant_boolean(conditional->condition.get());
            if (condition) {
                const StmtPtr& selected = *condition
                    ? conditional->then_branch : conditional->else_branch;
                return selected ? control_flow(*selected) : FlowSummary{};
            }
            const FlowSummary then_flow = conditional->then_branch
                ? control_flow(*conditional->then_branch) : FlowSummary{};
            const FlowSummary else_flow = conditional->else_branch
                ? control_flow(*conditional->else_branch) : FlowSummary{};
            return join_paths(then_flow, else_flow);
        }

        if (const auto* loop = std::get_if<WhileStmt>(&statement.data)) {
            const auto condition = constant_boolean(loop->condition.get());
            if (condition && !*condition) return FlowSummary{};

            const FlowSummary body = loop->body
                ? control_flow(*loop->body) : FlowSummary{};
            if (condition && *condition) {
                // A literal-true loop can leave only through a reachable break.
                // Falling through or continuing merely starts another iteration.
                return FlowSummary{body.breaks, body.returns, false, false};
            }
            // Unknown conditions may be false before the first iteration.
            return FlowSummary{true, body.returns, false, false};
        }

        if (const auto* repeat = std::get_if<RepeatStmt>(&statement.data)) {
            const auto count = constant_integer(repeat->count.get());
            if (count && *count <= 0) return FlowSummary{};

            const FlowSummary body = repeat->body
                ? control_flow(*repeat->body) : FlowSummary{};
            if (count && *count > 0) {
                // A known positive count runs the body at least once.
                return FlowSummary{
                    body.falls_through || body.breaks || body.continues,
                    body.returns, false, false
                };
            }
            // An unknown count may be zero, so the loop cannot prove a return.
            return FlowSummary{true, body.returns, false, false};
        }

        if (const auto* loop = std::get_if<ForStmt>(&statement.data)) {
            const FlowSummary body = loop->body
                ? control_flow(*loop->body) : FlowSummary{};
            const auto nonempty = for_iterable_is_nonempty(*loop);
            if (nonempty && *nonempty) {
                return FlowSummary{
                    body.falls_through || body.breaks || body.continues,
                    body.returns, false, false
                };
            }
            // If non-emptiness is not proven, the loop may execute zero times.
            return FlowSummary{true, body.returns, false, false};
        }

        if (const auto* matched = std::get_if<MatchStmt>(&statement.data)) {
            const auto info = model_.match_statements.find(matched);
            if (info == model_.match_statements.end()) return FlowSummary{};
            FlowSummary result{false, false, false, false};
            for (const MatchArm& arm : matched->arms) {
                if (arm.body) result = join_paths(result, control_flow(*arm.body));
            }
            return result;
        }

        // Declarations, expressions and nested function declarations do not
        // terminate the current function's control flow.
        return FlowSummary{};
    }

    void analyze_block(const Statement& statement, const bool create_scope) {
        if (create_scope) scopes_.emplace_back();
        const auto* block = std::get_if<BlockStmt>(&statement.data);
        if (block) {
            for (const StmtPtr& child : block->statements) {
                if (child) analyze_statement(*child);
            }
        }
        if (create_scope) scopes_.pop_back();
    }

    void analyze_match(const MatchStmt& matched, const SourceSpan span) {
        const Type scrutinee_type = matched.value
            ? analyze_expression(*matched.value) : Type::Error;
        const std::uint32_t enum_id = matched.value &&
            model_.enum_expression_types.count(matched.value.get())
                ? model_.enum_expression_types.at(matched.value.get()) : 0u;
        if (scrutinee_type != Type::Enum || enum_id == 0u ||
            enum_id > model_.enum_types.size()) {
            if (scrutinee_type != Type::Error && scrutinee_type != Type::Unknown)
                report("SEM070", "match requires a value of a declared enum type", span);
            for (const MatchArm& arm : matched.arms)
                if (arm.body) analyze_statement(*arm.body);
            return;
        }

        const SemanticEnumType& enumeration = model_.enum_types[enum_id - 1u];
        std::vector<bool> seen(enumeration.variants.size(), false);
        bool wildcard_seen = false;
        SemanticMatchInfo info;
        info.enum_type_id = enum_id;
        info.arms.reserve(matched.arms.size());

        if (matched.arms.empty())
            report("SEM078", "match must contain at least one arm", span);

        for (std::size_t arm_index = 0; arm_index < matched.arms.size(); ++arm_index) {
            const MatchArm& arm = matched.arms[arm_index];
            SemanticMatchArmInfo arm_info;
            arm_info.wildcard = arm.wildcard;
            const SemanticEnumVariant* variant = nullptr;

            if (arm.wildcard) {
                if (wildcard_seen || arm_index + 1u != matched.arms.size())
                    report("SEM071", "a wildcard match arm must appear at most once and be final",
                           arm.variant_name);
                if (!arm.bindings.empty())
                    report("SEM072", "a wildcard match arm cannot bind payload values",
                           arm.bindings.front());
                wildcard_seen = true;
            } else {
                if (wildcard_seen)
                    report("SEM071", "no match arm may follow a wildcard arm", arm.enum_name);
                const std::string named_enum = spelling(arm.enum_name);
                if (named_enum != enumeration.name)
                    report("SEM073", "match pattern names enum '" + named_enum +
                           "' but the scrutinee has type '" + enumeration.name + "'",
                           arm.enum_name);
                const std::string variant_name = spelling(arm.variant_name);
                const auto found = std::find_if(
                    enumeration.variants.begin(), enumeration.variants.end(),
                    [&](const SemanticEnumVariant& candidate) {
                        return candidate.name == variant_name;
                    });
                if (found == enumeration.variants.end()) {
                    report("SEM074", "enum '" + enumeration.name +
                           "' has no variant named '" + variant_name + "'",
                           arm.variant_name);
                } else {
                    variant = &*found;
                    arm_info.variant_index = static_cast<std::uint32_t>(
                        found - enumeration.variants.begin());
                    if (seen[arm_info.variant_index])
                        report("SEM075", "enum variant '" + enumeration.name + "." +
                               variant_name + "' appears more than once in this match",
                               arm.variant_name);
                    seen[arm_info.variant_index] = true;
                    if (arm.bindings.size() != variant->fields.size())
                        report("SEM076", "match pattern for '" + enumeration.name + "." +
                               variant_name + "' requires " +
                               std::to_string(variant->fields.size()) +
                               " payload binding(s), but got " +
                               std::to_string(arm.bindings.size()), arm.variant_name);
                }
            }

            scopes_.emplace_back();
            if (variant) {
                const std::size_t count = std::min(
                    arm.bindings.size(), variant->fields.size());
                for (std::size_t index = 0; index < count; ++index) {
                    const SemanticEnumPayloadField& field = variant->fields[index];
                    Symbol payload;
                    payload.type = field.type;
                    payload.struct_type_id = field.struct_type_id;
                    payload.enum_type_id = field.enum_type_id;
                    payload.array_type = field.array_type;
                    payload.immutable = true;
                    payload.declaration = arm.bindings[index];
                    SemanticBindingId binding_id = 0;
                    if (define(spelling(arm.bindings[index]), std::move(payload),
                               arm.bindings[index], &binding_id)) {
                        arm_info.payload_bindings.push_back(binding_id);
                    }
                }
            }
            if (arm.body) analyze_statement(*arm.body);
            scopes_.pop_back();
            info.arms.push_back(std::move(arm_info));
        }

        if (!wildcard_seen &&
            std::find(seen.begin(), seen.end(), false) != seen.end()) {
            report("SEM077", "match is not exhaustive; add the missing enum variants or a final wildcard arm",
                   span);
        }
        model_.match_statements[&matched] = std::move(info);
    }

    void analyze_statement(const Statement& statement) {
        if (const auto* function =
                std::get_if<FunctionDeclStmt>(&statement.data)) {
            analyze_function(*function, statement);
            return;
        }
        if (const auto* variable =
                std::get_if<VariableDeclStmt>(&statement.data)) {
            analyze_variable(*variable);
            return;
        }
        if (const auto* import = std::get_if<ImportDeclStmt>(&statement.data)) {
            report("SEM061",
                   "import declarations are not yet supported; module resolution is not implemented",
                   import->path_literal);
            return;
        }
        if (std::holds_alternative<StructDeclStmt>(statement.data)) return;
        if (std::holds_alternative<BlockStmt>(statement.data)) {
            analyze_block(statement, true);
            return;
        }
        if (const auto* returned = std::get_if<ReturnStmt>(&statement.data)) {
            if (!inside_function_) {
                report("SEM025", "return is only valid inside a function",
                       statement.span);
                if (returned->value) {
                    static_cast<void>(analyze_expression(*returned->value));
                }
                return;
            }
            if (current_return_type_ == Type::Void) {
                if (returned->value) {
                    static_cast<void>(analyze_expression(*returned->value));
                    report("SEM026",
                           "a void function cannot return a value; add a return type annotation",
                           returned->value->span);
                }
            } else if (current_return_type_ != Type::Error) {
                if (!returned->value) {
                    report("SEM027", "a non-void function must return a value",
                           statement.span);
                } else {
                    const auto* array_return = std::get_if<ArrayLiteralExpr>(
                        &returned->value->data);
                    if (array_return && current_return_array_type_) {
                        model_.array_expression_types[returned->value.get()] =
                            *current_return_array_type_;
                    }
                    if (current_return_type_ == Type::Pointer &&
                        current_return_pointer_type_ &&
                        is_null_pointer_literal(returned->value.get())) {
                        model_.pointer_expression_types[returned->value.get()] =
                            *current_return_pointer_type_;
                    }
                    const Type actual = analyze_expression(*returned->value);
                    require_type(actual, current_return_type_, returned->value->span,
                                 "SEM028", "return type does not match function");
                    const std::uint32_t returned_struct =
                        model_.struct_expression_types.find(returned->value.get()) !=
                            model_.struct_expression_types.end()
                        ? model_.struct_expression_types.at(returned->value.get()) : 0u;
                    if (actual == Type::Struct && returned_struct != current_return_struct_type_id_)
                        report("SEM028", "returned structure type does not match function return type",
                               returned->value->span);
                    const std::uint32_t returned_enum =
                        model_.enum_expression_types.count(returned->value.get())
                            ? model_.enum_expression_types.at(returned->value.get()) : 0u;
                    if (actual == Type::Enum && returned_enum != current_return_enum_type_id_)
                        report("SEM028", "returned enum type does not match function return type",
                               returned->value->span);
                    if (actual == Type::Array && current_return_array_type_) {
                        const auto actual_shape =
                            model_.array_expression_types.find(returned->value.get());
                        if (actual_shape == model_.array_expression_types.end() ||
                            !actual_shape->second.same_shape(*current_return_array_type_)) {
                            report("SEM028",
                                   "returned array does not match the function's annotated element type and complete dimension shape",
                                   returned->value->span);
                        }
                    }
                    if (actual == Type::Pointer) {
                        const auto actual_pointer = model_.pointer_expression_types.find(
                            returned->value.get());
                        if (!current_return_pointer_type_ ||
                            actual_pointer == model_.pointer_expression_types.end() ||
                            actual_pointer->second.pointee_type !=
                                current_return_pointer_type_->pointee_type ||
                            actual_pointer->second.pointee_struct_type_id !=
                                current_return_pointer_type_->pointee_struct_type_id ||
                            actual_pointer->second.pointee_enum_type_id !=
                                current_return_pointer_type_->pointee_enum_type_id ||
                            actual_pointer->second.indirection_depth !=
                                current_return_pointer_type_->indirection_depth) {
                            report("SEM028", "returned pointer does not match the function's exact pointee type",
                                   returned->value->span);
                        } else if (!is_null_pointer_literal(returned->value.get()) &&
                                   (!current_return_pointer_parameter_index_ ||
                                    (*current_return_pointer_parameter_index_ == 0u ||
                                     actual_pointer->second.pointee_parameter_index !=
                                         *current_return_pointer_parameter_index_))) {
                            report("SEM072", "a function may return only a pointer derived directly from one of its pointer parameters; returning local storage would create a dangling pointer",
                                   returned->value->span);
                        }
                    }
                }
            }
            return;
        }
        if (std::holds_alternative<BreakStmt>(statement.data)) {
            if (loop_depth_ == 0)
                report("SEM032", "break is only valid inside a loop", statement.span);
            return;
        }
        if (std::holds_alternative<ContinueStmt>(statement.data)) {
            if (loop_depth_ == 0)
                report("SEM033", "continue is only valid inside a loop", statement.span);
            return;
        }
        if (const auto* conditional = std::get_if<IfStmt>(&statement.data)) {
            if (conditional->condition) {
                const Type condition = analyze_expression(*conditional->condition);
                require_type(condition, Type::Bool, conditional->condition->span,
                             "SEM029", "if condition must be boolean");
            }
            if (conditional->then_branch) analyze_statement(*conditional->then_branch);
            if (conditional->else_branch) analyze_statement(*conditional->else_branch);
            return;
        }
        if (const auto* loop = std::get_if<WhileStmt>(&statement.data)) {
            if (loop->condition) {
                const Type condition = analyze_expression(*loop->condition);
                require_type(condition, Type::Bool, loop->condition->span,
                             "SEM034", "while condition must be boolean");
            }
            ++loop_depth_;
            if (loop->body) analyze_statement(*loop->body);
            --loop_depth_;
            return;
        }
        if (const auto* repeat = std::get_if<RepeatStmt>(&statement.data)) {
            if (repeat->count) {
                const Type count = analyze_expression(*repeat->count);
                require_type(count, Type::Int, repeat->count->span,
                             "SEM030", "repeat count must be an integer");
            }
            ++loop_depth_;
            if (repeat->body) analyze_statement(*repeat->body);
            --loop_depth_;
            return;
        }
        if (const auto* matched = std::get_if<MatchStmt>(&statement.data)) {
            analyze_match(*matched, statement.span);
            return;
        }
        if (const auto* loop = std::get_if<ForStmt>(&statement.data)) {
            const Expr* iterable_expression = loop->iterable.get();
            while (iterable_expression) {
                const auto* group = std::get_if<GroupExpr>(&iterable_expression->data);
                if (!group || !group->expression) break;
                iterable_expression = group->expression.get();
            }
            const bool is_range = iterable_expression &&
                std::holds_alternative<RangeExpr>(iterable_expression->data);
            const bool previous_permission = allow_range_expression_;
            allow_range_expression_ = is_range;
            const Type iterable = loop->iterable
                ? analyze_expression(*loop->iterable) : Type::Error;
            allow_range_expression_ = previous_permission;
            std::optional<SemanticArrayType> array_type;
            Type iteration_type = Type::Unknown;
            if (iterable == Type::Array && loop->iterable) {
                const auto shape = model_.array_expression_types.find(loop->iterable.get());
                if (shape != model_.array_expression_types.end()) {
                    array_type = shape->second;
                    iteration_type = array_type->is_nested()
                        ? Type::Array : array_type->element_type;
                }
            } else if (iterable == Type::Range) {
                iteration_type = Type::Int;
            } else if (iterable != Type::Error && iterable != Type::Unknown) {
                report("SEM031", "for-in requires an array or integer range",
                       loop->iterable ? loop->iterable->span : loop->variable);
            }
            scopes_.emplace_back();
            Symbol iteration_variable;
            iteration_variable.type = iteration_type;
            if (array_type && iteration_type == Type::Struct)
                iteration_variable.struct_type_id = array_type->element_struct_type_id;
            if (array_type && iteration_type == Type::Enum)
                iteration_variable.enum_type_id = array_type->element_enum_type_id;
            if (array_type && iteration_type == Type::Array)
                iteration_variable.array_type =
                    array_type->immediate_array_element_shape();
            iteration_variable.declaration = loop->variable;
            SemanticBindingId iteration_binding = 0;
            if (define(spelling(loop->variable), std::move(iteration_variable),
                       loop->variable, &iteration_binding))
                model_.for_bindings[loop] = iteration_binding;
            ++loop_depth_;
            if (loop->body) analyze_statement(*loop->body);
            --loop_depth_;
            scopes_.pop_back();
            return;
        }
        if (const auto* expression =
                std::get_if<ExpressionStmt>(&statement.data)) {
            if (expression->expression) {
                static_cast<void>(analyze_expression(*expression->expression));
            }
        }
    }

    const SourceFile& source_;
    const Program& program_;
    std::vector<Diagnostic> diagnostics_;
    std::vector<Scope> scopes_;
    std::unordered_map<const FunctionDeclStmt*, Symbol> signatures_;
    std::unordered_map<std::string, std::uint32_t> struct_ids_;
    std::unordered_map<std::string, std::uint32_t> enum_ids_;
    std::vector<const StructDeclStmt*> struct_declarations_;
    std::vector<const EnumDeclStmt*> enum_declarations_;
    SemanticModel model_;
    SemanticBindingId next_binding_id_{1};
    bool inside_function_{false};
    std::size_t loop_depth_{0};
    bool allow_range_expression_{false};
    Type current_return_type_{Type::Void};
    std::optional<SemanticArrayType> current_return_array_type_;
    std::uint32_t current_return_struct_type_id_{0};
    std::uint32_t current_return_enum_type_id_{0};
    std::optional<SemanticPointerType> current_return_pointer_type_;
    std::optional<std::uint32_t> current_return_pointer_parameter_index_;
    std::vector<const Expr*> null_pointer_literals_;
};

} // namespace

const char* semantic_type_name(const SemanticType type) noexcept {
    switch (type) {
    case SemanticType::Error: return "<error>";
    case SemanticType::Unknown: return "<unknown>";
    case SemanticType::Any: return "any";
    case SemanticType::Void: return "void";
    case SemanticType::Int: return "int";
    case SemanticType::Byte: return "byte";
    case SemanticType::Float: return "float";
    case SemanticType::Bool: return "bool";
    case SemanticType::String: return "string";
    case SemanticType::Struct: return "struct";
    case SemanticType::Enum: return "enum";
    case SemanticType::Array: return "array";
    case SemanticType::Range: return "range";
    case SemanticType::Pointer: return "pointer";
    }
    return "<unknown>";
}

bool SemanticResult::has_errors() const noexcept {
    return std::any_of(diagnostics.begin(), diagnostics.end(),
        [](const Diagnostic& diagnostic) {
            return diagnostic.severity == DiagnosticSeverity::Error;
        });
}

SemanticAnalyzer::SemanticAnalyzer(const SourceFile& source,
                                   const Program& program)
    : source_(source), program_(program) {}

SemanticResult SemanticAnalyzer::analyze() {
    return AnalyzerImpl(source_, program_).run();
}

} // namespace vex
