#include "vex/c_backend.hpp"

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
    case IRType::Float: return "double";
    case IRType::Bool: return "bool";
    case IRType::String: return "vex_string";
    case IRType::Array: return "void";
    case IRType::Struct: return "void";
    }
    return "void";
}

std::string struct_type_name(const std::uint32_t id) {
    return "vex_struct_" + std::to_string(id);
}

std::string struct_field_name(const std::uint32_t index) {
    return "vex_field_" + std::to_string(index);
}

std::string array_type_name(const IRType element_type, const std::uint32_t length) {
    const char* element = element_type == IRType::Int ? "int" :
        element_type == IRType::Float ? "float" :
        element_type == IRType::Bool ? "bool" : "string";
    return "vex_array_" + std::string(element) + "_" + std::to_string(length);
}

std::string c_type(const IRValue& value) {
    if (value.type == IRType::Struct) return struct_type_name(value.struct_type_id);
    if (value.type != IRType::Array) return c_type(value.type);
    return value.array_length == 0
        ? "vex_array *"
        : array_type_name(value.element_type, value.array_length);
}

std::string c_return_type(const IRFunction& function) {
    if (function.return_type == IRType::Struct)
        return struct_type_name(function.return_struct_type_id);
    if (function.return_type != IRType::Array) return c_type(function.return_type);
    return function.return_array_length == 0
        ? "vex_array *"
        : array_type_name(function.return_element_type, function.return_array_length);
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
        emit_struct_types();
        emit_array_types();
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
            << "static vex_array *vex_array_head = NULL;\n\n";
    }

    void emit_struct_types() {
        if (module_.struct_types.empty()) return;
        for (const IRStructType& structure : module_.struct_types) {
            output_ << "typedef struct " << struct_type_name(structure.id)
                    << " " << struct_type_name(structure.id) << ";\n";
        }
        output_ << '\n';
        for (const IRStructType& structure : module_.struct_types) {
            output_ << "struct " << struct_type_name(structure.id) << " {\n";
            for (std::size_t index = 0; index < structure.fields.size(); ++index) {
                const IRStructField& field = structure.fields[index];
                const std::string field_type = field.type == IRType::Struct
                    ? struct_type_name(field.struct_type_id) : c_type(field.type);
                output_ << "    " << field_type << ' '
                        << struct_field_name(static_cast<std::uint32_t>(index)) << ";\n";
            }
            output_ << "};\n";
        }
        output_ << '\n';
    }

    void emit_array_types() {
        std::set<std::pair<IRType, std::uint32_t>> shapes;
        for (const IRFunction& function : module_.functions) {
            if (function.return_type == IRType::Array)
                shapes.emplace(function.return_element_type, function.return_array_length);
            for (const IRFunctionParameter& parameter : function.parameters)
                if (parameter.value.type == IRType::Array)
                    shapes.emplace(parameter.value.element_type, parameter.value.array_length);
            for (const IRBlock& block : function.blocks) {
                for (const IRValue& parameter : block.parameters)
                    if (parameter.type == IRType::Array)
                        shapes.emplace(parameter.element_type, parameter.array_length);
                for (const IRInstruction& instruction : block.instructions)
                    if (instruction.result && instruction.result->type == IRType::Array)
                        shapes.emplace(instruction.result->element_type, instruction.result->array_length);
            }
        }
        for (const auto& [element, length] : shapes) {
            if (length == 0) continue;
            output_ << "typedef struct " << array_type_name(element, length) << " {\n"
                    << "    " << c_type(element) << " data[" << length << "];\n"
                    << "} " << array_type_name(element, length) << ";\n";
        }
        if (!shapes.empty()) output_ << '\n';
    }

    void emit_runtime() {
        output_
            << "static void vex_runtime_trap(const char *message) {\n"
            << "    (void)fprintf(stderr, \"VEX runtime error: %s\\n\", message);\n"
            << "    exit(EXIT_FAILURE);\n"
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
            << "static bool vex_string_equal(vex_string left, vex_string right) {\n"
            << "    return left.length == right.length &&\n"
            << "        (left.length == 0u || memcmp(left.data, right.data, left.length) == 0);\n"
            << "}\n\n"
            << "static void vex_say_int(int64_t value) {\n"
            << "    (void)printf(\"%\" PRId64 \"\\n\", value);\n"
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
            const IRStructType& structure = module_.struct_types[object->struct_type_id - 1u];
            const std::size_t index = static_cast<std::size_t>(std::stoul(instruction.immediate));
            output_ << "    " << result << " = " << operand(0) << "."
                    << struct_field_name(static_cast<std::uint32_t>(index)) << ";\n";
            return;
        }
        case IROp::StructSet: {
            const auto object = value_type(function, instruction.operands[0]);
            if (!object) return;
            const IRStructType& structure = module_.struct_types[object->struct_type_id - 1u];
            const std::size_t index = static_cast<std::size_t>(std::stoul(instruction.immediate));
            output_ << "    " << result << " = " << operand(0) << ";\n"
                    << "    " << result << "."
                    << struct_field_name(static_cast<std::uint32_t>(index))
                    << " = " << operand(1) << ";\n";
            return;
        }
        case IROp::ArrayMake: {
            const IRValue& array = *instruction.result;
            if (array.array_length == 0) {
                output_ << "    " << result << " = vex_array_new(sizeof("
                        << c_type(array.element_type) << "));\n";
                for (std::size_t index = 0; index < instruction.operands.size(); ++index) {
                    const std::string temporary = "vex_array_init_" +
                        std::to_string(array.id) + "_" + std::to_string(index);
                    output_ << "    " << c_type(array.element_type) << " " << temporary
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
            if (array->array_length == 0) {
                output_ << "    " << result << " = *(" << c_type(array->element_type)
                        << " *)vex_array_get(" << operand(0) << ", " << operand(1)
                        << ", sizeof(" << c_type(array->element_type) << "));\n";
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
            if (array->array_length == 0) {
                const std::string temporary = "vex_array_set_" +
                    std::to_string(instruction.result->id);
                output_ << "    " << result << " = " << operand(0) << ";\n"
                        << "    " << c_type(array->element_type) << " " << temporary
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
        case IROp::ArrayPush: {
            const auto array = value_type(function, instruction.operands[0]);
            if (!array) return;
            const std::string temporary = "vex_array_push_" +
                std::to_string(instruction.span.begin);
            output_ << "    " << c_type(array->element_type) << " " << temporary
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
                    << "    return 0;\n";
        } else {
            output_ << "    const int64_t vex_status = "
                    << function_name(entry_->name) << "();\n"
                    << "    vex_array_cleanup();\n"
                    << "    if (vex_status < 0 || vex_status > 255)\n"
                    << "        vex_runtime_trap(\"main returned an exit code outside 0..255\");\n"
                    << "    return (int)vex_status;\n";
        }
        output_ << "}\n";
    }

    const IRModule& module_;
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
