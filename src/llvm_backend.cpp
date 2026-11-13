#include "vex/llvm_backend.hpp"

#include <algorithm>
#include <bit>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vex {
namespace {

std::string llvm_type(const IRType type) {
    switch (type) {
    case IRType::Void: return "void";
    case IRType::Int: return "i64";
    case IRType::Float: return "double";
    case IRType::Bool: return "i1";
    case IRType::String: return "{ ptr, i64 }";
    case IRType::Array: return "[0 x i8]";
    case IRType::Struct: return "%vex.struct.0";
    }
    return "void";
}

std::string llvm_struct_type_name(const std::uint32_t id) {
    return "%vex.struct." + std::to_string(id);
}

std::string llvm_type(const IRValue& value) {
    if (value.type == IRType::Struct) return llvm_struct_type_name(value.struct_type_id);
    if (value.type != IRType::Array) return llvm_type(value.type);
    if (value.array_length == 0) return "ptr";
    return "[" + std::to_string(value.array_length) + " x " +
           llvm_type(value.element_type) + "]";
}

std::string llvm_return_type(const IRFunction& function) {
    if (function.return_type == IRType::Struct)
        return llvm_struct_type_name(function.return_struct_type_id);
    if (function.return_type != IRType::Array)
        return llvm_type(function.return_type);
    if (function.return_array_length == 0) return "ptr";
    return "[" + std::to_string(function.return_array_length) + " x " +
           llvm_type(function.return_element_type) + "]";
}

std::string value_slot(const IRValueId id) {
    return "%vex_slot_" + std::to_string(id);
}

std::string block_label(const IRBlockId id) {
    return "vex_bb_" + std::to_string(id);
}

std::string edge_label(const IRBlockId source, const char arm) {
    return "vex_edge_" + std::to_string(source) + "_" + arm;
}

std::string function_symbol(const std::string& name) {
    return "@vex_fn_" + name;
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

std::string llvm_escape_bytes(const std::string_view bytes) {
    std::ostringstream escaped;
    escaped << std::uppercase << std::hex;
    for (const unsigned char byte : bytes) {
        if (byte >= 0x20u && byte <= 0x7eu && byte != '"' && byte != '\\') {
            escaped << static_cast<char>(byte);
        } else {
            escaped << '\\' << std::setw(2) << std::setfill('0')
                    << static_cast<unsigned int>(byte);
        }
    }
    escaped << "\\00";
    return escaped.str();
}

std::optional<std::int64_t> parse_integer(const std::string& text) {
    std::int64_t value = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, value, 10);
    if (parsed.ec != std::errc{} || parsed.ptr != end) return std::nullopt;
    return value;
}

std::optional<std::string> llvm_float_literal(const std::string& text) {
    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(text.c_str(), &end);
    if (end != text.c_str() + text.size() || end == text.c_str() ||
        errno == ERANGE || !std::isfinite(value)) {
        return std::nullopt;
    }

    const std::uint64_t bits = std::bit_cast<std::uint64_t>(value);
    std::ostringstream result;
    result << "0x" << std::uppercase << std::hex
           << std::setw(16) << std::setfill('0') << bits;
    return result.str();
}

struct StringGlobal {
    std::string symbol;
    std::string bytes;
};

class LLVMEmitter {
public:
    explicit LLVMEmitter(const IRModule& module) : module_(module) {}

