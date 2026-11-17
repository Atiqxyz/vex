#include "vex/c_backend.hpp"
#include "vex/stack_policy.hpp"
#include "vex/target_layout.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <set>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace vex {
namespace {

const char* c_type(const IRType type) noexcept {
    switch (type) {
    case IRType::Void: return "void";
    case IRType::Int: return "int64_t";
    case IRType::Byte: return "uint8_t";
    case IRType::Float: return "double";
    case IRType::Bool: return "bool";
    case IRType::String: return "vex_string";
    case IRType::Array: return "void";
    case IRType::Struct: return "void";
    case IRType::Enum: return "void";
    }
    return "void";
}

std::string struct_type_name(const std::uint32_t id) {
    return "vex_struct_" + std::to_string(id);
}

std::string struct_field_name(const std::uint32_t index) {
    return "vex_field_" + std::to_string(index);
}

std::string enum_type_name(const std::uint32_t id) {
    return "vex_enum_" + std::to_string(id);
}

std::string array_type_name(const IRValue& array) {
    const std::string element = array.element_type == IRType::Int ? "int" :
        array.element_type == IRType::Float ? "float" :
        array.element_type == IRType::Bool ? "bool" :
        array.element_type == IRType::Byte ? "byte" :
        array.element_type == IRType::String ? "string" :
        array.element_type == IRType::Enum ? "enum_" + std::to_string(array.enum_type_id) :
        "struct_" + std::to_string(array.element_struct_type_id);
    std::string name = "vex_array_" + element;
    for (const std::uint32_t dimension : ir_array_dimensions(array)) {
        name += "_" + (dimension == 0u ? std::string("dynamic")
                                      : std::to_string(dimension));
    }
    return name;
}

IRValue array_element_value(const IRValue& array) {
    return ir_array_element_value(array);
}

std::string c_type(const IRValue& value) {
    if (value.type == IRType::Struct) return struct_type_name(value.struct_type_id);
    if (value.type == IRType::Enum) return enum_type_name(value.enum_type_id);
    if (value.type != IRType::Array) return c_type(value.type);
    return ir_array_dimensions(value).front() == 0u
        ? "vex_array *" : array_type_name(value);
}

std::string c_return_type(const IRFunction& function) {
    if (function.return_type == IRType::Struct)
        return struct_type_name(function.return_struct_type_id);
    if (function.return_type == IRType::Enum)
        return enum_type_name(function.return_enum_type_id);
    if (function.return_type != IRType::Array) return c_type(function.return_type);
    std::vector<std::uint32_t> dimensions = function.return_array_dimensions;
    if (dimensions.empty()) dimensions.push_back(function.return_array_length);
    return c_type(ir_make_array_value(function.return_element_type,
        std::move(dimensions), function.return_element_struct_type_id, function.span,
        function.return_element_enum_type_id));
}

std::string value_name(const IRValueId id) {
    return "vex_v_" + std::to_string(id);
}

std::string block_name(const IRBlockId id) {
    return "vex_bb_" + std::to_string(id);
}

std::string function_name(const std::string& name) {
    return "vex_fn_" + name;
}

bool is_vex_identifier(const std::string& name) {
    if (name.empty()) return false;
    const auto is_start = [](const unsigned char value) {
        return (value >= 'a' && value <= 'z') ||
               (value >= 'A' && value <= 'Z') || value == '_';
    };
    const auto is_continue = [&](const unsigned char value) {
        return is_start(value) || (value >= '0' && value <= '9');
    };
    if (!is_start(static_cast<unsigned char>(name.front()))) return false;
    return std::all_of(name.begin() + 1, name.end(),
        [&](const char value) {
            return is_continue(static_cast<unsigned char>(value));
        });
}

std::string edge_name(const IRBlockId source, const char arm,
                      const std::size_t index) {
    return "vex_edge_b" + std::to_string(source) + "_" + arm + "_" +
           std::to_string(index);
}

std::optional<std::string> decode_string_literal(const std::string_view literal) {
    if (literal.size() < 2 || literal.front() != '"' || literal.back() != '"') {
        return std::nullopt;
    }

    std::string decoded;
    decoded.reserve(literal.size() - 2);
    for (std::size_t index = 1; index + 1 < literal.size(); ++index) {
        const char current = literal[index];
        if (current != '\\') {
            decoded.push_back(current);
            continue;
        }

        ++index;
        if (index >= literal.size() - 1) return std::nullopt;
        switch (literal[index]) {
        case 'n': decoded.push_back('\n'); break;
        case 'r': decoded.push_back('\r'); break;
        case 't': decoded.push_back('\t'); break;
        case '0': decoded.push_back('\0'); break;
        case '\\': decoded.push_back('\\'); break;
        case '"': decoded.push_back('"'); break;
        case '\'': decoded.push_back('\''); break;
        default: return std::nullopt;
        }
    }
    return decoded;
}

std::string escape_c_string(const std::string_view bytes) {
    std::string escaped;
    escaped.reserve(bytes.size());
    for (const unsigned char byte : bytes) {
        if (byte == static_cast<unsigned char>('\\')) {
            escaped += "\\\\";
        } else if (byte == static_cast<unsigned char>('"')) {
            escaped += "\\\"";
        } else if (byte >= 0x20u && byte <= 0x7eu) {
            escaped.push_back(static_cast<char>(byte));
        } else {
            escaped.push_back('\\');
            escaped.push_back(static_cast<char>('0' + ((byte >> 6u) & 0x07u)));
            escaped.push_back(static_cast<char>('0' + ((byte >> 3u) & 0x07u)));
            escaped.push_back(static_cast<char>('0' + (byte & 0x07u)));
        }
    }
    return escaped;
}

class CEmitter {
public:
    explicit CEmitter(const IRModule& module) : module_(module) {}

    CBackendResult run() {
        CBackendResult result;
        const IRVerificationResult verified = IRVerifier{}.verify(module_);
        result.diagnostics = verified.diagnostics;
        if (verified.has_errors()) return result;

        validate_entry_point();
        if (has_errors(diagnostics_)) {
            result.diagnostics = std::move(diagnostics_);
            return result;
        }

        for (const IRFunction& function : module_.functions) {
            if (!is_vex_identifier(function.name)) {
                report("CG007", "function name cannot be represented as a VEX identifier",
                       function.span);
            }
        }
        if (has_errors(diagnostics_)) {
            result.diagnostics = std::move(diagnostics_);
            return result;
        }

        emit_preamble();
        emit_enum_types();
        emit_struct_types();
        emit_array_types();
        emit_target_layout_assertions();
        emit_nested_fixed_stack_assertions();
        emit_runtime();
        emit_prototypes();
        for (const IRFunction& function : module_.functions) {
            emit_function(function);
        }
        emit_entry_wrapper();

        result.source = output_.str();
        result.diagnostics = std::move(diagnostics_);
        if (has_errors(result.diagnostics)) result.source.clear();
        return result;
    }

private:
    static bool has_errors(const std::vector<Diagnostic>& diagnostics) {
        return std::any_of(diagnostics.begin(), diagnostics.end(),
            [](const Diagnostic& diagnostic) {
                return diagnostic.severity == DiagnosticSeverity::Error;
            });
    }

    void report(const char* code, std::string message, const SourceSpan span = {}) {
        diagnostics_.push_back(Diagnostic{
            DiagnosticSeverity::Error, code, std::move(message), span
        });
    }

    void validate_entry_point() {
        for (const IRFunction& function : module_.functions) {
            if (function.name != "main") continue;
            entry_ = &function;
            if (!function.parameters.empty()) {
                report("CG001", "the initial C backend requires main() to have no parameters",
                       function.span);
            }
            if (function.return_type != IRType::Void &&
                function.return_type != IRType::Int) {
                report("CG002", "main must return void or int in the initial C backend",
                       function.span);
            }
        }
    }

