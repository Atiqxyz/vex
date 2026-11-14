#include "vex/semantic.hpp"

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

Type common_numeric_type(const Type left, const Type right) noexcept {
    if (left == Type::Error || right == Type::Error) return Type::Error;
    if (left == Type::Unknown || right == Type::Unknown) return Type::Unknown;
    if (!is_numeric(left) || !is_numeric(right)) return Type::Error;
    return left == Type::Float || right == Type::Float ? Type::Float : Type::Int;
}

Type compound_assignment_result(const TokenKind assignment,const Type left,const Type right) noexcept {
    switch(assignment) {
    case TokenKind::PlusEqual: case TokenKind::MinusEqual:
    case TokenKind::StarEqual: case TokenKind::SlashEqual:
        return common_numeric_type(left,right);
    case TokenKind::PercentEqual:
        if(left==Type::Error||right==Type::Error)return Type::Error;
        if(left==Type::Unknown||right==Type::Unknown)return Type::Unknown;
        return left==Type::Int&&right==Type::Int?Type::Int:Type::Error;
    default:return Type::Error;
    }
}

struct Symbol {
    SemanticBindingId id{0};
    Type type{Type::Unknown};
    bool immutable{false};
    bool is_function{false};
    std::vector<Type> parameters;
    std::vector<std::optional<SemanticArrayType>> parameter_array_types;
    SourceSpan declaration{};
    std::optional<SemanticArrayType> array_type;
    std::optional<SemanticArrayType> return_array_type;
    std::uint32_t struct_type_id{0};
    std::vector<std::uint32_t> parameter_struct_type_ids;
    std::uint32_t return_struct_type_id{0};
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
        collect_struct_types();
        collect_function_signatures();
        for (const StmtPtr& statement : program_.statements) {
            if (statement && !std::holds_alternative<StructDeclStmt>(statement->data))
                analyze_statement(*statement);
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
        std::uint32_t* struct_type_id = nullptr) {
        const std::string name = spelling(span);
        const std::size_t open = name.find('[');
        if (open != std::string::npos) {
            const std::size_t close = name.find(']', open + 1);
            if (close == std::string::npos || close + 1 != name.size()) {
                report("SEM003", "malformed fixed-array type annotation", span);
                return Type::Error;
            }
            std::string element_name = name.substr(0, open);
            const auto trim = [](std::string& value) {
                const auto whitespace = [](const char character) {
                    return character == ' ' || character == '\t' || character == '\r' || character == '\n';
                };
                while (!value.empty() && whitespace(value.front())) value.erase(value.begin());
                while (!value.empty() && whitespace(value.back())) value.pop_back();
            };
            trim(element_name);
            Type element_type = Type::Error;
            std::uint32_t element_struct_type_id = 0;
            if (element_name == "int") element_type = Type::Int;
            else if (element_name == "float") element_type = Type::Float;
            else if (element_name == "bool") element_type = Type::Bool;
            else if (element_name == "string") element_type = Type::String;
            else {
                const auto structure = struct_ids_.find(element_name);
                if (structure != struct_ids_.end()) {
                    element_type = Type::Struct;
                    element_struct_type_id = structure->second;
                } else {
                    report("SEM003", "array element type must be a primitive or a declared structure type", span);
                    return Type::Error;
                }
            }
            std::string length_text = name.substr(open + 1, close - open - 1);
            trim(length_text);
            std::uint32_t length = 0;
            const auto parsed = std::from_chars(length_text.data(),
                length_text.data() + length_text.size(), length);
            if (length_text.empty()) {
                if (array_shape)
                    *array_shape = SemanticArrayType{element_type, 0, element_struct_type_id};
                return Type::Array;
            }
            if (parsed.ec != std::errc{} ||
                parsed.ptr != length_text.data() + length_text.size() ||
                length == 0 || length > 65536u) {
                report("SEM041", "fixed-array lengths must be between 1 and 65,536; use [] for a dynamic array", span);
                return Type::Error;
            }
            if (array_shape)
                *array_shape = SemanticArrayType{element_type, length, element_struct_type_id};
            return Type::Array;
        }
        if (name == "int") return Type::Int;
        if (name == "float") return Type::Float;
        if (name == "bool") return Type::Bool;
        if (name == "string") return Type::String;
        const auto structure = struct_ids_.find(name);
        if (structure != struct_ids_.end()) {
            if (struct_type_id) *struct_type_id = structure->second;
            return Type::Struct;
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

        Symbol length;
        length.type = Type::Int;
        length.is_function = true;
        length.parameters = {Type::Array};
        length.parameter_array_types = {SemanticArrayType{Type::Unknown, 0}};
        length.declaration = {0, 0};
        scopes_.front().emplace("length", std::move(length));

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
    }

    void collect_struct_types() {
        std::vector<const StructDeclStmt*> declarations;
        for (const StmtPtr& statement : program_.statements) {
            if (!statement) continue;
            const auto* structure = std::get_if<StructDeclStmt>(&statement->data);
            if (!structure) continue;
            const std::string name = spelling(structure->name);
            if (name == "int" || name == "float" || name == "bool" ||
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
            declarations.push_back(structure);
        }
        for (const StructDeclStmt* declaration : declarations) {
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
                std::optional<SemanticArrayType> field_array_type;
                const Type field_type = field.type_annotation
                    ? annotation_type(*field.type_annotation, false,
                                      &field_array_type, &field_struct_id)
                    : Type::Error;
                if (!field.type_annotation || field_type == Type::Error ||
                    field_type == Type::Void || field_type == Type::Unknown ||
                    field_type == Type::Any ||
                    (field_type == Type::Array && !field_array_type)) {
                    report("SEM048", "structure fields require a primitive, a previously declared structure, or a one-dimensional array of either",
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
                     field_array_type->length != 0 &&
                     field_array_type->element_type == Type::Struct);
                const bool forward_dynamic_structure =
                    field_type == Type::Array && field_array_type &&
                    field_array_type->length == 0 &&
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
                    field_name, field_type, field_struct_id, field_array_type, field.name
                });
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
                                  &signature.return_struct_type_id)
                : Type::Void;

            for (const Parameter& parameter : function->parameters) {
                Type parameter_type = Type::Unknown;
                std::optional<SemanticArrayType> parameter_array_type;
                std::uint32_t parameter_struct_type_id = 0;
                if (parameter.type_annotation) {
                    parameter_type = annotation_type(
                        *parameter.type_annotation, false, &parameter_array_type,
                        &parameter_struct_type_id);
                } else {
                    report("SEM004",
                           "function parameters require an explicit type annotation",
                           parameter.name);
                }
                signature.parameters.push_back(parameter_type);
                signature.parameter_array_types.push_back(parameter_array_type);
                signature.parameter_struct_type_ids.push_back(parameter_struct_type_id);
            }

            const std::string name = spelling(function->name);
            const auto existing = scopes_.front().find(name);
            if (existing != scopes_.front().end()) {
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
            public_signature.return_struct_type_id = signature.return_struct_type_id;
            public_signature.declaration = function->name;
            model_.functions.emplace(function, std::move(public_signature));
            signatures_.emplace(function, std::move(signature));
        }
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
            declaration, symbol.array_type, symbol.struct_type_id
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
            case TokenKind::Integer: return Type::Int;
            case TokenKind::Float: return Type::Float;
            case TokenKind::String: return Type::String;
            case TokenKind::KwTrue:
            case TokenKind::KwFalse: return Type::Bool;
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
            return grouped_type;
        }

        if (const auto* array = std::get_if<ArrayLiteralExpr>(&expression.data)) {
            const auto contextual_shape = model_.array_expression_types.find(&expression);
            const bool has_context =
                contextual_shape != model_.array_expression_types.end() &&
                contextual_shape->second.element_type != Type::Unknown;
            if (array->elements.empty()) {
                if (has_context && contextual_shape->second.length == 0)
                    return Type::Array;
                report("SEM041", "empty array literals require an explicit dynamic-array type such as int[]",
                       expression.span);
                return Type::Error;
            }
            Type element_type = Type::Unknown;
            std::uint32_t element_struct_type_id = 0;
            for (const ExprPtr& element : array->elements) {
                const Type actual = element ? analyze_expression(*element) : Type::Error;
                if (actual == Type::Error || actual == Type::Void ||
                    actual == Type::Array || actual == Type::Any) {
                    report("SEM041", "array elements must use one supported primitive or structure type",
                           element ? element->span : expression.span);
                    return Type::Error;
                }
                std::uint32_t actual_struct_type_id = 0;
                if (actual == Type::Struct && element) {
                    const auto found = model_.struct_expression_types.find(element.get());
                    if (found != model_.struct_expression_types.end())
                        actual_struct_type_id = found->second;
                }
                if (element_type == Type::Unknown) {
                    element_type = actual;
                    element_struct_type_id = actual_struct_type_id;
                } else if (element_type == Type::Struct || actual == Type::Struct) {
                    if (element_type != actual ||
                        element_struct_type_id != actual_struct_type_id) {
                        report("SEM041", "all structure array elements must have the same nominal type",
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
            if (array->elements.size() > 65536u) {
                report("SEM041", "fixed arrays are limited to 65,536 elements",
                       expression.span);
                return Type::Error;
            }
            const SemanticArrayType inferred_shape{
                element_type, static_cast<std::uint32_t>(array->elements.size()),
                element_struct_type_id};
            if (has_context) {
                const SemanticArrayType expected = contextual_shape->second;
                const bool compatible_element =
                    expected.element_type == element_type ||
                    (expected.element_type == Type::Float && element_type == Type::Int);
                const bool compatible_struct =
                    expected.element_type != Type::Struct ||
                    expected.element_struct_type_id == element_struct_type_id;
                const bool compatible_length =
                    expected.length == 0 ||
                    expected.length == array->elements.size();
                // Compatible context supplies the declared shape (including
                // numeric widening and dynamic length). An incompatible context
                // must leave the inferred shape visible to the declaration,
                // call, or return checker so it can issue its established error.
                model_.array_expression_types[&expression] =
                    compatible_element && compatible_struct && compatible_length
                        ? expected : inferred_shape;
            } else {
                model_.array_expression_types[&expression] = inferred_shape;
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
                const auto actual_array = field.value
                    ? model_.array_expression_types.find(field.value.get())
                    : model_.array_expression_types.end();
                const bool array_shape_matches = expected->type != Type::Array ||
                    (expected->array_type && actual_array != model_.array_expression_types.end() &&
                     expected->array_type->element_type == actual_array->second.element_type &&
                     expected->array_type->length == actual_array->second.length &&
                     expected->array_type->element_struct_type_id ==
                         actual_array->second.element_struct_type_id);
                if (!is_assignable(expected->type, actual) ||
                    (expected->type == Type::Struct && expected->struct_type_id != actual_struct) ||
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
            if (shape->second.element_type == Type::Struct)
                model_.struct_expression_types[&expression] =
                    shape->second.element_struct_type_id;
            return shape->second.element_type;
        }

        if (const auto* unary = std::get_if<UnaryExpr>(&expression.data)) {
            if (!unary->operand) return Type::Error;
            const Type operand = analyze_expression(*unary->operand);
            if (operand == Type::Error || operand == Type::Unknown) return operand;
            if (unary->op == TokenKind::Bang) {
                if (operand != Type::Bool) {
                    report("SEM011", "operator '!' requires a bool operand; got " +
                           std::string(type_name(operand)), unary->operand->span);
                    return Type::Error;
                }
                return Type::Bool;
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
            if (left == Type::Unknown || right == Type::Unknown) return Type::Unknown;

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
            case TokenKind::Less:
            case TokenKind::LessEqual:
            case TokenKind::Greater:
            case TokenKind::GreaterEqual:
                if (!is_numeric(left) || !is_numeric(right)) {
                    report("SEM013", "comparison operators require numeric operands",
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
            if(assignment->target&&(std::holds_alternative<MemberExpr>(assignment->target->data)||
                                    std::holds_alternative<IndexExpr>(assignment->target->data))) {
                const Type target_type=analyze_expression(*assignment->target);
                const NameExpr* root=member_root_name(assignment->target.get());
                if(!root) {
                    report("SEM055","aggregate elements and fields can only be assigned through a named binding",assignment->target->span);
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
                const auto expected_array=model_.array_expression_types.find(assignment->target.get());
                if(target_type==Type::Array&&expected_array!=model_.array_expression_types.end()&&assignment->value&&
                   std::holds_alternative<ArrayLiteralExpr>(assignment->value->data))
                    model_.array_expression_types[assignment->value.get()]=expected_array->second;
                const Type actual=assignment->value?analyze_expression(*assignment->value):Type::Error;
                if(compound) {
                    const Type operation_type=compound_assignment_result(assignment->op,target_type,actual);
                    if(operation_type==Type::Error&&target_type!=Type::Error&&actual!=Type::Error&&
                       target_type!=Type::Unknown&&actual!=Type::Unknown)
                        report("SEM012","compound assignment requires compatible numeric operands; '%=' requires two ints",expression.span);
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
                     expected_array->second.element_type==actual_array->second.element_type&&
                     expected_array->second.length==actual_array->second.length&&
                     expected_array->second.element_struct_type_id==actual_array->second.element_struct_type_id);
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
            if(assignment->value&&symbol->type==Type::Array&&symbol->array_type&&symbol->array_type->length==0&&
               std::holds_alternative<ArrayLiteralExpr>(assignment->value->data))
                model_.array_expression_types[assignment->value.get()]=*symbol->array_type;
            const Type value=assignment->value?analyze_expression(*assignment->value):Type::Error;
            if(symbol->immutable)report("SEM017","cannot assign to immutable binding '"+identifier+"'",target->name);
            if(compound) {
                const Type operation_type=compound_assignment_result(assignment->op,symbol->type,value);
                if(operation_type==Type::Error&&symbol->type!=Type::Error&&value!=Type::Error&&
                   symbol->type!=Type::Unknown&&value!=Type::Unknown)
                    report("SEM012","compound assignment requires compatible numeric operands; '%=' requires two ints",expression.span);
                else if(operation_type!=Type::Error&&operation_type!=Type::Unknown&&!is_assignable(symbol->type,operation_type))
                    report("SEM018","compound assignment result cannot be stored in the target type",expression.span);
                return symbol->type;
            }
            if(symbol->type==Type::Array&&assignment->value) {
                const auto shape=model_.array_expression_types.find(assignment->value.get());
                if(symbol->array_type&&shape!=model_.array_expression_types.end()&&
                   (symbol->array_type->element_type!=shape->second.element_type||
                    symbol->array_type->length!=shape->second.length||
                    symbol->array_type->element_struct_type_id!=shape->second.element_struct_type_id))
                    report("SEM045","array assignment requires the same element type and length",assignment->value->span);
            }
            const std::uint32_t assigned_struct=assignment->value&&model_.struct_expression_types.count(assignment->value.get())
                ?model_.struct_expression_types.at(assignment->value.get()):0u;
            if(!is_assignable(symbol->type,value)||(symbol->type==Type::Struct&&symbol->struct_type_id!=assigned_struct))
                report("SEM018","cannot assign "+std::string(type_name(value))+" to "+type_name(symbol->type)+
                       " binding '"+identifier+"'",expression.span);
            if(symbol->type==Type::Struct)model_.struct_expression_types[&expression]=symbol->struct_type_id;
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
            argument_types.reserve(call->arguments.size());
            for (std::size_t index = 0; index < call->arguments.size(); ++index) {
                const ExprPtr& argument = call->arguments[index];
                if (function && argument &&
                    index < function->parameter_array_types.size() &&
                    function->parameter_array_types[index] &&
                    std::holds_alternative<ArrayLiteralExpr>(argument->data)) {
                    model_.array_expression_types[argument.get()] =
                        *function->parameter_array_types[index];
                }
                argument_types.push_back(argument
                    ? analyze_expression(*argument) : Type::Error);
                const auto structure = argument
                    ? model_.struct_expression_types.find(argument.get())
                    : model_.struct_expression_types.end();
                argument_struct_type_ids.push_back(
                    structure == model_.struct_expression_types.end() ? 0u : structure->second);
            }

            if (!function) return Type::Error;
            if (name) model_.resolved_calls[call] = spelling(name->name);

            const std::string builtin_name = name ? spelling(name->name) : std::string{};
            if (builtin_name == "length" || builtin_name == "push" ||
                builtin_name == "pop") {
                const std::size_t expected_count = builtin_name == "push" ? 2u : 1u;
                if (argument_types.size() != expected_count) {
                    report("SEM020", builtin_name + " expects " +
                           std::to_string(expected_count) + " argument(s)", expression.span);
                    return Type::Error;
                }
                if (argument_types[0] != Type::Array || !call->arguments[0]) {
                    report("SEM021", builtin_name +
                           " requires an array as its first argument", call->arguments[0]
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
                if (shape->second.length != 0) {
                    report("SEM021", builtin_name +
                           " requires a dynamic array declared with type []", call->arguments[0]->span);
                    return Type::Error;
                }
                if (const NameExpr* array_name =
                        member_root_name(call->arguments[0].get())) {
                    Symbol* array_symbol = lookup(spelling(array_name->name));
                    if (array_symbol && array_symbol->immutable) {
                        report("SEM017", builtin_name +
                               " cannot modify an array through immutable binding '" +
                               spelling(array_name->name) + "'", array_name->name);
                    }
                }
                if (builtin_name == "push") {
                    const std::uint32_t value_struct_type_id = call->arguments[1] &&
                        model_.struct_expression_types.find(call->arguments[1].get()) !=
                            model_.struct_expression_types.end()
                        ? model_.struct_expression_types.at(call->arguments[1].get()) : 0u;
                    if (!is_assignable(shape->second.element_type, argument_types[1]) ||
                        (shape->second.element_type == Type::Struct &&
                         shape->second.element_struct_type_id != value_struct_type_id)) {
                        report("SEM021", "push value has type " +
                               std::string(type_name(argument_types[1])) +
                               "; expected " + type_name(shape->second.element_type),
                               call->arguments[1]->span);
                        return Type::Error;
                    }
                    return Type::Void;
                }
                if (shape->second.element_type == Type::Struct)
                    model_.struct_expression_types[&expression] =
                        shape->second.element_struct_type_id;
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
                     function->parameter_struct_type_ids[index] != argument_struct_type_ids[index])) {
                    report("SEM021", "argument " + std::to_string(index + 1) +
                           " has type " + type_name(argument_types[index]) +
                           "; expected " + type_name(function->parameters[index]),
                           call->arguments[index]->span);
                    continue;
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
                        actual_shape->second.element_type != expected.element_type ||
                        actual_shape->second.length != expected.length ||
                        actual_shape->second.element_struct_type_id != expected.element_struct_type_id) {
                        report("SEM021", "array argument " +
                               std::to_string(index + 1) +
                               " does not match the required element type and fixed length",
                               call->arguments[index]->span);
                    }
                }
            }
            if (function->type == Type::Array && function->return_array_type)
                model_.array_expression_types[&expression] =
                    *function->return_array_type;
            if (function->type == Type::Struct && function->return_struct_type_id != 0)
                model_.struct_expression_types[&expression] = function->return_struct_type_id;
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
        if (variable.type_annotation) {
            declared = annotation_type(*variable.type_annotation, false,
                                       &declared_array_type, &declared_struct_type_id);
        }
        const auto* array_initializer = variable.initializer
            ? std::get_if<ArrayLiteralExpr>(&variable.initializer->data) : nullptr;
        if (array_initializer && declared == Type::Array && declared_array_type) {
            model_.array_expression_types[variable.initializer.get()] =
                *declared_array_type;
        }
        const Type inferred = variable.initializer
            ? analyze_expression(*variable.initializer) : Type::Error;
        if (!variable.type_annotation) declared = inferred;
        else {
            require_type(inferred, declared,
                         variable.initializer ? variable.initializer->span : variable.name,
                         "SEM022", "initializer type does not match variable annotation");
            const std::uint32_t inferred_struct = variable.initializer &&
                model_.struct_expression_types.find(variable.initializer.get()) !=
                    model_.struct_expression_types.end()
                ? model_.struct_expression_types.at(variable.initializer.get()) : 0u;
            if (declared == Type::Struct && inferred == Type::Struct &&
                declared_struct_type_id != inferred_struct)
                report("SEM022", "initializer structure type does not match variable annotation",
                       variable.initializer ? variable.initializer->span : variable.name);
        }
        if (declared == Type::Void) {
            report("SEM023", "variables cannot have type void", variable.name);
            declared = Type::Error;
        }
        Symbol symbol;
        symbol.type = declared;
        if (declared == Type::Array) symbol.array_type = declared_array_type;
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
                if (declared_array_type &&
                    (declared_array_type->element_type != shape->second.element_type ||
                     declared_array_type->length != shape->second.length ||
                     declared_array_type->element_struct_type_id != shape->second.element_struct_type_id)) {
                    report("SEM045",
                           "array initializer does not match the annotated element type and fixed length",
                           variable.initializer->span);
                }
                if (declared_array_type && declared_array_type->length == 0 &&
                    shape->second.element_type != Type::Unknown) {
                    model_.array_expression_types[variable.initializer.get()] =
                        *declared_array_type;
                    symbol.array_type = *declared_array_type;
                } else if (!symbol.array_type) {
                    symbol.array_type = shape->second;
                }
            }
        }
        symbol.immutable = variable.immutable;
        symbol.declaration = variable.name;
        SemanticBindingId binding_id = 0;
        if (define(spelling(variable.name), std::move(symbol),
                   variable.name, &binding_id)) {
            model_.variable_bindings[&variable] = binding_id;
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
            symbol.declaration = parameter.name;
            SemanticBindingId binding_id = 0;
            if (define(spelling(parameter.name), std::move(symbol),
                       parameter.name, &binding_id)) {
                model_.parameter_bindings[&parameter] = binding_id;
            }
        }

        const bool was_in_function = inside_function_;
        const Type previous_return = current_return_type_;
        const auto previous_return_array_type = current_return_array_type_;
        const std::uint32_t previous_return_struct_type_id = current_return_struct_type_id_;
        inside_function_ = true;
        current_return_type_ = signature.type;
        current_return_array_type_ = signature.return_array_type;
        current_return_struct_type_id_ = signature.return_struct_type_id;

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
                    if (actual == Type::Array && current_return_array_type_) {
                        const auto actual_shape =
                            model_.array_expression_types.find(returned->value.get());
                        if (actual_shape == model_.array_expression_types.end() ||
                            actual_shape->second.element_type != current_return_array_type_->element_type ||
                            actual_shape->second.length != current_return_array_type_->length ||
                            actual_shape->second.element_struct_type_id !=
                                current_return_array_type_->element_struct_type_id) {
                            report("SEM028",
                                   "returned array does not match the function's annotated element type and fixed length",
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
                    iteration_type = array_type->element_type;
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
    SemanticModel model_;
    SemanticBindingId next_binding_id_{1};
    bool inside_function_{false};
    std::size_t loop_depth_{0};
    bool allow_range_expression_{false};
    Type current_return_type_{Type::Void};
    std::optional<SemanticArrayType> current_return_array_type_;
    std::uint32_t current_return_struct_type_id_{0};
};

} // namespace

const char* semantic_type_name(const SemanticType type) noexcept {
    switch (type) {
    case SemanticType::Error: return "<error>";
    case SemanticType::Unknown: return "<unknown>";
    case SemanticType::Any: return "any";
    case SemanticType::Void: return "void";
    case SemanticType::Int: return "int";
    case SemanticType::Float: return "float";
    case SemanticType::Bool: return "bool";
    case SemanticType::String: return "string";
    case SemanticType::Struct: return "struct";
    case SemanticType::Array: return "array";
    case SemanticType::Range: return "range";
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