    BackendResult run() {
        BackendResult result;
        const IRVerificationResult verified = IRVerifier{}.verify(module_);
        result.diagnostics = verified.diagnostics;
        if (verified.has_errors()) return result;

        validate_module();
        collect_string_literals();
        if (has_errors(diagnostics_)) {
            result.diagnostics = std::move(diagnostics_);
            return result;
        }

        emit_header();
        emit_string_globals();
        emit_runtime_declarations();
        emit_checked_integer_helpers();
        emit_checked_array_index_helper();
        emit_array_runtime_helpers();
        for (const IRFunction& function : module_.functions) {
            emit_function(function);
            if (has_errors(diagnostics_)) break;
        }
        if (!has_errors(diagnostics_)) emit_entry_wrapper();

        result.output = output_.str();
        result.diagnostics = std::move(diagnostics_);
        if (has_errors(result.diagnostics)) result.output.clear();
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

    void validate_module() {
        for (const IRFunction& function : module_.functions) {
            if (!is_vex_identifier(function.name)) {
                report("LLVM001", "function name cannot be represented as a VEX identifier",
                       function.span);
            }
            if (function.name == "main") {
                entry_ = &function;
                if (!function.parameters.empty()) {
                    report("LLVM002", "the LLVM backend requires main() to have no parameters",
                           function.span);
                }
                if (function.return_type != IRType::Void &&
                    function.return_type != IRType::Int) {
                    report("LLVM003", "main must return void or int",
                           function.span);
                }
            }
        }
    }

    void collect_string_literals() {
        std::size_t next = 0;
        for (const IRFunction& function : module_.functions) {
            for (const IRBlock& block : function.blocks) {
                for (const IRInstruction& instruction : block.instructions) {
                    if (instruction.op != IROp::Constant || !instruction.result ||
                        instruction.result->type != IRType::String) {
                        continue;
                    }
                    const auto decoded = decode_string_literal(instruction.immediate);
                    if (!decoded) {
                        report("LLVM004", "invalid string literal spelling in IR",
                               instruction.span);
                        continue;
                    }
                    const std::string symbol = "@.vex_str_" + std::to_string(next++);
                    string_globals_.push_back(StringGlobal{symbol, *decoded});
                    string_symbols_.emplace(&instruction, symbol);
                }
            }
        }
    }

    void emit_header() {
        output_ << "; Generated by the VEX LLVM backend. Do not edit by hand.\n"
                << "source_filename = \"vex-module\"\n\n"
                << "@.vex_fmt_i64 = private unnamed_addr constant [6 x i8] "
                   "c\"%lld\\0A\\00\", align 1\n"
                << "@.vex_fmt_f64 = private unnamed_addr constant [7 x i8] "
                   "c\"%.17g\\0A\\00\", align 1\n"
                << "@.vex_true = private unnamed_addr constant [5 x i8] "
                   "c\"true\\00\", align 1\n"
                << "@.vex_false = private unnamed_addr constant [6 x i8] "
                   "c\"false\\00\", align 1\n"
                << "%vex.array = type { ptr, i64, i64, i64, ptr }\n";
        for (const IRStructType& structure : module_.struct_types) {
            output_ << llvm_struct_type_name(structure.id) << " = type { ";
            for (std::size_t index = 0; index < structure.fields.size(); ++index) {
                if (index != 0) output_ << ", ";
                const IRStructField& field = structure.fields[index];
                if (field.type == IRType::Struct)
                    output_ << llvm_struct_type_name(field.struct_type_id);
                else
                    output_ << llvm_type(field.type);
            }
            output_ << " }\n";
        }
        output_ << "@vex_array_head = internal global ptr null\n\n";
    }

    void emit_string_globals() {
        for (const StringGlobal& item : string_globals_) {
            output_ << item.symbol << " = private unnamed_addr constant ["
                    << (item.bytes.size() + 1u) << " x i8] c\""
                    << llvm_escape_bytes(item.bytes) << "\", align 1\n";
        }
        if (!string_globals_.empty()) output_ << '\n';
    }

    void emit_runtime_declarations() {
        output_ << "declare i32 @printf(ptr, ...)\n"
                << "declare i32 @puts(ptr)\n"
                << "declare i32 @putchar(i32)\n"
                << "declare i32 @memcmp(ptr, ptr, i64)\n"
                << "declare ptr @malloc(i64)\n"
                << "declare ptr @realloc(ptr, i64)\n"
                << "declare void @free(ptr)\n"
                << "declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1 immarg)\n"
                << "declare { i64, i1 } @llvm.umul.with.overflow.i64(i64, i64)\n"
                << "declare void @llvm.trap()\n"
                << "declare { i64, i1 } @llvm.sadd.with.overflow.i64(i64, i64)\n"
                << "declare { i64, i1 } @llvm.ssub.with.overflow.i64(i64, i64)\n"
                << "declare { i64, i1 } @llvm.smul.with.overflow.i64(i64, i64)\n"
                << "declare double @sqrt(double)\n"
                << "declare double @sin(double)\n"
                << "declare double @cos(double)\n"
                << "declare double @pow(double, double)\n"
                << "declare double @fabs(double)\n\n";
    }

    void emit_checked_integer_helpers() {
        emit_overflow_helper("add", "llvm.sadd.with.overflow.i64");
        emit_overflow_helper("sub", "llvm.ssub.with.overflow.i64");
        emit_overflow_helper("mul", "llvm.smul.with.overflow.i64");
        output_
            << "define internal i64 @vex_i64_neg(i64 %value) {\n"
            << "entry:\n"
            << "  %pair = call { i64, i1 } @llvm.ssub.with.overflow.i64(i64 0, i64 %value)\n"
            << "  %result = extractvalue { i64, i1 } %pair, 0\n"
            << "  %overflow = extractvalue { i64, i1 } %pair, 1\n"
            << "  br i1 %overflow, label %trap, label %ok\n"
            << "trap:\n"
            << "  call void @llvm.trap()\n"
            << "  unreachable\n"
            << "ok:\n"
            << "  ret i64 %result\n"
            << "}\n\n";
        emit_division_helper("div", "sdiv");
        emit_division_helper("rem", "srem");
    }

    void emit_checked_array_index_helper() {
        output_ << "define internal i64 @vex_checked_array_index(i64 %index, i64 %length) {\n"
                << "entry:\n"
                << "  %negative = icmp slt i64 %index, 0\n"
                << "  %past_end = icmp sge i64 %index, %length\n"
                << "  %invalid = or i1 %negative, %past_end\n"
                << "  br i1 %invalid, label %trap, label %ok\n"
                << "trap:\n"
                << "  call void @llvm.trap()\n"
                << "  unreachable\n"
                << "ok:\n"
                << "  ret i64 %index\n"
                << "}\n\n";
    }

    void emit_array_runtime_helpers() {
        output_ << R"IR(
define internal ptr @vex_array_new(i64 %element_size) {
entry:
  %invalid = icmp eq i64 %element_size, 0
  br i1 %invalid, label %trap, label %allocate
allocate:
  %array = call ptr @malloc(i64 40)
  %failed = icmp eq ptr %array, null
  br i1 %failed, label %trap, label %initialize
initialize:
  %data_ptr = getelementptr inbounds %vex.array, ptr %array, i32 0, i32 0
  %length_ptr = getelementptr inbounds %vex.array, ptr %array, i32 0, i32 1
  %capacity_ptr = getelementptr inbounds %vex.array, ptr %array, i32 0, i32 2
  %size_ptr = getelementptr inbounds %vex.array, ptr %array, i32 0, i32 3
  %next_ptr = getelementptr inbounds %vex.array, ptr %array, i32 0, i32 4
  store ptr null, ptr %data_ptr
  store i64 0, ptr %length_ptr
  store i64 0, ptr %capacity_ptr
  store i64 %element_size, ptr %size_ptr
  %head = load ptr, ptr @vex_array_head
  store ptr %head, ptr %next_ptr
  store ptr %array, ptr @vex_array_head
  ret ptr %array
trap:
  call void @llvm.trap()
  unreachable
}

define internal i64 @vex_array_length(ptr %array) {
entry:
  %length_ptr = getelementptr inbounds %vex.array, ptr %array, i32 0, i32 1
  %length = load i64, ptr %length_ptr
  ret i64 %length
}

define internal ptr @vex_array_get(ptr %array, i64 %index, i64 %element_size) {
entry:
  %size_ptr = getelementptr inbounds %vex.array, ptr %array, i32 0, i32 3
  %actual_size = load i64, ptr %size_ptr
  %size_ok = icmp eq i64 %actual_size, %element_size
  br i1 %size_ok, label %check_index, label %trap
check_index:
  %length_ptr = getelementptr inbounds %vex.array, ptr %array, i32 0, i32 1
  %length = load i64, ptr %length_ptr
  %negative = icmp slt i64 %index, 0
  %past_end = icmp sge i64 %index, %length
  %invalid = or i1 %negative, %past_end
  br i1 %invalid, label %trap, label %address
address:
  %data_ptr = getelementptr inbounds %vex.array, ptr %array, i32 0, i32 0
  %data = load ptr, ptr %data_ptr
  %offset = mul i64 %index, %element_size
  %element = getelementptr inbounds i8, ptr %data, i64 %offset
  ret ptr %element
trap:
  call void @llvm.trap()
  unreachable
}

define internal void @vex_array_push(ptr %array, ptr %value, i64 %element_size) {
entry:
  %size_ptr = getelementptr inbounds %vex.array, ptr %array, i32 0, i32 3
  %actual_size = load i64, ptr %size_ptr
  %size_ok = icmp eq i64 %actual_size, %element_size
  br i1 %size_ok, label %read_length, label %trap
read_length:
  %length_ptr = getelementptr inbounds %vex.array, ptr %array, i32 0, i32 1
  %capacity_ptr = getelementptr inbounds %vex.array, ptr %array, i32 0, i32 2
  %data_ptr = getelementptr inbounds %vex.array, ptr %array, i32 0, i32 0
  %length = load i64, ptr %length_ptr
  %capacity = load i64, ptr %capacity_ptr
  %at_limit = icmp eq i64 %length, 9223372036854775807
  br i1 %at_limit, label %trap, label %growth_check
growth_check:
  %required = add i64 %length, 1
  %enough = icmp uge i64 %capacity, %required
  br i1 %enough, label %ready, label %grow
grow:
  %capacity_zero = icmp eq i64 %capacity, 0
  %capacity_too_large = icmp ugt i64 %capacity, 4611686018427387903
  %doubled = mul i64 %capacity, 2
  %initial_or_doubled = select i1 %capacity_zero, i64 4, i64 %doubled
  %safe_capacity = select i1 %capacity_too_large, i64 %required, i64 %initial_or_doubled
  %undersized = icmp ult i64 %safe_capacity, %required
  %new_capacity = select i1 %undersized, i64 %required, i64 %safe_capacity
  %size_pair = call { i64, i1 } @llvm.umul.with.overflow.i64(i64 %new_capacity, i64 %element_size)
  %byte_count = extractvalue { i64, i1 } %size_pair, 0
  %size_overflow = extractvalue { i64, i1 } %size_pair, 1
  br i1 %size_overflow, label %trap, label %reallocate
reallocate:
  %old_data = load ptr, ptr %data_ptr
  %new_data = call ptr @realloc(ptr %old_data, i64 %byte_count)
  %allocation_failed = icmp eq ptr %new_data, null
  br i1 %allocation_failed, label %trap, label %capacity_ready
capacity_ready:
  store ptr %new_data, ptr %data_ptr
  store i64 %new_capacity, ptr %capacity_ptr
  br label %ready
ready:
  %data = load ptr, ptr %data_ptr
  %offset = mul i64 %length, %element_size
  %destination = getelementptr inbounds i8, ptr %data, i64 %offset
  call void @llvm.memcpy.p0.p0.i64(ptr %destination, ptr %value, i64 %element_size, i1 false)
  store i64 %required, ptr %length_ptr
  ret void
trap:
  call void @llvm.trap()
  unreachable
}

define internal void @vex_array_pop(ptr %array, ptr %result, i64 %element_size) {
entry:
  %length_ptr = getelementptr inbounds %vex.array, ptr %array, i32 0, i32 1
  %length = load i64, ptr %length_ptr
  %empty = icmp eq i64 %length, 0
  br i1 %empty, label %trap, label %remove
remove:
  %index = sub i64 %length, 1
  %element = call ptr @vex_array_get(ptr %array, i64 %index, i64 %element_size)
  call void @llvm.memcpy.p0.p0.i64(ptr %result, ptr %element, i64 %element_size, i1 false)
  store i64 %index, ptr %length_ptr
  ret void
trap:
  call void @llvm.trap()
  unreachable
}

define internal void @vex_array_cleanup() {
entry:
  %head = load ptr, ptr @vex_array_head
  br label %loop
loop:
  %current = phi ptr [ %head, %entry ], [ %next, %body ]
  %finished = icmp eq ptr %current, null
  br i1 %finished, label %done, label %body
body:
  %data_ptr = getelementptr inbounds %vex.array, ptr %current, i32 0, i32 0
  %next_ptr = getelementptr inbounds %vex.array, ptr %current, i32 0, i32 4
  %data = load ptr, ptr %data_ptr
  %next = load ptr, ptr %next_ptr
  call void @free(ptr %data)
  call void @free(ptr %current)
  br label %loop
done:
  store ptr null, ptr @vex_array_head
  ret void
}
)IR";
    }