    void emit_preamble() {
        output_
            << "/* Generated by the VEX C backend. Do not edit by hand. */\n"
            << "#include <inttypes.h>\n"
            << "#include <math.h>\n"
            << "#include <stddef.h>\n"
            << "#include <stdbool.h>\n"
            << "#include <stdint.h>\n"
            << "#include <stdio.h>\n"
            << "#include <stdlib.h>\n"
            << "#include <string.h>\n\n"
            << "typedef struct vex_string {\n"
            << "    const unsigned char *data;\n"
            << "    size_t length;\n"
            << "} vex_string;\n"
            << "typedef struct vex_array {\n"
            << "    void *data;\n"
            << "    size_t length;\n"
            << "    size_t capacity;\n"
            << "    size_t element_size;\n"
            << "    struct vex_array *next;\n"
            << "} vex_array;\n"
            << "typedef struct vex_string_allocation {\n"
            << "    struct vex_string_allocation *next;\n"
            << "    unsigned char data[];\n"
            << "} vex_string_allocation;\n"
            << "static vex_array *vex_array_head = NULL;\n"
            << "static vex_string_allocation *vex_string_head = NULL;\n\n";
    }

    void emit_array_typedef(const IRValue& array) {
        const std::vector<std::uint32_t> dimensions = ir_array_dimensions(array);
        if (dimensions.empty()) return;
        const IRValue element = array_element_value(array);
        if (element.type == IRType::Array) emit_array_typedef(element);
        if (dimensions.front() == 0u) return;

        const std::string name = array_type_name(array);
        if (!emitted_array_shapes_.insert(name).second) return;
        emitted_array_types_.emplace(name, array);
        output_ << "typedef struct " << name << " {\n"
                << "    " << c_type(element) << " data[" << dimensions.front() << "];\n"
                << "} " << name << ";\n";
    }

    void emit_enum_types() {
        const TargetDataLayout layout;
        for (const IREnumType& enumeration : module_.enum_types) {
            output_ << "typedef struct " << enum_type_name(enumeration.id)
                    << " " << enum_type_name(enumeration.id) << ";\\n";
        }
        if (!module_.enum_types.empty()) output_ << '\\n';
        for (const IREnumType& enumeration : module_.enum_types) {
            std::string error;
            const auto enum_layout = layout.layout_of_enum(enumeration.id, module_, &error);
            if (!enum_layout) {
                report("CG011", "cannot compute target layout for enum " +
                       enumeration.name + ": " + error);
                continue;
            }
            output_ << "struct " << enum_type_name(enumeration.id) << " {\\n"
                    << "    uint32_t tag;\\n";
            if (enum_layout->payload_size != 0u) {
                output_ << "    _Alignas(" << enum_layout->payload_alignment
                        << ") unsigned char payload[" << enum_layout->payload_size
                        << "u];\\n";
            }
            output_ << "};\\n";
        }
        if (!module_.enum_types.empty()) output_ << '\\n';
    }

    void emit_struct_types() {
        if (module_.struct_types.empty()) return;
        for (const IRStructType& structure : module_.struct_types) {
            output_ << "typedef struct " << struct_type_name(structure.id)
                    << " " << struct_type_name(structure.id) << ";\n";
        }
        output_ << '\n';
        // Structure fields can contain only earlier declared structures by value.
        // Emit an array wrapper after its element structure is complete and before
        // the containing structure uses that wrapper.
        for (const IRStructType& structure : module_.struct_types) {
            for (const IRStructField& field : structure.fields) {
                if (field.type == IRType::Array)
                    emit_array_typedef(ir_struct_field_value(field));
            }
            output_ << "struct " << struct_type_name(structure.id) << " {\n";
            for (std::size_t index = 0; index < structure.fields.size(); ++index) {
                const IRStructField& field = structure.fields[index];
                IRValue value = ir_struct_field_value(field);
                output_ << "    " << c_type(value) << ' '
                        << struct_field_name(static_cast<std::uint32_t>(index)) << ";\n";
            }
            output_ << "};\n";
        }
        output_ << '\n';
    }

    void emit_array_types() {
        const std::size_t before = emitted_array_shapes_.size();
        for (const IRFunction& function : module_.functions) {
            if (function.return_type == IRType::Array) {
                std::vector<std::uint32_t> dimensions = function.return_array_dimensions;
                if (dimensions.empty()) dimensions.push_back(function.return_array_length);
                emit_array_typedef(ir_make_array_value(function.return_element_type,
                    std::move(dimensions), function.return_element_struct_type_id,
                    function.span, function.return_element_enum_type_id));
            }
            for (const IRFunctionParameter& parameter : function.parameters)
                if (parameter.value.type == IRType::Array)
                    emit_array_typedef(parameter.value);
            for (const IRBlock& block : function.blocks) {
                for (const IRValue& parameter : block.parameters)
                    if (parameter.type == IRType::Array) emit_array_typedef(parameter);
                for (const IRInstruction& instruction : block.instructions)
                    if (instruction.result && instruction.result->type == IRType::Array)
                        emit_array_typedef(*instruction.result);
            }
        }
        if (emitted_array_shapes_.size() != before) output_ << '\n';
    }

    void emit_target_layout_assertions() {
        const TargetDataLayout layout;
        std::string error;
        IRValue string_value;
        string_value.type = IRType::String;
        const auto string_layout = layout.layout_of(string_value, module_, &error);
        if (!string_layout) {
            report("CG011", "cannot compute target string layout: " + error);
            return;
        }
        output_ << "_Static_assert(sizeof(vex_string) == " << string_layout->size
                << "u, \"VEX string size disagrees with target data layout\");\n"
                << "_Static_assert(_Alignof(vex_string) == " << string_layout->alignment
                << "u, \"VEX string alignment disagrees with target data layout\");\n";

        for (const auto& [name, value] : emitted_array_types_) {
            error.clear();
            const auto array_layout = layout.layout_of(value, module_, &error);
            if (!array_layout) {
                report("CG011", "cannot compute target layout for " + name + ": " + error);
                continue;
            }
            output_ << "_Static_assert(sizeof(" << name << ") == " << array_layout->size
                    << "u, \"VEX array size disagrees with target data layout\");\n"
                    << "_Static_assert(_Alignof(" << name << ") == " << array_layout->alignment
                    << "u, \"VEX array alignment disagrees with target data layout\");\n";
        }

        for (const IRStructType& structure : module_.struct_types) {
            error.clear();
            const auto struct_layout = layout.layout_of_struct(structure.id, module_, &error);
            if (!struct_layout) {
                report("CG011", "cannot compute target layout for structure " +
                       structure.name + ": " + error);
                continue;
            }
            const std::string type = struct_type_name(structure.id);
            output_ << "_Static_assert(sizeof(" << type << ") == " << struct_layout->size
                    << "u, \"VEX structure size disagrees with target data layout\");\n"
                    << "_Static_assert(_Alignof(" << type << ") == " << struct_layout->alignment
                    << "u, \"VEX structure alignment disagrees with target data layout\");\n";
            for (std::size_t index = 0; index < struct_layout->fields.size(); ++index) {
                output_ << "_Static_assert(offsetof(" << type << ", "
                        << struct_field_name(static_cast<std::uint32_t>(index)) << ") == "
                        << struct_layout->fields[index].offset
                        << "u, \"VEX structure field offset disagrees with target data layout\");\n";
            }
        }
        output_ << '\n';
    }

    void emit_nested_fixed_stack_assertions() {
        std::size_t function_index = 0;
        for (const IRFunction& function : module_.functions) {
            const auto values = ir_function_nested_fixed_stack_values(function, module_);
            if (values.empty()) { ++function_index; continue; }
            std::set<std::string> asserted_types;
            for (const IRValue& value : values) {
                const std::string type = c_type(value);
                if (!asserted_types.insert(type).second) continue;
                output_ << "_Static_assert(sizeof(" << type << ") <= "
                        << max_nested_fixed_aggregate_bytes
                        << "u, \"VEX nested fixed aggregate exceeds the 256 KiB object limit\");\n";
            }
            const std::string frame = "vex_nested_fixed_frame_" + std::to_string(function_index);
            output_ << "typedef struct " << frame << " {\n";
            for (std::size_t i = 0; i < values.size(); ++i)
                output_ << "    " << c_type(values[i]) << " vex_slot_" << i << ";\n";
            output_ << "} " << frame << ";\n"
                    << "_Static_assert(sizeof(" << frame << ") <= "
                    << max_function_nested_fixed_stack_bytes
                    << "u, \"VEX function exceeds the 512 KiB nested fixed aggregate stack budget\");\n\n";
            ++function_index;
        }
    }

