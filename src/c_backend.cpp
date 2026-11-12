#include "vex/c_backend.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
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
    }
    return "void";
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
            << "} vex_string;\n\n";
    }

    void emit_runtime() {
        output_
            << "static void vex_runtime_trap(const char *message) {\n"
            << "    (void)fprintf(stderr, \"VEX runtime error: %s\\n\", message);\n"
            << "    exit(EXIT_FAILURE);\n"
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
            << "    if (left == INT64_MIN && right == -1)\n"
            << "        vex_runtime_trap(\"signed integer overflow in remainder\");\n"
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
        output_ << "static " << c_type(function.return_type) << ' '
                << function_name(function.name) << '(';
        if (function.parameters.empty()) {
            output_ << "void";
        } else {
            for (std::size_t index = 0; index < function.parameters.size(); ++index) {
                if (index != 0) output_ << ", ";
                output_ << c_type(function.parameters[index].value.type) << ' '
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
            output_ << "    " << c_type(destination->parameters[index].type) << ' '
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
                    output_ << "    " << c_type(parameter.type) << ' '
                            << value_name(parameter.id) << ";\n";
                }
            }
            for (const IRInstruction& instruction : block.instructions) {
                if (instruction.result &&
                    parameter_ids.find(instruction.result->id) == parameter_ids.end()) {
                    output_ << "    " << c_type(instruction.result->type) << ' '
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

    std::optional<IRType> value_type(const IRFunction& function,
                                    const IRValueId id) const {
        for (const IRFunctionParameter& parameter : function.parameters) {
            if (parameter.value.id == id) return parameter.value.type;
        }
        for (const IRBlock& block : function.blocks) {
            for (const IRValue& parameter : block.parameters) {
                if (parameter.id == id) return parameter.type;
            }
            for (const IRInstruction& instruction : block.instructions) {
                if (instruction.result && instruction.result->id == id) {
                    return instruction.result->type;
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
            if (type == IRType::Int) {
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
            const std::string expression = type == IRType::Int
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
            if (type == IRType::String) {
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
        case IROp::Say: {
            const auto type = value_type(function, instruction.operands[0]);
            const char* helper = type == IRType::Int ? "vex_say_int" :
                type == IRType::Float ? "vex_say_float" :
                type == IRType::Bool ? "vex_say_bool" : "vex_say_string";
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
                    << "    return 0;\n";
        } else {
            output_ << "    const int64_t vex_status = "
                    << function_name(entry_->name) << "();\n"
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

CBackendResult CBackend::generate(const IRModule& module) const {
    return CEmitter(module).run();
}

} // namespace vex