    void emit_overflow_helper(const char* name, const char* intrinsic) {
        output_ << "define internal i64 @vex_i64_" << name
                << "(i64 %left, i64 %right) {\n"
                << "entry:\n"
                << "  %pair = call { i64, i1 } @" << intrinsic
                << "(i64 %left, i64 %right)\n"
                << "  %result = extractvalue { i64, i1 } %pair, 0\n"
                << "  %overflow = extractvalue { i64, i1 } %pair, 1\n"
                << "  br i1 %overflow, label %trap, label %ok\n"
                << "trap:\n"
                << "  call void @llvm.trap()\n"
                << "  unreachable\n"
                << "ok:\n"
                << "  ret i64 %result\n"
                << "}\n\n";
    }

    void emit_division_helper(const char* name, const char* operation) {
        output_ << "define internal i64 @vex_i64_" << name
                << "(i64 %left, i64 %right) {\n"
                << "entry:\n"
                << "  %zero = icmp eq i64 %right, 0\n";
        if (std::string_view{name} == "rem") {
            output_ << "  br i1 %zero, label %trap, label %overflow_check\n"
                    << "overflow_check:\n"
                    << "  %min = icmp eq i64 %left, -9223372036854775808\n"
                    << "  %negative_one = icmp eq i64 %right, -1\n"
                    << "  %overflow = and i1 %min, %negative_one\n"
                    << "  br i1 %overflow, label %zero_result, label %ok\n"
                    << "zero_result:\n"
                    << "  ret i64 0\n";
        } else {
            output_ << "  %min = icmp eq i64 %left, -9223372036854775808\n"
                    << "  %negative_one = icmp eq i64 %right, -1\n"
                    << "  %overflow = and i1 %min, %negative_one\n"
                    << "  %invalid = or i1 %zero, %overflow\n"
                    << "  br i1 %invalid, label %trap, label %ok\n";
        }
        output_ << "trap:\n"
                << "  call void @llvm.trap()\n"
                << "  unreachable\n"
                << "ok:\n"
                << "  %result = " << operation << " i64 %left, %right\n"
                << "  ret i64 %result\n"
                << "}\n\n";
    }