    void emit_runtime() {
        output_
            << "static void vex_runtime_trap(const char *message) {\n"
            << "    (void)fprintf(stderr, \"VEX runtime error: %s\\n\", message);\n"
            << "    exit(EXIT_FAILURE);\n"
            << "}\n\n"
            << "static uint8_t vex_checked_byte(int64_t value) {\n"
            << "    if (value < 0 || value > 255)\n"
            << "        vex_runtime_trap(\"integer is outside byte range 0 through 255\");\n"
            << "    return (uint8_t)value;\n"
            << "}\n\n"
            << "static size_t vex_checked_array_index(int64_t index, size_t length) {\n"
            << "    if (index < 0 || (uint64_t)index >= (uint64_t)length)\n"
            << "        vex_runtime_trap(\"array index is out of bounds\");\n"
            << "    return (size_t)index;\n"
            << "}\n\n"
            << "static vex_array *vex_array_new(size_t element_size) {\n"
            << "    if (element_size == 0u) vex_runtime_trap(\"invalid dynamic array element size\");\n"
            << "    vex_array *array = (vex_array *)calloc(1u, sizeof(vex_array));\n"
            << "    if (array == NULL) vex_runtime_trap(\"dynamic array allocation failed\");\n"
            << "    array->element_size = element_size;\n"
            << "    array->next = vex_array_head;\n"
            << "    vex_array_head = array;\n"
            << "    return array;\n"
            << "}\n\n"
            << "static void vex_array_check(vex_array *array, size_t element_size) {\n"
            << "    if (array == NULL || element_size == 0u || array->element_size != element_size)\n"
            << "        vex_runtime_trap(\"dynamic array element type mismatch\");\n"
            << "}\n\n"
            << "static int64_t vex_array_capacity(vex_array *array, size_t element_size) {\n"
            << "    vex_array_check(array, element_size);\n"
            << "    if ((uint64_t)array->capacity > (uint64_t)INT64_MAX)\n"
            << "        vex_runtime_trap(\"dynamic array capacity exceeds int range\");\n"
            << "    return (int64_t)array->capacity;\n"
            << "}\n\n"
            << "static void vex_array_reserve(vex_array *array, int64_t requested, size_t element_size) {\n"
            << "    vex_array_check(array, element_size);\n"
            << "    if (requested < 0) vex_runtime_trap(\"dynamic array capacity cannot be negative\");\n"
            << "    const uint64_t requested_count = (uint64_t)requested;\n"
            << "    if (requested_count <= (uint64_t)array->capacity) return;\n"
            << "    if (requested_count > (uint64_t)(SIZE_MAX / element_size))\n"
            << "        vex_runtime_trap(\"dynamic array reservation size overflow\");\n"
            << "    void *next_data = realloc(array->data, (size_t)requested_count * element_size);\n"
            << "    if (next_data == NULL) vex_runtime_trap(\"dynamic array reservation failed\");\n"
            << "    array->data = next_data;\n"
            << "    array->capacity = (size_t)requested_count;\n"
            << "}\n\n"
            << "static int64_t vex_array_length(vex_array *array) {\n"
            << "    if (array == NULL || array->length > (size_t)INT64_MAX)\n"
            << "        vex_runtime_trap(\"invalid dynamic array length\");\n"
            << "    return (int64_t)array->length;\n"
            << "}\n\n"
            << "static void *vex_array_get(vex_array *array, int64_t index, size_t element_size) {\n"
            << "    vex_array_check(array, element_size);\n"
            << "    if (index < 0 || (uint64_t)index >= (uint64_t)array->length)\n"
            << "        vex_runtime_trap(\"dynamic array index is out of bounds\");\n"
            << "    return (unsigned char *)array->data + (size_t)index * element_size;\n"
            << "}\n\n"
            << "static void vex_array_set(vex_array *array, int64_t index, const void *value, size_t element_size) {\n"
            << "    (void)memcpy(vex_array_get(array, index, element_size), value, element_size);\n"
            << "}\n\n"
            << "static void vex_array_push(vex_array *array, const void *value, size_t element_size) {\n"
            << "    vex_array_check(array, element_size);\n"
            << "    if (array->length >= (size_t)INT64_MAX) vex_runtime_trap(\"dynamic array length overflow\");\n"
            << "    if (array->length == array->capacity) {\n"
            << "        size_t next_capacity = array->capacity == 0u ? 4u : array->capacity;\n"
            << "        if (array->capacity != 0u) {\n"
            << "            if (next_capacity > (size_t)INT64_MAX / 2u) next_capacity = array->length + 1u;\n"
            << "            else next_capacity *= 2u;\n"
            << "        }\n"
            << "        if (next_capacity < array->length + 1u) next_capacity = array->length + 1u;\n"
            << "        if (next_capacity > SIZE_MAX / element_size) vex_runtime_trap(\"dynamic array allocation size overflow\");\n"
            << "        void *next_data = realloc(array->data, next_capacity * element_size);\n"
            << "        if (next_data == NULL) vex_runtime_trap(\"dynamic array growth failed\");\n"
            << "        array->data = next_data;\n"
            << "        array->capacity = next_capacity;\n"
            << "    }\n"
            << "    (void)memcpy((unsigned char *)array->data + array->length * element_size, value, element_size);\n"
            << "    ++array->length;\n"
            << "}\n\n"
            << "static void vex_array_pop(vex_array *array, void *result, size_t element_size) {\n"
            << "    vex_array_check(array, element_size);\n"
            << "    if (array->length == 0u) vex_runtime_trap(\"cannot pop an empty dynamic array\");\n"
            << "    --array->length;\n"
            << "    (void)memcpy(result, (unsigned char *)array->data + array->length * element_size, element_size);\n"
            << "}\n\n"
            << "static vex_array *vex_byte_slice(vex_array *source, int64_t start, int64_t count) {\n"
            << "    vex_array_check(source, 1u);\n"
            << "    if (start < 0 || count < 0 || (uint64_t)start > (uint64_t)source->length ||\n"
            << "        (uint64_t)count > (uint64_t)source->length - (uint64_t)start)\n"
            << "        vex_runtime_trap(\"byte slice is out of bounds\");\n"
            << "    vex_array *result = vex_array_new(1u);\n"
            << "    if (count != 0) {\n"
            << "        vex_array_reserve(result, count, 1u);\n"
            << "        (void)memcpy(result->data, (const unsigned char *)source->data + (size_t)start, (size_t)count);\n"
            << "        result->length = (size_t)count;\n"
            << "    }\n"
            << "    return result;\n"
            << "}\n\n"
            << "static int64_t vex_byte_read(vex_array *array, int64_t offset, size_t width, bool little_endian) {\n"
            << "    vex_array_check(array, 1u);\n"
            << "    if (offset < 0 || (uint64_t)offset > (uint64_t)array->length ||\n"
            << "        width > (uint64_t)array->length - (uint64_t)offset)\n"
            << "        vex_runtime_trap(\"binary read is out of bounds\");\n"
            << "    const unsigned char *bytes = (const unsigned char *)array->data + (size_t)offset;\n"
            << "    uint64_t value = 0u;\n"
            << "    for (size_t index = 0u; index < width; ++index) {\n"
            << "        const size_t shift_index = little_endian ? index : width - 1u - index;\n"
            << "        value |= (uint64_t)bytes[index] << (shift_index * 8u);\n"
            << "    }\n"
            << "    return (int64_t)value;\n"
            << "}\n\n"
            << "static void vex_byte_write(vex_array *array, int64_t offset, int64_t value, size_t width, bool little_endian) {\n"
            << "    vex_array_check(array, 1u);\n"
            << "    const uint64_t maximum = width == 2u ? UINT64_C(65535) : UINT64_C(4294967295);\n"
            << "    if (value < 0 || (uint64_t)value > maximum)\n"
            << "        vex_runtime_trap(\"binary integer value is outside the selected unsigned width\");\n"
            << "    if (offset < 0 || (uint64_t)offset > (uint64_t)array->length ||\n"
            << "        width > (uint64_t)array->length - (uint64_t)offset)\n"
            << "        vex_runtime_trap(\"binary write is out of bounds\");\n"
            << "    unsigned char *bytes = (unsigned char *)array->data + (size_t)offset;\n"
            << "    const uint64_t bits = (uint64_t)value;\n"
            << "    for (size_t index = 0u; index < width; ++index) {\n"
            << "        const size_t shift_index = little_endian ? index : width - 1u - index;\n"
            << "        bytes[index] = (unsigned char)(bits >> (shift_index * 8u));\n"
            << "    }\n"
            << "}\n\n"
            << "static bool vex_valid_file_path(vex_string path) {\n"
            << "    return path.length != 0u && path.data != NULL &&\n"
            << "        memchr(path.data, 0, path.length) == NULL;\n"
            << "}\n\n"
            << "static int64_t vex_file_read(vex_string path, vex_array *array) {\n"
            << "    vex_array_check(array, 1u);\n"
            << "    if (!vex_valid_file_path(path)) return -1;\n"
            << "    FILE *file = fopen((const char *)path.data, \"rb\");\n"
            << "    if (file == NULL) return -1;\n"
            << "    const size_t original_length = array->length;\n"
            << "    for (;;) {\n"
            << "        if (array->length > (size_t)INT64_MAX - 4096u) {\n"
            << "            array->length = original_length; (void)fclose(file); return -1;\n"
            << "        }\n"
            << "        const size_t required = array->length + 4096u;\n"
            << "        size_t next_capacity = array->capacity;\n"
            << "        if (next_capacity < required) {\n"
            << "            if (next_capacity == 0u) next_capacity = 4096u;\n"
            << "            while (next_capacity < required) {\n"
            << "                if (next_capacity > SIZE_MAX / 2u) { next_capacity = required; break; }\n"
            << "                next_capacity *= 2u;\n"
            << "            }\n"
            << "        }\n"
            << "        if (next_capacity > (size_t)INT64_MAX) {\n"
            << "            array->length = original_length; (void)fclose(file); return -1;\n"
            << "        }\n"
            << "        vex_array_reserve(array, (int64_t)next_capacity, 1u);\n"
            << "        unsigned char *destination = (unsigned char *)array->data + array->length;\n"
            << "        const size_t count = fread(destination, 1u, 4096u, file);\n"
            << "        array->length += count;\n"
            << "        if (count < 4096u) {\n"
            << "            if (ferror(file)) { array->length = original_length; (void)fclose(file); return -1; }\n"
            << "            break;\n"
            << "        }\n"
            << "    }\n"
            << "    if (fclose(file) != 0) { array->length = original_length; return -1; }\n"
            << "    return (int64_t)(array->length - original_length);\n"
            << "}\n\n"
            << "static int64_t vex_file_write(vex_string path, vex_array *array) {\n"
            << "    vex_array_check(array, 1u);\n"
            << "    if (!vex_valid_file_path(path) || array->length > (size_t)INT64_MAX) return -1;\n"
            << "    FILE *file = fopen((const char *)path.data, \"wb\");\n"
            << "    if (file == NULL) return -1;\n"
            << "    const unsigned char *source = (const unsigned char *)array->data;\n"
            << "    size_t offset = 0u;\n"
            << "    while (offset < array->length) {\n"
            << "        const size_t count = fwrite(source + offset, 1u, array->length - offset, file);\n"
            << "        if (count == 0u) { (void)fclose(file); return -1; }\n"
            << "        offset += count;\n"
            << "    }\n"
            << "    if (fclose(file) != 0) return -1;\n"
            << "    return (int64_t)array->length;\n"
            << "}\n\n"
            << "static void vex_array_cleanup(void) {\n"
            << "    while (vex_array_head != NULL) {\n"
            << "        vex_array *array = vex_array_head;\n"
            << "        vex_array_head = array->next;\n"
            << "        free(array->data);\n"
            << "        free(array);\n"
            << "    }\n"
            << "}\n\n"
            << "static int64_t vex_i64_add(int64_t left, int64_t right) {\n"
            << "    if ((right > 0 && left > INT64_MAX - right) ||\n"
            << "        (right < 0 && left < INT64_MIN - right)) {\n"
            << "        vex_runtime_trap(\"signed integer overflow in addition\");\n"
            << "    }\n"
            << "    return left + right;\n"
            << "}\n\n"
            << "static int64_t vex_i64_sub(int64_t left, int64_t right) {\n"
            << "    if ((right > 0 && left < INT64_MIN + right) ||\n"
            << "        (right < 0 && left > INT64_MAX + right)) {\n"
            << "        vex_runtime_trap(\"signed integer overflow in subtraction\");\n"
            << "    }\n"
            << "    return left - right;\n"
            << "}\n\n"
            << "static int64_t vex_i64_mul(int64_t left, int64_t right) {\n"
            << "    if (left == 0 || right == 0) return 0;\n"
            << "    if ((left == INT64_MIN && right == -1) ||\n"
            << "        (right == INT64_MIN && left == -1)) {\n"
            << "        vex_runtime_trap(\"signed integer overflow in multiplication\");\n"
            << "    }\n"
            << "    if (left > 0) {\n"
            << "        if ((right > 0 && left > INT64_MAX / right) ||\n"
            << "            (right < 0 && right < INT64_MIN / left)) {\n"
            << "            vex_runtime_trap(\"signed integer overflow in multiplication\");\n"
            << "        }\n"
            << "    } else {\n"
            << "        if ((right > 0 && left < INT64_MIN / right) ||\n"
            << "            (right < 0 && right < INT64_MAX / left)) {\n"
            << "            vex_runtime_trap(\"signed integer overflow in multiplication\");\n"
            << "        }\n"
            << "    }\n"
            << "    return left * right;\n"
            << "}\n\n"
            << "static int64_t vex_i64_div(int64_t left, int64_t right) {\n"
            << "    if (right == 0) vex_runtime_trap(\"division by zero\");\n"
            << "    if (left == INT64_MIN && right == -1)\n"
            << "        vex_runtime_trap(\"signed integer overflow in division\");\n"
            << "    return left / right;\n"
            << "}\n\n"
            << "static int64_t vex_i64_rem(int64_t left, int64_t right) {\n"
            << "    if (right == 0) vex_runtime_trap(\"remainder by zero\");\n"
            << "    if (left == INT64_MIN && right == -1) return 0;\n"
            << "    return left % right;\n"
            << "}\n\n"
            << "static int64_t vex_i64_neg(int64_t value) {\n"
            << "    if (value == INT64_MIN)\n"
            << "        vex_runtime_trap(\"signed integer overflow in negation\");\n"
            << "    return -value;\n"
            << "}\n\n"
            << "static int64_t vex_i64_from_bits(uint64_t bits) {\n"
            << "    int64_t value; (void)memcpy(&value, &bits, sizeof(value)); return value;\n"
            << "}\n"
            << "static int64_t vex_i64_bit_and(int64_t left, int64_t right) { return vex_i64_from_bits((uint64_t)left & (uint64_t)right); }\n"
            << "static int64_t vex_i64_bit_or(int64_t left, int64_t right) { return vex_i64_from_bits((uint64_t)left | (uint64_t)right); }\n"
            << "static int64_t vex_i64_bit_xor(int64_t left, int64_t right) { return vex_i64_from_bits((uint64_t)left ^ (uint64_t)right); }\n"
            << "static int64_t vex_i64_bit_not(int64_t value) { return vex_i64_from_bits(~(uint64_t)value); }\n\n"
            << "static int64_t vex_i64_shl(int64_t value, int64_t count) {\n"
            << "    if (count < 0 || count >= 64) vex_runtime_trap(\"int shift count must be in 0..63\");\n"
            << "    return vex_i64_from_bits((uint64_t)value << (unsigned)count);\n"
            << "}\n"
            << "static int64_t vex_i64_shr(int64_t value, int64_t count) {\n"
            << "    if (count < 0 || count >= 64) vex_runtime_trap(\"int shift count must be in 0..63\");\n"
            << "    const unsigned shift = (unsigned)count;\n"
            << "    const uint64_t bits = (uint64_t)value;\n"
            << "    if (shift == 0u) return value;\n"
            << "    uint64_t result = bits >> shift;\n"
            << "    if ((bits & (UINT64_C(1) << 63u)) != 0u) result |= UINT64_MAX << (64u - shift);\n"
            << "    return vex_i64_from_bits(result);\n"
            << "}\n"
            << "static uint8_t vex_u8_shl(uint8_t value, int64_t count) {\n"
            << "    if (count < 0 || count >= 8) vex_runtime_trap(\"byte shift count must be in 0..7\");\n"
            << "    return (uint8_t)((uint16_t)value << (unsigned)count);\n"
            << "}\n"
            << "static uint8_t vex_u8_shr(uint8_t value, int64_t count) {\n"
            << "    if (count < 0 || count >= 8) vex_runtime_trap(\"byte shift count must be in 0..7\");\n"
            << "    return (uint8_t)(value >> (unsigned)count);\n"
            << "}\n\n"
            << "static int64_t vex_i64_rotl(int64_t value, int64_t count) {\n"
            << "    const unsigned shift = (unsigned)((uint64_t)count & UINT64_C(63));\n"
            << "    const uint64_t bits = (uint64_t)value;\n"
            << "    return vex_i64_from_bits((bits << shift) | (bits >> ((64u - shift) & 63u)));\n"
            << "}\n"
            << "static int64_t vex_i64_rotr(int64_t value, int64_t count) {\n"
            << "    const unsigned shift = (unsigned)((uint64_t)count & UINT64_C(63));\n"
            << "    const uint64_t bits = (uint64_t)value;\n"
            << "    return vex_i64_from_bits((bits >> shift) | (bits << ((64u - shift) & 63u)));\n"
            << "}\n"
            << "static uint8_t vex_u8_rotl(uint8_t value, int64_t count) {\n"
            << "    const unsigned shift = (unsigned)((uint64_t)count & UINT64_C(7));\n"
            << "    return (uint8_t)(((uint16_t)value << shift) | ((uint16_t)value >> ((8u - shift) & 7u)));\n"
            << "}\n"
            << "static uint8_t vex_u8_rotr(uint8_t value, int64_t count) {\n"
            << "    const unsigned shift = (unsigned)((uint64_t)count & UINT64_C(7));\n"
            << "    return (uint8_t)(((uint16_t)value >> shift) | ((uint16_t)value << ((8u - shift) & 7u)));\n"
            << "}\n\n"
            << "static vex_string vex_string_concat(vex_string left, vex_string right) {\n"
            << "    if (left.length == 0u) return right;\n"
            << "    if (right.length == 0u) return left;\n"
            << "    if (left.length > SIZE_MAX - right.length)\n"
            << "        vex_runtime_trap(\"string length overflow during concatenation\");\n"
            << "    const size_t length = left.length + right.length;\n"
            << "    if (length > SIZE_MAX - sizeof(vex_string_allocation) - 1u)\n"
            << "        vex_runtime_trap(\"string allocation size overflow\");\n"
            << "    vex_string_allocation *allocation = (vex_string_allocation *)\n"
            << "        malloc(sizeof(*allocation) + length + 1u);\n"
            << "    if (allocation == NULL) vex_runtime_trap(\"string allocation failed\");\n"
            << "    unsigned char *data = allocation->data;\n"
            << "    (void)memcpy(data, left.data, left.length);\n"
            << "    (void)memcpy(data + left.length, right.data, right.length);\n"
            << "    data[length] = 0u;\n"
            << "    allocation->next = vex_string_head;\n"
            << "    vex_string_head = allocation;\n"
            << "    return (vex_string){data, length};\n"
            << "}\n\n"
            << "static vex_string vex_string_join(vex_array *parts) {\n"
            << "    vex_array_check(parts, sizeof(vex_string));\n"
            << "    const vex_string *items = (const vex_string *)parts->data;\n"
            << "    size_t length = 0u;\n"
            << "    for (size_t index = 0u; index < parts->length; ++index) {\n"
            << "        if (items[index].length > SIZE_MAX - length)\n"
            << "            vex_runtime_trap(\"string length overflow during join\");\n"
            << "        length += items[index].length;\n"
            << "    }\n"
            << "    if (length == 0u) return (vex_string){NULL, 0u};\n"
            << "    if (length > SIZE_MAX - sizeof(vex_string_allocation) - 1u)\n"
            << "        vex_runtime_trap(\"string allocation size overflow during join\");\n"
            << "    vex_string_allocation *allocation = (vex_string_allocation *)\n"
            << "        malloc(sizeof(*allocation) + length + 1u);\n"
            << "    if (allocation == NULL) vex_runtime_trap(\"string join allocation failed\");\n"
            << "    unsigned char *data = allocation->data;\n"
            << "    size_t offset = 0u;\n"
            << "    for (size_t index = 0u; index < parts->length; ++index) {\n"
            << "        if (items[index].length != 0u) {\n"
            << "            (void)memcpy(data + offset, items[index].data, items[index].length);\n"
            << "            offset += items[index].length;\n"
            << "        }\n"
            << "    }\n"
            << "    data[length] = 0u;\n"
            << "    allocation->next = vex_string_head;\n"
            << "    vex_string_head = allocation;\n"
            << "    return (vex_string){data, length};\n"
            << "}\n\n"
            << "static void vex_string_cleanup(void) {\n"
            << "    while (vex_string_head != NULL) {\n"
            << "        vex_string_allocation *allocation = vex_string_head;\n"
            << "        vex_string_head = allocation->next;\n"
            << "        free(allocation);\n"
            << "    }\n"
            << "}\n\n"
            << "static bool vex_string_equal(vex_string left, vex_string right) {\n"
            << "    return left.length == right.length &&\n"
            << "        (left.length == 0u || memcmp(left.data, right.data, left.length) == 0);\n"
            << "}\n\n"
            << "static void vex_say_int(int64_t value) {\n"
            << "    (void)printf(\"%\" PRId64 \"\\n\", value);\n"
            << "}\n\n"
            << "static void vex_say_byte(uint8_t value) {\n"
            << "    (void)printf(\"%u\\n\", (unsigned int)value);\n"
            << "}\n\n"
            << "static void vex_say_float(double value) {\n"
            << "    (void)printf(\"%.17g\\n\", value);\n"
            << "}\n\n"
            << "static void vex_say_bool(bool value) {\n"
            << "    (void)puts(value ? \"true\" : \"false\");\n"
            << "}\n\n"
            << "static void vex_say_string(vex_string value) {\n"
            << "    if (value.length != 0u) (void)fwrite(value.data, 1u, value.length, stdout);\n"
            << "    (void)fputc('\\n', stdout);\n"
            << "}\n\n";
    }

    void emit_prototypes() {
        for (const IRFunction& function : module_.functions) {
            emit_function_signature(function);
            output_ << ";\n";
        }
        if (!module_.functions.empty()) output_ << '\n';
    }

    void emit_function_signature(const IRFunction& function) {
        output_ << "static " << c_return_type(function) << ' '
                << function_name(function.name) << '(';
        if (function.parameters.empty()) {
            output_ << "void";
        } else {
            for (std::size_t index = 0; index < function.parameters.size(); ++index) {
                if (index != 0) output_ << ", ";
                output_ << c_type(function.parameters[index].value) << ' '
                        << value_name(function.parameters[index].value.id);
            }
        }
        output_ << ')';
    }

    const IRBlock* find_block(const IRFunction& function, const IRBlockId id) const {
        const auto found = std::find_if(function.blocks.begin(), function.blocks.end(),
            [id](const IRBlock& block) { return block.id == id; });
        return found == function.blocks.end() ? nullptr : &*found;
    }

    void declare_edge(const IRFunction& function, const IRBlockId source,
                      const char arm, const IRBlockId target,
                      const std::vector<IRValueId>& arguments) {
        const IRBlock* destination = find_block(function, target);
        if (!destination) return;
        for (std::size_t index = 0; index < arguments.size() &&
             index < destination->parameters.size(); ++index) {
            output_ << "    " << c_type(destination->parameters[index]) << ' '
                    << edge_name(source, arm, index) << ";\n";
        }
    }