    void index_values(const IRFunction& function) {
        value_types_.clear();
        for (const IRFunctionParameter& parameter : function.parameters) {
            value_types_.emplace(parameter.value.id, parameter.value);
        }
        for (const IRBlock& block : function.blocks) {
            for (const IRValue& parameter : block.parameters) {
                value_types_.emplace(parameter.id, parameter);
            }
            for (const IRInstruction& instruction : block.instructions) {
                if (instruction.result) {
                    value_types_.emplace(instruction.result->id, *instruction.result);
                }
            }
        }
    }

    void emit_function(const IRFunction& function) {
        current_function_ = &function;
        index_values(function);
        output_ << "define internal " << llvm_return_type(function) << ' '
                << function_symbol(function.name) << '(';
        for (std::size_t index = 0; index < function.parameters.size(); ++index) {
            if (index != 0) output_ << ", ";
            output_ << llvm_type(function.parameters[index].value)
                    << " %arg_" << index;
        }
        output_ << ") {\nentry:\n";

        std::vector<std::pair<IRValueId, IRValue>> ordered_values(
            value_types_.begin(), value_types_.end());
        std::sort(ordered_values.begin(), ordered_values.end(),
            [](const auto& left, const auto& right) {
                return left.first < right.first;
            });
        for (const auto& [id, type] : ordered_values) {
            output_ << "  " << value_slot(id) << " = alloca "
                    << llvm_type(type) << ", align "
                    << (type.type == IRType::Bool ? 1 : 8) << "\n";
        }
        for (std::size_t index = 0; index < function.parameters.size(); ++index) {
            const IRValue& parameter = function.parameters[index].value;
            output_ << "  store " << llvm_type(parameter) << " %arg_" << index
                    << ", ptr " << value_slot(parameter.id) << "\n";
        }
        output_ << "  br label %" << block_label(function.entry_block) << "\n";

        for (const IRBlock& block : function.blocks) {
            output_ << '\n' << block_label(block.id) << ":\n";
            for (const IRInstruction& instruction : block.instructions) {
                emit_instruction(instruction);
                if (has_errors(diagnostics_)) return;
            }
            if (block.terminator) emit_terminator(block);
        }

        for (const IRBlock& block : function.blocks) {
            if (!block.terminator) continue;
            std::visit([&](const auto& terminator) {
                using T = std::decay_t<decltype(terminator)>;
                if constexpr (std::is_same_v<T, IRBranch>) {
                    emit_edge(block.id, 'u', terminator.target, terminator.arguments);
                } else if constexpr (std::is_same_v<T, IRConditionalBranch>) {
                    emit_edge(block.id, 't', terminator.true_target,
                              terminator.true_arguments);
                    emit_edge(block.id, 'f', terminator.false_target,
                              terminator.false_arguments);
                }
            }, block.terminator->data);
        }
        output_ << "}\n\n";
    }

    std::string next_temp() {
        return "%vex_tmp_" + std::to_string(next_temp_++);
    }

    std::string load_value(const IRValueId id) {
        const auto found = value_types_.find(id);
        if (found == value_types_.end()) return "undef";
        const std::string temporary = next_temp();
        output_ << "  " << temporary << " = load " << llvm_type(found->second)
                << ", ptr " << value_slot(id) << "\n";
        return temporary;
    }

    void store_result(const IRInstruction& instruction, const std::string& value) {
        if (!instruction.result) return;
        output_ << "  store " << llvm_type(*instruction.result) << ' '
                << value << ", ptr " << value_slot(instruction.result->id) << "\n";
    }