    void declare_edge_temporaries(const IRFunction& function) {
        for (const IRBlock& block : function.blocks) {
            if (!block.terminator) continue;
            std::visit([&](const auto& terminator) {
                using T = std::decay_t<decltype(terminator)>;
                if constexpr (std::is_same_v<T, IRBranch>) {
                    declare_edge(function, block.id, 'u', terminator.target,
                                 terminator.arguments);
                } else if constexpr (std::is_same_v<T, IRConditionalBranch>) {
                    declare_edge(function, block.id, 't', terminator.true_target,
                                 terminator.true_arguments);
                    declare_edge(function, block.id, 'f', terminator.false_target,
                                 terminator.false_arguments);
                }
            }, block.terminator->data);
        }
    }

    void emit_edge(const IRFunction& function, const IRBlockId source,
                   const char arm, const IRBlockId target,
                   const std::vector<IRValueId>& arguments) {
        const IRBlock* destination = find_block(function, target);
        if (!destination) return;

        for (std::size_t index = 0; index < arguments.size() &&
             index < destination->parameters.size(); ++index) {
            output_ << "        " << edge_name(source, arm, index) << " = "
                    << value_name(arguments[index]) << ";\n";
        }
        for (std::size_t index = 0; index < arguments.size() &&
             index < destination->parameters.size(); ++index) {
            output_ << "        " << value_name(destination->parameters[index].id)
                    << " = " << edge_name(source, arm, index) << ";\n";
        }
        output_ << "        goto " << block_name(target) << ";\n";
    }

    void emit_function(const IRFunction& function) {
        emit_function_signature(function);
        output_ << " {\n";

        std::unordered_set<IRValueId> parameter_ids;
        for (const IRFunctionParameter& parameter : function.parameters) {
            parameter_ids.insert(parameter.value.id);
        }
        for (const IRBlock& block : function.blocks) {
            for (const IRValue& parameter : block.parameters) {
                if (parameter_ids.find(parameter.id) == parameter_ids.end()) {
                    output_ << "    " << c_type(parameter) << ' '
                            << value_name(parameter.id) << ";\n";
                }
            }
            for (const IRInstruction& instruction : block.instructions) {
                if (instruction.result &&
                    parameter_ids.find(instruction.result->id) == parameter_ids.end()) {
                    output_ << "    " << c_type(*instruction.result) << ' '
                            << value_name(instruction.result->id) << ";\n";
                }
            }
        }
        declare_edge_temporaries(function);
        output_ << "\n    goto " << block_name(function.entry_block) << ";\n";

        for (const IRBlock& block : function.blocks) {
            output_ << '\n' << block_name(block.id) << ": ;\n";
            for (const IRInstruction& instruction : block.instructions) {
                emit_instruction(function, instruction);
                if (has_errors(diagnostics_)) return;
            }
            if (block.terminator) emit_terminator(function, block);
        }
        output_ << "}\n\n";
    }

    std::optional<IRValue> value_type(const IRFunction& function,
                                    const IRValueId id) const {
        for (const IRFunctionParameter& parameter : function.parameters) {
            if (parameter.value.id == id) return parameter.value;
        }
        for (const IRBlock& block : function.blocks) {
            for (const IRValue& parameter : block.parameters) {
                if (parameter.id == id) return parameter;
            }
            for (const IRInstruction& instruction : block.instructions) {
                if (instruction.result && instruction.result->id == id) {
                    return *instruction.result;
                }
            }
        }
        return std::nullopt;
    }

    bool emit_constant(const IRInstruction& instruction) {
        if (!instruction.result) return false;
        const std::string target = value_name(instruction.result->id);
        switch (instruction.result->type) {
        case IRType::Int: {
            std::int64_t value = 0;
            const char* begin = instruction.immediate.data();
            const char* end = begin + instruction.immediate.size();
            const auto parsed = std::from_chars(begin, end, value, 10);
            if (parsed.ec != std::errc{} || parsed.ptr != end) {
                report("CG003", "integer constant is outside the supported int64 range",
                       instruction.span);
                return false;
            }
            output_ << "    " << target << " = INT64_C(" << value << ");\n";
            return true;
        }
        case IRType::Byte: {
            std::uint16_t value = 0;
            const char* begin = instruction.immediate.data();
            const char* end = begin + instruction.immediate.size();
            const auto parsed = std::from_chars(begin, end, value, 10);
            if (parsed.ec != std::errc{} || parsed.ptr != end || value > 255u) {
                report("CG009", "byte constant must be in the range 0 through 255",
                       instruction.span);
                return false;
            }
            output_ << "    " << target << " = (uint8_t)" << value << "u;\n";
            return true;
        }
        case IRType::Float: {
            char* end = nullptr;
            errno = 0;
            const double parsed = std::strtod(instruction.immediate.c_str(), &end);
            if (end != instruction.immediate.c_str() + instruction.immediate.size() ||
                end == instruction.immediate.c_str() || errno == ERANGE ||
                !std::isfinite(parsed)) {
                report("CG008", "floating-point constant is not a finite representable double",
                       instruction.span);
                return false;
            }
            output_ << "    " << target << " = " << instruction.immediate << ";\n";
            return true;
        }
        case IRType::Bool:
            if (instruction.immediate != "true" && instruction.immediate != "false") {
                report("CG004", "invalid boolean constant spelling", instruction.span);
                return false;
            }
            output_ << "    " << target << " = "
                    << (instruction.immediate == "true" ? "true" : "false") << ";\n";
            return true;
        case IRType::String: {
            const auto decoded = decode_string_literal(instruction.immediate);
            if (!decoded) {
                report("CG005", "invalid string literal spelling in IR", instruction.span);
                return false;
            }
            const std::string escaped = escape_c_string(*decoded);
            output_ << "    " << target
                    << " = (vex_string){ (const unsigned char *)\"" << escaped
                    << "\", sizeof(\"" << escaped << "\") - 1u };\n";
            return true;
        }
        case IRType::Void:
            report("CG006", "void constant cannot be emitted", instruction.span);
            return false;
        case IRType::Array:
            report("CG009", "array values must be constructed with array_make", instruction.span);
            return false;
        case IRType::Struct:
            report("CG010", "structure values must be constructed with struct_make", instruction.span);
            return false;
        case IRType::Enum:
            report("CG010", "enum values must be constructed with enum_make", instruction.span);
            return false;
        }
        return false;
    }