    void emit_constant(const IRInstruction& instruction) {
        if (!instruction.result) return;
        const std::string result = next_temp();
        switch (instruction.result->type) {
        case IRType::Int: {
            const auto parsed = parse_integer(instruction.immediate);
            if (!parsed) {
                report("LLVM005", "integer constant is outside the supported int64 range",
                       instruction.span);
                return;
            }
            output_ << "  " << result << " = add i64 0, " << *parsed << "\n";
            break;
        }
        case IRType::Float: {
            const auto literal = llvm_float_literal(instruction.immediate);
            if (!literal) {
                report("LLVM006", "floating-point constant is not a finite representable double",
                       instruction.span);
                return;
            }
            output_ << "  " << result << " = fadd double 0.0, " << *literal << "\n";
            break;
        }
        case IRType::Bool:
            if (instruction.immediate != "true" && instruction.immediate != "false") {
                report("LLVM007", "invalid boolean constant spelling", instruction.span);
                return;
            }
            output_ << "  " << result << " = xor i1 "
                    << (instruction.immediate == "true" ? "true" : "false")
                    << ", false\n";
            break;
        case IRType::String: {
            const auto found = string_symbols_.find(&instruction);
            if (found == string_symbols_.end()) {
                report("LLVM004", "string constant was not collected", instruction.span);
                return;
            }
            const auto decoded = decode_string_literal(instruction.immediate);
            if (!decoded) return;
            const std::string pointer = next_temp();
            output_ << "  " << pointer << " = getelementptr inbounds ["
                    << (decoded->size() + 1u) << " x i8], ptr " << found->second
                    << ", i64 0, i64 0\n";
            output_ << "  " << result << ".ptr = insertvalue { ptr, i64 } poison, ptr "
                    << pointer << ", 0\n";
            output_ << "  " << result << ".str = insertvalue { ptr, i64 } "
                    << result << ".ptr, i64 " << decoded->size() << ", 1\n";
            store_result(instruction, result + ".str");
            return;
        }
        case IRType::Void:
            report("LLVM008", "void constant cannot be emitted", instruction.span);
            return;
        case IRType::Array:
            report("LLVM009", "array values must be constructed with array_make", instruction.span);
            return;
        case IRType::Struct:
            report("LLVM010", "structure values must be constructed with struct_make", instruction.span);
            return;
        }
        store_result(instruction, result);
    }

    void emit_integer_comparison(const IRInstruction& instruction,
                                 const std::string& left,
                                 const std::string& right,
                                 const IRType type) {
        const char* predicate = "eq";
        if (instruction.op == IROp::NotEqual) predicate = "ne";
        else if (instruction.op == IROp::Less) predicate = "slt";
        else if (instruction.op == IROp::LessEqual) predicate = "sle";
        else if (instruction.op == IROp::Greater) predicate = "sgt";
        else if (instruction.op == IROp::GreaterEqual) predicate = "sge";
        const char* compare_type = type == IRType::Bool ? "i1" : "i64";
        const std::string result = next_temp();
        output_ << "  " << result << " = icmp " << predicate << ' '
                << compare_type << ' ' << left << ", " << right << "\n";
        store_result(instruction, result);
    }

    void emit_string_comparison(const IRInstruction& instruction,
                                const std::string& left,
                                const std::string& right) {
        const std::string left_pointer = next_temp();
        const std::string left_length = next_temp();
        const std::string right_pointer = next_temp();
        const std::string right_length = next_temp();
        const std::string lengths_equal = next_temp();
        const std::string left_is_shorter = next_temp();
        const std::string compare_length = next_temp();
        const std::string comparison = next_temp();
        const std::string bytes_equal = next_temp();
        const std::string result = next_temp();
        output_ << "  " << left_pointer << " = extractvalue { ptr, i64 } "
                << left << ", 0\n"
                << "  " << left_length << " = extractvalue { ptr, i64 } "
                << left << ", 1\n"
                << "  " << right_pointer << " = extractvalue { ptr, i64 } "
                << right << ", 0\n"
                << "  " << right_length << " = extractvalue { ptr, i64 } "
                << right << ", 1\n"
                << "  " << lengths_equal << " = icmp eq i64 " << left_length
                << ", " << right_length << "\n"
                << "  " << left_is_shorter << " = icmp ult i64 " << left_length
                << ", " << right_length << "\n"
                << "  " << compare_length << " = select i1 " << left_is_shorter
                << ", i64 " << left_length << ", i64 " << right_length << "\n"
                << "  " << comparison << " = call i32 @memcmp(ptr " << left_pointer
                << ", ptr " << right_pointer << ", i64 " << compare_length << ")\n"
                << "  " << bytes_equal << " = icmp eq i32 " << comparison << ", 0\n"
                << "  " << result << " = and i1 " << lengths_equal << ", "
                << bytes_equal << "\n";
        if (instruction.op == IROp::NotEqual) {
            const std::string inverted = next_temp();
            output_ << "  " << inverted << " = xor i1 " << result << ", true\n";
            store_result(instruction, inverted);
        } else {
            store_result(instruction, result);
        }
    }

    void emit_say(const IRInstruction& instruction, const std::string& value) {
        const IRType type = value_types_.at(instruction.operands.front()).type;
        if (type == IRType::Int) {
            output_ << "  call i32 (ptr, ...) @printf(ptr @.vex_fmt_i64, i64 "
                    << value << ")\n";
        } else if (type == IRType::Float) {
            output_ << "  call i32 (ptr, ...) @printf(ptr @.vex_fmt_f64, double "
                    << value << ")\n";
        } else if (type == IRType::Bool) {
            const std::string text = next_temp();
            output_ << "  " << text << " = select i1 " << value
                    << ", ptr @.vex_true, ptr @.vex_false\n"
                    << "  call i32 @puts(ptr " << text << ")\n";
        } else if (type == IRType::String) {
            emit_string_output(value);
        }
    }