    void emit_instruction(const IRFunction& function,
                          const IRInstruction& instruction) {
        if (instruction.op == IROp::Constant) {
            static_cast<void>(emit_constant(instruction));
            return;
        }

        const auto operand = [&](const std::size_t index) {
            return value_name(instruction.operands[index]);
        };
        const std::string result = instruction.result
            ? value_name(instruction.result->id) : std::string{};

        switch (instruction.op) {
        case IROp::StringConcat:
            output_ << "    " << result << " = vex_string_concat("
                    << operand(0) << ", " << operand(1) << ");\n";
            return;
        case IROp::Add:
        case IROp::Subtract:
        case IROp::Multiply:
        case IROp::Divide:
        case IROp::Remainder: {
            const auto type = value_type(function, instruction.operands[0]);
            std::string operation;
            if (instruction.op == IROp::Add) operation = "+";
            else if (instruction.op == IROp::Subtract) operation = "-";
            else if (instruction.op == IROp::Multiply) operation = "*";
            else if (instruction.op == IROp::Divide) operation = "/";
            else operation = "%";

            std::string expression;
            if (type && type->type == IRType::Int) {
                const char* helper = instruction.op == IROp::Add ? "vex_i64_add" :
                    instruction.op == IROp::Subtract ? "vex_i64_sub" :
                    instruction.op == IROp::Multiply ? "vex_i64_mul" :
                    instruction.op == IROp::Divide ? "vex_i64_div" : "vex_i64_rem";
                expression = std::string(helper) + "(" + operand(0) + ", " +
                             operand(1) + ")";
            } else {
                expression = operand(0) + " " + operation + " " + operand(1);
            }
            output_ << "    " << result << " = " << expression << ";\n";
            return;
        }
        case IROp::Negate: {
            const auto type = value_type(function, instruction.operands[0]);
            const std::string expression = type && type->type == IRType::Int
                ? "vex_i64_neg(" + operand(0) + ")"
                : "(-" + operand(0) + ")";
            output_ << "    " << result << " = " << expression << ";\n";
            return;
        }
        case IROp::BitwiseAnd:
        case IROp::BitwiseOr:
        case IROp::BitwiseXor: {
            const auto type = value_type(function, instruction.operands[0]);
            if (type && type->type == IRType::Byte) {
                const char* operation = instruction.op == IROp::BitwiseAnd ? "&" :
                    instruction.op == IROp::BitwiseOr ? "|" : "^";
                output_ << "    " << result << " = (uint8_t)(" << operand(0)
                        << ' ' << operation << ' ' << operand(1) << ");\n";
            } else {
                const char* helper = instruction.op == IROp::BitwiseAnd ? "vex_i64_bit_and" :
                    instruction.op == IROp::BitwiseOr ? "vex_i64_bit_or" : "vex_i64_bit_xor";
                output_ << "    " << result << " = " << helper << "("
                        << operand(0) << ", " << operand(1) << ");\n";
            }
            return;
        }
        case IROp::BitwiseNot: {
            const auto type = value_type(function, instruction.operands[0]);
            if (type && type->type == IRType::Byte)
                output_ << "    " << result << " = (uint8_t)(~" << operand(0) << ");\n";
            else
                output_ << "    " << result << " = vex_i64_bit_not(" << operand(0) << ");\n";
            return;
        }
        case IROp::ShiftLeft:
        case IROp::ShiftRight:
        case IROp::RotateLeft:
        case IROp::RotateRight: {
            const auto type = value_type(function, instruction.operands[0]);
            const bool is_byte = type && type->type == IRType::Byte;
            const char* operation = instruction.op == IROp::ShiftLeft ? "shl" :
                instruction.op == IROp::ShiftRight ? "shr" :
                instruction.op == IROp::RotateLeft ? "rotl" : "rotr";
            output_ << "    " << result << " = " << (is_byte ? "vex_u8_" : "vex_i64_")
                    << operation << "(" << operand(0) << ", " << operand(1) << ");\n";
            return;
        }
        case IROp::LogicalNot:
            output_ << "    " << result << " = !" << operand(0) << ";\n";
            return;
        case IROp::LogicalAnd:
        case IROp::LogicalOr:
            output_ << "    " << result << " = " << operand(0)
                    << (instruction.op == IROp::LogicalAnd ? " && " : " || ")
                    << operand(1) << ";\n";
            return;
        case IROp::Equal:
        case IROp::NotEqual: {
            const auto type = value_type(function, instruction.operands[0]);
            if (type && type->type == IRType::String) {
                output_ << "    " << result << " = vex_string_equal("
                        << operand(0) << ", " << operand(1) << ')';
                if (instruction.op == IROp::NotEqual) output_ << " == false";
                output_ << ";\n";
            } else {
                output_ << "    " << result << " = " << operand(0)
                        << (instruction.op == IROp::Equal ? " == " : " != ")
                        << operand(1) << ";\n";
            }
            return;
        }
        case IROp::Less:
        case IROp::LessEqual:
        case IROp::Greater:
        case IROp::GreaterEqual: {
            const char* operation = instruction.op == IROp::Less ? " < " :
                instruction.op == IROp::LessEqual ? " <= " :
                instruction.op == IROp::Greater ? " > " : " >= ";
            output_ << "    " << result << " = " << operand(0)
                    << operation << operand(1) << ";\n";
            return;
        }
        case IROp::IntToFloat:
            output_ << "    " << result << " = (double)" << operand(0) << ";\n";
            return;
        case IROp::IntToByteChecked:
            output_ << "    " << result << " = vex_checked_byte(" << operand(0) << ");\n";
            return;
        case IROp::ByteToInt:
            output_ << "    " << result << " = (int64_t)" << operand(0) << ";\n";
            return;
        case IROp::Call:
            if (instruction.result) output_ << "    " << result << " = ";
            else output_ << "    ";
            output_ << function_name(instruction.callee) << '(';
            for (std::size_t index = 0; index < instruction.operands.size(); ++index) {
                if (index != 0) output_ << ", ";
                output_ << operand(index);
            }
            output_ << ");\n";
            return;
        case IROp::Math: {
            const char* function = instruction.callee == "abs" ? "fabs" :
                instruction.callee.c_str();
            output_ << "    " << result << " = " << function << '(';
            for (std::size_t index = 0; index < instruction.operands.size(); ++index) {
                if (index != 0) output_ << ", ";
                output_ << operand(index);
            }
            output_ << ");\n";
            return;
        }
        case IROp::StructMake: {
            if (!instruction.result) return;
            const std::uint32_t id = instruction.result->struct_type_id;
            const IRStructType& structure = module_.struct_types[id - 1u];
            for (std::size_t index = 0; index < structure.fields.size(); ++index) {
                output_ << "    " << result << "."
                        << struct_field_name(static_cast<std::uint32_t>(index))
                        << " = " << operand(index) << ";\n";
            }
            return;
        }
        case IROp::StructGet: {
            const auto object = value_type(function, instruction.operands[0]);
            if (!object) return;
            const std::size_t index = static_cast<std::size_t>(std::stoul(instruction.immediate));
            output_ << "    " << result << " = " << operand(0) << "."
                    << struct_field_name(static_cast<std::uint32_t>(index)) << ";\n";
            return;
        }
        case IROp::StructSet: {
            const auto object = value_type(function, instruction.operands[0]);
            if (!object) return;
            const std::size_t index = static_cast<std::size_t>(std::stoul(instruction.immediate));
            output_ << "    " << result << " = " << operand(0) << ";\n"
                    << "    " << result << "."
                    << struct_field_name(static_cast<std::uint32_t>(index))
                    << " = " << operand(1) << ";\n";
            return;
        }
        case IROp::ArrayMake: {
            const IRValue& array = *instruction.result;
            const IRValue element = array_element_value(array);
            if (array.array_length == 0) {
                output_ << "    " << result << " = vex_array_new(sizeof("
                        << c_type(element) << "));\n";
                for (std::size_t index = 0; index < instruction.operands.size(); ++index) {
                    const std::string temporary = "vex_array_init_" +
                        std::to_string(array.id) + "_" + std::to_string(index);
                    output_ << "    " << c_type(element) << " " << temporary
                            << " = " << operand(index) << ";\n"
                            << "    vex_array_push(" << result << ", &" << temporary
                            << ", sizeof(" << temporary << "));\n";
                }
            } else {
                for (std::size_t index = 0; index < instruction.operands.size(); ++index)
                    output_ << "    " << result << ".data[" << index << "] = " << operand(index) << ";\n";
            }
            return;
        }
        case IROp::ArrayGet: {
            const auto array = value_type(function, instruction.operands[0]);
            if (!array) return;
            const IRValue element = array_element_value(*array);
            if (array->array_length == 0) {
                output_ << "    " << result << " = *(" << c_type(element)
                        << " *)vex_array_get(" << operand(0) << ", " << operand(1)
                        << ", sizeof(" << c_type(element) << "));\n";
            } else {
                output_ << "    " << result << " = " << operand(0)
                        << ".data[vex_checked_array_index(" << operand(1) << ", "
                        << array->array_length << "u)];\n";
            }
            return;
        }
        case IROp::ArraySet: {
            const auto array = value_type(function, instruction.operands[0]);
            if (!array) return;
            const IRValue element = array_element_value(*array);
            if (array->array_length == 0) {
                const std::string temporary = "vex_array_set_" +
                    std::to_string(instruction.result->id);
                output_ << "    " << result << " = " << operand(0) << ";\n"
                        << "    " << c_type(element) << " " << temporary
                        << " = " << operand(2) << ";\n"
                        << "    vex_array_set(" << operand(0) << ", " << operand(1)
                        << ", &" << temporary << ", sizeof(" << temporary << "));\n";
            } else {
                output_ << "    " << result << " = " << operand(0) << ";\n"
                        << "    " << result << ".data[vex_checked_array_index("
                        << operand(1) << ", " << array->array_length << "u)] = "
                        << operand(2) << ";\n";
            }
            return;
        }
        case IROp::ArrayLength:
            output_ << "    " << result << " = vex_array_length(" << operand(0) << ");\n";
            return;
        case IROp::ArrayCapacity: {
            const auto array = value_type(function, instruction.operands[0]);
            if (!array) return;
            const IRValue element = array_element_value(*array);
            output_ << "    " << result << " = vex_array_capacity("
                    << operand(0) << ", sizeof(" << c_type(element) << "));\n";
            return;
        }
        case IROp::ArrayReserve: {
            const auto array = value_type(function, instruction.operands[0]);
            if (!array) return;
            const IRValue element = array_element_value(*array);
            output_ << "    vex_array_reserve(" << operand(0) << ", "
                    << operand(1) << ", sizeof(" << c_type(element) << "));\n";
            return;
        }
        case IROp::StringLength:
            output_ << "    " << result << " = " << operand(0) << ".length;\n";
            return;
        case IROp::StringJoin:
            output_ << "    " << result << " = vex_string_join(" << operand(0) << ");\n";
            return;
        case IROp::ByteSlice:
            output_ << "    " << result << " = vex_byte_slice(" << operand(0) << ", "
                    << operand(1) << ", " << operand(2) << ");\n";
            return;
        case IROp::ByteRead:
        case IROp::ByteWrite: {
            const std::size_t width = instruction.immediate.starts_with("2") ? 2u : 4u;
            const bool little = instruction.immediate.ends_with("le");
            if (instruction.op == IROp::ByteRead) {
                output_ << "    " << result << " = vex_byte_read(" << operand(0) << ", "
                        << operand(1) << ", " << width << "u, "
                        << (little ? "true" : "false") << ");\n";
            } else {
                output_ << "    vex_byte_write(" << operand(0) << ", " << operand(1)
                        << ", " << operand(2) << ", " << width << "u, "
                        << (little ? "true" : "false") << ");\n";
            }
            return;
        }
        case IROp::FileRead:
            output_ << "    " << result << " = vex_file_read(" << operand(0)
                    << ", " << operand(1) << ");\n";
            return;
        case IROp::FileWrite:
            output_ << "    " << result << " = vex_file_write(" << operand(0)
                    << ", " << operand(1) << ");\n";
            return;
        case IROp::ArrayPush: {
            const auto array = value_type(function, instruction.operands[0]);
            if (!array) return;
            const IRValue element = array_element_value(*array);
            const std::string temporary = "vex_array_push_" +
                std::to_string(instruction.span.begin);
            output_ << "    " << c_type(element) << " " << temporary
                    << " = " << operand(1) << ";\n"
                    << "    vex_array_push(" << operand(0) << ", &" << temporary
                    << ", sizeof(" << temporary << "));\n";
            return;
        }
        case IROp::ArrayPop: {
            const auto array = value_type(function, instruction.operands[0]);
            if (!array) return;
            output_ << "    vex_array_pop(" << operand(0) << ", &" << result
                    << ", sizeof(" << result << "));\n";
            return;
        }
        case IROp::Say: {
            const auto type = value_type(function, instruction.operands[0]);
            const char* helper = type && type->type == IRType::Int ? "vex_say_int" :
                type && type->type == IRType::Byte ? "vex_say_byte" :
                type && type->type == IRType::Float ? "vex_say_float" :
                type && type->type == IRType::Bool ? "vex_say_bool" : "vex_say_string";
            output_ << "    " << helper << '(' << operand(0) << ");\n";
            return;
        }
        case IROp::Constant:
            return;
        }
    }