    void emit_string_output(const std::string& value) {
        const std::string pointer = next_temp();
        const std::string length = next_temp();
        const std::string index_slot = next_temp();
        const std::size_t label_id = next_label_++;
        const std::string loop = "vex_string_loop_" + std::to_string(label_id);
        const std::string body = "vex_string_body_" + std::to_string(label_id);
        const std::string done = "vex_string_done_" + std::to_string(label_id);
        const std::string index = next_temp();
        const std::string finished = next_temp();
        const std::string byte_pointer = next_temp();
        const std::string byte = next_temp();
        const std::string character = next_temp();
        const std::string ignored = next_temp();
        const std::string next_index = next_temp();

        output_ << "  " << pointer << " = extractvalue { ptr, i64 } "
                << value << ", 0\n"
                << "  " << length << " = extractvalue { ptr, i64 } "
                << value << ", 1\n"
                << "  " << index_slot << " = alloca i64, align 8\n"
                << "  store i64 0, ptr " << index_slot << "\n"
                << "  br label %" << loop << "\n"
                << loop << ":\n"
                << "  " << index << " = load i64, ptr " << index_slot << "\n"
                << "  " << finished << " = icmp uge i64 " << index << ", "
                << length << "\n"
                << "  br i1 " << finished << ", label %" << done
                << ", label %" << body << "\n"
                << body << ":\n"
                << "  " << byte_pointer << " = getelementptr inbounds i8, ptr "
                << pointer << ", i64 " << index << "\n"
                << "  " << byte << " = load i8, ptr " << byte_pointer << "\n"
                << "  " << character << " = zext i8 " << byte << " to i32\n"
                << "  " << ignored << " = call i32 @putchar(i32 " << character << ")\n"
                << "  " << next_index << " = add i64 " << index << ", 1\n"
                << "  store i64 " << next_index << ", ptr " << index_slot << "\n"
                << "  br label %" << loop << "\n"
                << done << ":\n"
                << "  call i32 @putchar(i32 10)\n";
    }

    void emit_instruction(const IRInstruction& instruction) {
        if (instruction.op == IROp::Constant) {
            emit_constant(instruction);
            return;
        }

        std::vector<std::string> operands;
        operands.reserve(instruction.operands.size());
        for (const IRValueId id : instruction.operands) {
            operands.push_back(load_value(id));
        }

        switch (instruction.op) {
        case IROp::Constant:
            return;
        case IROp::Add:
        case IROp::Subtract:
        case IROp::Multiply:
        case IROp::Divide:
        case IROp::Remainder: {
            const IRType type = value_types_.at(instruction.operands.front()).type;
            std::string result;
            if (type == IRType::Int) {
                const char* helper = instruction.op == IROp::Add ? "add" :
                    instruction.op == IROp::Subtract ? "sub" :
                    instruction.op == IROp::Multiply ? "mul" :
                    instruction.op == IROp::Divide ? "div" : "rem";
                result = next_temp();
                output_ << "  " << result << " = call i64 @vex_i64_" << helper
                        << "(i64 " << operands[0] << ", i64 " << operands[1] << ")\n";
            } else {
                const char* operation = instruction.op == IROp::Add ? "fadd" :
                    instruction.op == IROp::Subtract ? "fsub" :
                    instruction.op == IROp::Multiply ? "fmul" : "fdiv";
                result = next_temp();
                output_ << "  " << result << " = " << operation << " double "
                        << operands[0] << ", " << operands[1] << "\n";
            }
            store_result(instruction, result);
            return;
        }
        case IROp::Negate: {
            const IRType type = value_types_.at(instruction.operands.front()).type;
            const std::string result = next_temp();
            if (type == IRType::Int) {
                output_ << "  " << result << " = call i64 @vex_i64_neg(i64 "
                        << operands[0] << ")\n";
            } else {
                output_ << "  " << result << " = fneg double " << operands[0] << "\n";
            }
            store_result(instruction, result);
            return;
        }
        case IROp::LogicalNot: {
            const std::string result = next_temp();
            output_ << "  " << result << " = xor i1 " << operands[0] << ", true\n";
            store_result(instruction, result);
            return;
        }
        case IROp::LogicalAnd:
        case IROp::LogicalOr: {
            const std::string result = next_temp();
            output_ << "  " << result << " = "
                    << (instruction.op == IROp::LogicalAnd ? "and" : "or")
                    << " i1 " << operands[0] << ", " << operands[1] << "\n";
            store_result(instruction, result);
            return;
        }
        case IROp::Equal:
        case IROp::NotEqual: {
            const IRType type = value_types_.at(instruction.operands.front()).type;
            if (type == IRType::String) {
                emit_string_comparison(instruction, operands[0], operands[1]);
            } else if (type == IRType::Float) {
                const std::string result = next_temp();
                output_ << "  " << result << " = fcmp "
                        << (instruction.op == IROp::Equal ? "oeq" : "une")
                        << " double " << operands[0] << ", " << operands[1] << "\n";
                store_result(instruction, result);
            } else {
                emit_integer_comparison(instruction, operands[0], operands[1], type);
            }
            return;
        }
        case IROp::Less:
        case IROp::LessEqual:
        case IROp::Greater:
        case IROp::GreaterEqual: {
            const IRType type = value_types_.at(instruction.operands.front()).type;
            const std::string result = next_temp();
            if (type == IRType::Float) {
                const char* predicate = instruction.op == IROp::Less ? "olt" :
                    instruction.op == IROp::LessEqual ? "ole" :
                    instruction.op == IROp::Greater ? "ogt" : "oge";
                output_ << "  " << result << " = fcmp " << predicate
                        << " double " << operands[0] << ", " << operands[1] << "\n";
            } else {
                const char* predicate = instruction.op == IROp::Less ? "slt" :
                    instruction.op == IROp::LessEqual ? "sle" :
                    instruction.op == IROp::Greater ? "sgt" : "sge";
                output_ << "  " << result << " = icmp " << predicate
                        << " i64 " << operands[0] << ", " << operands[1] << "\n";
            }
            store_result(instruction, result);
            return;
        }
        case IROp::IntToFloat: {
            const std::string result = next_temp();
            output_ << "  " << result << " = sitofp i64 " << operands[0]
                    << " to double\n";
            store_result(instruction, result);
            return;
        }
        case IROp::Call: {
            const std::string result = instruction.result ? next_temp() : std::string{};
            if (instruction.result) {
                output_ << "  " << result << " = call "
                        << llvm_type(*instruction.result) << ' '
                        << function_symbol(instruction.callee) << '(';
            } else {
                output_ << "  call void " << function_symbol(instruction.callee) << '(';
            }
            for (std::size_t index = 0; index < operands.size(); ++index) {
                if (index != 0) output_ << ", ";
                output_ << llvm_type(value_types_.at(instruction.operands[index]))
                        << ' ' << operands[index];
            }
            output_ << ")\n";
            if (instruction.result) store_result(instruction, result);
            return;
        }
        case IROp::Math: {
            const char* symbol = instruction.callee == "abs" ? "@fabs" :
                instruction.callee == "sqrt" ? "@sqrt" :
                instruction.callee == "sin" ? "@sin" :
                instruction.callee == "cos" ? "@cos" : "@pow";
            const std::string result = next_temp();
            output_ << "  " << result << " = call double " << symbol << '(';
            for (std::size_t index = 0; index < operands.size(); ++index) {
                if (index != 0) output_ << ", ";
                output_ << "double " << operands[index];
            }
            output_ << ")\n";
            store_result(instruction, result);
            return;
        }
        case IROp::Say:
            emit_say(instruction, operands.front());
            return;
        case IROp::StructMake: {
            const IRValue& structure_value = *instruction.result;
            const IRStructType& structure = module_.struct_types[structure_value.struct_type_id - 1u];
            std::string aggregate = "poison";
            const std::string type = llvm_type(structure_value);
            for (std::size_t index = 0; index < structure.fields.size(); ++index) {
                const std::string inserted = next_temp();
                const IRValue& field_value = value_types_.at(instruction.operands[index]);
                output_ << "  " << inserted << " = insertvalue " << type << ' '
                        << aggregate << ", " << llvm_type(field_value) << ' '
                        << operands[index] << ", " << index << "\n";
                aggregate = inserted;
            }
            store_result(instruction, aggregate);
            return;
        }
        case IROp::StructGet: {
            const IRValue& object = value_types_.at(instruction.operands[0]);
            const std::size_t index = static_cast<std::size_t>(std::stoul(instruction.immediate));
            const std::string extracted = next_temp();
            output_ << "  " << extracted << " = extractvalue " << llvm_type(object)
                    << ' ' << operands[0] << ", " << index << "\n";
            store_result(instruction, extracted);
            return;
        }
        case IROp::StructSet: {
            const IRValue& object = value_types_.at(instruction.operands[0]);
            const std::size_t index = static_cast<std::size_t>(std::stoul(instruction.immediate));
            const std::string inserted = next_temp();
            const IRValue& field_value = value_types_.at(instruction.operands[1]);
            output_ << "  " << inserted << " = insertvalue " << llvm_type(object)
                    << ' ' << operands[0] << ", " << llvm_type(field_value) << ' '
                    << operands[1] << ", " << index << "\n";
            store_result(instruction, inserted);
            return;
        }
        case IROp::ArrayMake: {
            const IRValue& array = *instruction.result;
            if (array.array_length == 0) {
                const std::string dynamic_array = next_temp();
                const std::size_t element_size = array.element_type == IRType::Bool ? 1u :
                    array.element_type == IRType::String ? 16u : 8u;
                output_ << "  " << dynamic_array << " = call ptr @vex_array_new(i64 "
                        << element_size << ")\n";
                for (const IRValueId operand_id : instruction.operands) {
                    output_ << "  call void @vex_array_push(ptr " << dynamic_array
                            << ", ptr " << value_slot(operand_id) << ", i64 "
                            << element_size << ")\n";
                }
                store_result(instruction, dynamic_array);
                return;
            }
            const std::string type = llvm_type(array);
            std::string aggregate = "poison";
            for (std::size_t index = 0; index < operands.size(); ++index) {
                const std::string next = next_temp();
                output_ << "  " << next << " = insertvalue " << type << ' '
                        << aggregate << ", " << llvm_type(array.element_type) << ' '
                        << operands[index] << ", " << index << "\n";
                aggregate = next;
            }
            store_result(instruction, aggregate);
            return;
        }
        case IROp::ArrayGet: {
            const IRValue& array = value_types_.at(instruction.operands[0]);
            if (array.array_length == 0) {
                const std::size_t element_size =
                    array.element_type == IRType::Bool ? 1u :
                    array.element_type == IRType::String ? 16u : 8u;
                const std::string pointer = next_temp();
                const std::string value = next_temp();
                output_ << "  " << pointer << " = call ptr @vex_array_get(ptr "
                        << operands[0] << ", i64 " << operands[1] << ", i64 "
                        << element_size << ")\n"
                        << "  " << value << " = load " << llvm_type(array.element_type)
                        << ", ptr " << pointer << "\n";
                store_result(instruction, value);
                return;
            }
            const std::string checked = next_temp();
            const std::string pointer = next_temp();
            const std::string value = next_temp();
            output_ << "  " << checked << " = call i64 @vex_checked_array_index(i64 "
                    << operands[1] << ", i64 " << array.array_length << ")\n"
                    << "  " << pointer << " = getelementptr inbounds "
                    << llvm_type(array) << ", ptr " << value_slot(instruction.operands[0])
                    << ", i64 0, i64 " << checked << "\n"
                    << "  " << value << " = load " << llvm_type(array.element_type)
                    << ", ptr " << pointer << "\n";
            store_result(instruction, value);
            return;
        }
        case IROp::ArraySet: {
            const IRValue& array = value_types_.at(instruction.operands[0]);
            const IRValue& result_array = *instruction.result;
            if (array.array_length == 0) {
                const std::size_t element_size =
                    array.element_type == IRType::Bool ? 1u :
                    array.element_type == IRType::String ? 16u : 8u;
                const std::string pointer = next_temp();
                output_ << "  " << pointer << " = call ptr @vex_array_get(ptr "
                        << operands[0] << ", i64 " << operands[1] << ", i64 "
                        << element_size << ")\n"
                        << "  store " << llvm_type(array.element_type) << ' '
                        << operands[2] << ", ptr " << pointer << "\n";
                store_result(instruction, operands[0]);
                return;
            }
            const std::string checked = next_temp();
            const std::string pointer = next_temp();
            output_ << "  store " << llvm_type(array) << ' ' << operands[0]
                    << ", ptr " << value_slot(result_array.id) << "\n"
                    << "  " << checked << " = call i64 @vex_checked_array_index(i64 "
                    << operands[1] << ", i64 " << array.array_length << ")\n"
                    << "  " << pointer << " = getelementptr inbounds "
                    << llvm_type(result_array) << ", ptr " << value_slot(result_array.id)
                    << ", i64 0, i64 " << checked << "\n"
                    << "  store " << llvm_type(array.element_type) << ' ' << operands[2]
                    << ", ptr " << pointer << "\n";
            return;
        }
        case IROp::ArrayLength: {
            const std::string result = next_temp();
            output_ << "  " << result << " = call i64 @vex_array_length(ptr "
                    << operands[0] << ")\n";
            store_result(instruction, result);
            return;
        }
        case IROp::ArrayPush: {
            const IRValue& array = value_types_.at(instruction.operands[0]);
            const std::size_t element_size =
                array.element_type == IRType::Bool ? 1u :
                array.element_type == IRType::String ? 16u : 8u;
            output_ << "  call void @vex_array_push(ptr " << operands[0]
                    << ", ptr " << value_slot(instruction.operands[1])
                    << ", i64 " << element_size << ")\n";
            return;
        }
        case IROp::ArrayPop: {
            const IRValue& array = value_types_.at(instruction.operands[0]);
            const std::size_t element_size =
                array.element_type == IRType::Bool ? 1u :
                array.element_type == IRType::String ? 16u : 8u;
            output_ << "  call void @vex_array_pop(ptr " << operands[0]
                    << ", ptr " << value_slot(instruction.result->id)
                    << ", i64 " << element_size << ")\n";
            return;
        }
        }
    }