    void emit_terminator(const IRFunction& function, const IRBlock& block) {
        std::visit([&](const auto& terminator) {
            using T = std::decay_t<decltype(terminator)>;
            if constexpr (std::is_same_v<T, IRReturn>) {
                output_ << "    return";
                if (terminator.value) output_ << ' ' << value_name(*terminator.value);
                output_ << ";\n";
            } else if constexpr (std::is_same_v<T, IRBranch>) {
                output_ << "    {\n";
                emit_edge(function, block.id, 'u', terminator.target,
                          terminator.arguments);
                output_ << "    }\n";
            } else if constexpr (std::is_same_v<T, IRConditionalBranch>) {
                output_ << "    if (" << value_name(terminator.condition) << ") {\n";
                emit_edge(function, block.id, 't', terminator.true_target,
                          terminator.true_arguments);
                output_ << "    } else {\n";
                emit_edge(function, block.id, 'f', terminator.false_target,
                          terminator.false_arguments);
                output_ << "    }\n";
            } else if constexpr (std::is_same_v<T, IRUnreachable>) {
                output_ << "    vex_runtime_trap(\"reached unreachable code\");\n";
            }
        }, block.terminator->data);
    }

    void emit_entry_wrapper() {
        if (!entry_) return;
        output_ << "int main(void) {\n";
        if (entry_->return_type == IRType::Void) {
            output_ << "    " << function_name(entry_->name) << "();\n"
                    << "    vex_array_cleanup();\n"
                    << "    vex_string_cleanup();\n"
                    << "    return 0;\n";
        } else {
            output_ << "    const int64_t vex_status = "
                    << function_name(entry_->name) << "();\n"
                    << "    vex_array_cleanup();\n"
                    << "    vex_string_cleanup();\n"
                    << "    if (vex_status < 0 || vex_status > 255)\n"
                    << "        vex_runtime_trap(\"main returned an exit code outside 0..255\");\n"
                    << "    return (int)vex_status;\n";
        }
        output_ << "}\n";
    }

    const IRModule& module_;
    std::set<std::string> emitted_array_shapes_;
    std::unordered_map<std::string, IRValue> emitted_array_types_;
    std::ostringstream output_;
    std::vector<Diagnostic> diagnostics_;
    const IRFunction* entry_{nullptr};
};

} // namespace

bool CBackendResult::has_errors() const noexcept {
    return std::any_of(diagnostics.begin(), diagnostics.end(),
        [](const Diagnostic& diagnostic) {
            return diagnostic.severity == DiagnosticSeverity::Error;
        });
}

std::string_view CBackend::name() const noexcept {
    return "c";
}

BackendResult CBackend::emit(const IRModule& module) const {
    CBackendResult generated = generate(module);
    return BackendResult{
        std::move(generated.source),
        std::move(generated.diagnostics)
    };
}

CBackendResult CBackend::generate(const IRModule& module) const {
    return CEmitter(module).run();
}

} // namespace vex