    void emit_terminator(const IRBlock& block) {
        std::visit([&](const auto& terminator) {
            using T = std::decay_t<decltype(terminator)>;
            if constexpr (std::is_same_v<T, IRReturn>) {
                if (terminator.value) {
                    const auto found = value_types_.find(*terminator.value);
                    const std::string value = load_value(*terminator.value);
                    output_ << "  ret " << llvm_type(found->second) << ' '
                            << value << "\n";
                } else {
                    output_ << "  ret void\n";
                }
            } else if constexpr (std::is_same_v<T, IRBranch>) {
                output_ << "  br label %" << edge_label(block.id, 'u') << "\n";
            } else if constexpr (std::is_same_v<T, IRConditionalBranch>) {
                const std::string condition = load_value(terminator.condition);
                output_ << "  br i1 " << condition
                        << ", label %" << edge_label(block.id, 't')
                        << ", label %" << edge_label(block.id, 'f') << "\n";
            } else if constexpr (std::is_same_v<T, IRUnreachable>) {
                output_ << "  call void @llvm.trap()\n"
                        << "  unreachable\n";
            }
        }, block.terminator->data);
    }

    void emit_edge(const IRBlockId source, const char arm,
                   const IRBlockId target,
                   const std::vector<IRValueId>& arguments) {
        output_ << '\n' << edge_label(source, arm) << ":\n";
        const auto destination = std::find_if(
            current_function_->blocks.begin(), current_function_->blocks.end(),
            [target](const IRBlock& block) { return block.id == target; });
        if (destination != current_function_->blocks.end()) {
            const std::size_t count = std::min(arguments.size(),
                                               destination->parameters.size());
            std::vector<std::pair<IRValue, std::string>> edge_values;
            edge_values.reserve(count);
            for (std::size_t index = 0; index < count; ++index) {
                const IRValueId argument_id = arguments[index];
                edge_values.emplace_back(value_types_.at(argument_id),
                                         load_value(argument_id));
            }
            for (std::size_t index = 0; index < count; ++index) {
                output_ << "  store " << llvm_type(edge_values[index].first) << ' '
                        << edge_values[index].second << ", ptr "
                        << value_slot(destination->parameters[index].id) << "\n";
            }
        }
        output_ << "  br label %" << block_label(target) << "\n";
    }

    void emit_entry_wrapper() {
        if (!entry_) return;
        output_ << "define i32 @main() {\nentry:\n";
        if (entry_->return_type == IRType::Void) {
            output_ << "  call void " << function_symbol(entry_->name) << "()\n"
                    << "  call void @vex_array_cleanup()\n"
                    << "  ret i32 0\n";
        } else {
            output_ << "  %vex_status = call i64 "
                    << function_symbol(entry_->name) << "()\n"
                    << "  %vex_underflow = icmp slt i64 %vex_status, 0\n"
                    << "  %vex_overflow = icmp sgt i64 %vex_status, 255\n"
                    << "  %vex_invalid = or i1 %vex_underflow, %vex_overflow\n"
                    << "  br i1 %vex_invalid, label %vex_invalid_exit, label %vex_valid_exit\n"
                    << "vex_invalid_exit:\n"
                    << "  call void @llvm.trap()\n"
                    << "  unreachable\n"
                    << "vex_valid_exit:\n"
                    << "  call void @vex_array_cleanup()\n"
                    << "  %vex_exit_code = trunc i64 %vex_status to i32\n"
                    << "  ret i32 %vex_exit_code\n";
        }
        output_ << "}\n";
    }

    const IRModule& module_;
    std::ostringstream output_;
    std::vector<Diagnostic> diagnostics_;
    std::vector<StringGlobal> string_globals_;
    std::unordered_map<const IRInstruction*, std::string> string_symbols_;
    std::unordered_map<IRValueId, IRValue> value_types_;
    const IRFunction* entry_{nullptr};
    const IRFunction* current_function_{nullptr};
    std::size_t next_temp_{0};
    std::size_t next_label_{0};
};

} // namespace

std::string_view LLVMBackend::name() const noexcept {
    return "llvm";
}

BackendResult LLVMBackend::emit(const IRModule& module) const {
    return LLVMEmitter(module).run();
}

} // namespace vex
