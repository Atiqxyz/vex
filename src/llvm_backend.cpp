#include "vex/llvm_backend.hpp"
#include "vex/stack_policy.hpp"
#include "vex/target_layout.hpp"

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
    case IRType::Byte: return "i8";
    case IRType::Float: return "double";
    case IRType::Bool: return "i1";
    case IRType::String: return "{ ptr, i64 }";
    case IRType::Array: return "[0 x i8]";
    case IRType::Struct: return "%vex.struct.0";
    case IRType::Enum: return "%vex.enum.0";
    }
    return "void";
}

std::string llvm_struct_type_name(const std::uint32_t id) {
    return "%vex.struct." + std::to_string(id);
}

std::string llvm_enum_type_name(const std::uint32_t id) {
    return "%vex.enum." + std::to_string(id);
}

std::string llvm_type(const IRValue& value) {
    if (value.type == IRType::Struct) return llvm_struct_type_name(value.struct_type_id);
    if (value.type == IRType::Enum) return llvm_enum_type_name(value.enum_type_id);
    if (value.type != IRType::Array) return llvm_type(value.type);
    const std::vector<std::uint32_t> dimensions = ir_array_dimensions(value);
    if (dimensions.front() == 0u) return "ptr";
    const IRValue element = ir_array_element_value(value);
    return "[" + std::to_string(dimensions.front()) + " x " +
           llvm_type(element) + "]";
}

IRValue array_element_value(const IRValue& array) {
    return ir_array_element_value(array);
}

IRValue array_field_value(const IRStructField& field) {
    return ir_struct_field_value(field);
}

std::string llvm_return_type(const IRFunction& function) {
    if (function.return_type == IRType::Struct)
        return llvm_struct_type_name(function.return_struct_type_id);
    if (function.return_type == IRType::Enum)
        return llvm_enum_type_name(function.return_enum_type_id);
    if (function.return_type != IRType::Array)
        return llvm_type(function.return_type);
    std::vector<std::uint32_t> dimensions = function.return_array_dimensions;
    if (dimensions.empty()) dimensions.push_back(function.return_array_length);
    return llvm_type(ir_make_array_value(function.return_element_type,
        std::move(dimensions), function.return_element_struct_type_id, function.span,
        function.return_element_enum_type_id));
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
        emit_string_runtime_helpers();
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
                << "@.vex_file_read_mode = private unnamed_addr constant [3 x i8] c\"rb\\00\", align 1\n"
                << "@.vex_file_write_mode = private unnamed_addr constant [3 x i8] c\"wb\\00\", align 1\n"
                << "%vex.array = type { ptr, i64, i64, i64, ptr }\n"
                << "%vex.string.node = type { ptr, [0 x i8] }\n";
        for (const IREnumType& enumeration : module_.enum_types) {
            std::string error;
            const auto enum_layout = target_layout_.layout_of_enum(
                enumeration.id, module_, &error);
            if (!enum_layout) {
                report("LLVM011", "cannot compute target layout for enum " +
                       enumeration.name + ": " + error);
                continue;
            }
            output_ << llvm_enum_type_name(enumeration.id) << " = type { i32";
            if (enum_layout->payload_size != 0u) {
                const std::uint64_t alignment = enum_layout->payload_alignment;
                const char* alignment_type = alignment >= 8u ? "i64" :
                    alignment >= 4u ? "i32" : alignment >= 2u ? "i16" : "i8";
                output_ << ", [0 x " << alignment_type << "], ["
                        << enum_layout->payload_size << " x i8]";
            }
            output_ << " }\\n";
        }
        for (const IRStructType& structure : module_.struct_types) {
            output_ << llvm_struct_type_name(structure.id) << " = type { ";
            for (std::size_t index = 0; index < structure.fields.size(); ++index) {
                if (index != 0) output_ << ", ";
                const IRStructField& field = structure.fields[index];
                output_ << llvm_type(array_field_value(field));
            }
            output_ << " }\n";
        }
        output_ << "@vex_array_head = internal global ptr null\n"
                << "@vex_string_head = internal global ptr null\n\n";
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
                << "declare ptr @fopen(ptr, ptr)\n"
                << "declare i64 @fread(ptr, i64, i64, ptr)\n"
                << "declare i64 @fwrite(ptr, i64, i64, ptr)\n"
                << "declare i32 @fclose(ptr)\n"
                << "declare i32 @ferror(ptr)\n"
                << "declare ptr @memchr(ptr, i32, i64)\n"
                << "declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1 immarg)\n"
                << "declare { i64, i1 } @llvm.umul.with.overflow.i64(i64, i64)\n"
                << "declare { i64, i1 } @llvm.uadd.with.overflow.i64(i64, i64)\n"
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

    void emit_string_runtime_helpers() {
        output_ << R"IR(
define internal { ptr, i64 } @vex_string_concat({ ptr, i64 } %left, { ptr, i64 } %right) {
entry:
  %left_data = extractvalue { ptr, i64 } %left, 0
  %left_length = extractvalue { ptr, i64 } %left, 1
  %right_data = extractvalue { ptr, i64 } %right, 0
  %right_length = extractvalue { ptr, i64 } %right, 1
  %left_empty = icmp eq i64 %left_length, 0
  br i1 %left_empty, label %return_right, label %check_right_empty
return_right:
  ret { ptr, i64 } %right
check_right_empty:
  %right_empty = icmp eq i64 %right_length, 0
  br i1 %right_empty, label %return_left, label %check_length
return_left:
  ret { ptr, i64 } %left
check_length:
  %total_pair = call { i64, i1 } @llvm.uadd.with.overflow.i64(i64 %left_length, i64 %right_length)
  %total_length = extractvalue { i64, i1 } %total_pair, 0
  %length_overflow = extractvalue { i64, i1 } %total_pair, 1
  br i1 %length_overflow, label %trap, label %check_payload_size
check_payload_size:
  %payload_pair = call { i64, i1 } @llvm.uadd.with.overflow.i64(i64 %total_length, i64 1)
  %payload_size = extractvalue { i64, i1 } %payload_pair, 0
  %payload_overflow = extractvalue { i64, i1 } %payload_pair, 1
  %node_size_pointer = getelementptr %vex.string.node, ptr null, i32 1
  %node_size = ptrtoint ptr %node_size_pointer to i64
  %allocation_pair = call { i64, i1 } @llvm.uadd.with.overflow.i64(i64 %node_size, i64 %payload_size)
  %allocation_size = extractvalue { i64, i1 } %allocation_pair, 0
  %allocation_overflow = extractvalue { i64, i1 } %allocation_pair, 1
  %size_overflow = or i1 %payload_overflow, %allocation_overflow
  br i1 %size_overflow, label %trap, label %allocate
allocate:
  %node = call ptr @malloc(i64 %allocation_size)
  %allocation_failed = icmp eq ptr %node, null
  br i1 %allocation_failed, label %trap, label %register_allocation
register_allocation:
  %next_pointer = getelementptr %vex.string.node, ptr %node, i32 0, i32 0
  %old_head = load ptr, ptr @vex_string_head
  store ptr %old_head, ptr %next_pointer
  %data = getelementptr %vex.string.node, ptr %node, i32 0, i32 1
  store ptr %node, ptr @vex_string_head
  call void @llvm.memcpy.p0.p0.i64(ptr %data, ptr %left_data, i64 %left_length, i1 false)
  %right_destination = getelementptr i8, ptr %data, i64 %left_length
  call void @llvm.memcpy.p0.p0.i64(ptr %right_destination, ptr %right_data, i64 %right_length, i1 false)
  %terminator = getelementptr i8, ptr %data, i64 %total_length
  store i8 0, ptr %terminator
  %result_with_data = insertvalue { ptr, i64 } poison, ptr %data, 0
  %result = insertvalue { ptr, i64 } %result_with_data, i64 %total_length, 1
  ret { ptr, i64 } %result
trap:
  call void @llvm.trap()
  unreachable
}

define internal { ptr, i64 } @vex_string_join(ptr %parts) {
entry:
  %size_ptr = getelementptr inbounds %vex.array, ptr %parts, i32 0, i32 3
  %element_size = load i64, ptr %size_ptr
  %string_element_end = getelementptr { ptr, i64 }, ptr null, i32 1
  %string_element_size = ptrtoint ptr %string_element_end to i64
  %size_ok = icmp eq i64 %element_size, %string_element_size
  br i1 %size_ok, label %read_array, label %trap
read_array:
  %length_ptr = getelementptr inbounds %vex.array, ptr %parts, i32 0, i32 1
  %data_ptr = getelementptr inbounds %vex.array, ptr %parts, i32 0, i32 0
  %count = load i64, ptr %length_ptr
  %items = load ptr, ptr %data_ptr
  %sum_index_slot = alloca i64, align 8
  %total_slot = alloca i64, align 8
  store i64 0, ptr %sum_index_slot
  store i64 0, ptr %total_slot
  br label %sum_loop
sum_loop:
  %sum_index = load i64, ptr %sum_index_slot
  %sum_done = icmp uge i64 %sum_index, %count
  br i1 %sum_done, label %check_empty, label %sum_item
sum_item:
  %sum_offset = mul i64 %sum_index, %string_element_size
  %sum_item_ptr = getelementptr inbounds i8, ptr %items, i64 %sum_offset
  %sum_item_length_ptr = getelementptr inbounds { ptr, i64 }, ptr %sum_item_ptr, i32 0, i32 1
  %sum_item_length = load i64, ptr %sum_item_length_ptr
  %old_total = load i64, ptr %total_slot
  %sum_pair = call { i64, i1 } @llvm.uadd.with.overflow.i64(i64 %old_total, i64 %sum_item_length)
  %new_total = extractvalue { i64, i1 } %sum_pair, 0
  %sum_overflow = extractvalue { i64, i1 } %sum_pair, 1
  br i1 %sum_overflow, label %trap, label %sum_advance
sum_advance:
  store i64 %new_total, ptr %total_slot
  %next_sum_index = add i64 %sum_index, 1
  store i64 %next_sum_index, ptr %sum_index_slot
  br label %sum_loop
check_empty:
  %total_length = load i64, ptr %total_slot
  %empty = icmp eq i64 %total_length, 0
  br i1 %empty, label %return_empty, label %check_payload
return_empty:
  %empty_with_pointer = insertvalue { ptr, i64 } poison, ptr null, 0
  %empty_result = insertvalue { ptr, i64 } %empty_with_pointer, i64 0, 1
  ret { ptr, i64 } %empty_result
check_payload:
  %payload_pair = call { i64, i1 } @llvm.uadd.with.overflow.i64(i64 %total_length, i64 1)
  %payload_size = extractvalue { i64, i1 } %payload_pair, 0
  %payload_overflow = extractvalue { i64, i1 } %payload_pair, 1
  %node_size_pointer = getelementptr %vex.string.node, ptr null, i32 1
  %node_size = ptrtoint ptr %node_size_pointer to i64
  %allocation_pair = call { i64, i1 } @llvm.uadd.with.overflow.i64(i64 %node_size, i64 %payload_size)
  %allocation_size = extractvalue { i64, i1 } %allocation_pair, 0
  %allocation_overflow = extractvalue { i64, i1 } %allocation_pair, 1
  %size_overflow = or i1 %payload_overflow, %allocation_overflow
  br i1 %size_overflow, label %trap, label %allocate
allocate:
  %node = call ptr @malloc(i64 %allocation_size)
  %allocation_failed = icmp eq ptr %node, null
  br i1 %allocation_failed, label %trap, label %register_allocation
register_allocation:
  %next_pointer = getelementptr %vex.string.node, ptr %node, i32 0, i32 0
  %old_head = load ptr, ptr @vex_string_head
  store ptr %old_head, ptr %next_pointer
  %output_data = getelementptr %vex.string.node, ptr %node, i32 0, i32 1
  store ptr %node, ptr @vex_string_head
  %copy_index_slot = alloca i64, align 8
  %output_offset_slot = alloca i64, align 8
  store i64 0, ptr %copy_index_slot
  store i64 0, ptr %output_offset_slot
  br label %copy_loop
copy_loop:
  %copy_index = load i64, ptr %copy_index_slot
  %copy_done = icmp uge i64 %copy_index, %count
  br i1 %copy_done, label %finish, label %copy_item
copy_item:
  %copy_offset = mul i64 %copy_index, %string_element_size
  %copy_item_ptr = getelementptr inbounds i8, ptr %items, i64 %copy_offset
  %copy_item_data = load ptr, ptr %copy_item_ptr
  %copy_item_length_ptr = getelementptr inbounds { ptr, i64 }, ptr %copy_item_ptr, i32 0, i32 1
  %copy_item_length = load i64, ptr %copy_item_length_ptr
  %is_empty_chunk = icmp eq i64 %copy_item_length, 0
  br i1 %is_empty_chunk, label %copy_advance, label %copy_bytes
copy_bytes:
  %output_offset = load i64, ptr %output_offset_slot
  %destination = getelementptr inbounds i8, ptr %output_data, i64 %output_offset
  call void @llvm.memcpy.p0.p0.i64(ptr %destination, ptr %copy_item_data, i64 %copy_item_length, i1 false)
  %next_output_offset = add i64 %output_offset, %copy_item_length
  store i64 %next_output_offset, ptr %output_offset_slot
  br label %copy_advance
copy_advance:
  %old_copy_index = load i64, ptr %copy_index_slot
  %next_copy_index = add i64 %old_copy_index, 1
  store i64 %next_copy_index, ptr %copy_index_slot
  br label %copy_loop
finish:
  %terminator = getelementptr inbounds i8, ptr %output_data, i64 %total_length
  store i8 0, ptr %terminator
  %result_with_data = insertvalue { ptr, i64 } poison, ptr %output_data, 0
  %result = insertvalue { ptr, i64 } %result_with_data, i64 %total_length, 1
  ret { ptr, i64 } %result
trap:
  call void @llvm.trap()
  unreachable
}

define internal void @vex_string_cleanup() {
entry:
  br label %loop
loop:
  %node = load ptr, ptr @vex_string_head
  %finished = icmp eq ptr %node, null
  br i1 %finished, label %done, label %release
release:
  %next_pointer = getelementptr %vex.string.node, ptr %node, i32 0, i32 0
  %next = load ptr, ptr %next_pointer
  store ptr %next, ptr @vex_string_head
  call void @free(ptr %node)
  br label %loop
done:
  ret void
}
)IR";
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
        output_
            << "define internal i8 @vex_int_to_byte(i64 %value) {\n"
            << "entry:\n"
            << "  %negative = icmp slt i64 %value, 0\n"
            << "  %too_large = icmp sgt i64 %value, 255\n"
            << "  %invalid = or i1 %negative, %too_large\n"
            << "  br i1 %invalid, label %trap, label %ok\n"
            << "trap:\n"
            << "  call void @llvm.trap()\n"
            << "  unreachable\n"
            << "ok:\n"
            << "  %result = trunc i64 %value to i8\n"
            << "  ret i8 %result\n"
            << "}\n\n";
        emit_division_helper("div", "sdiv");
        emit_division_helper("rem", "srem");
        emit_shift_helpers();
    }

    void emit_shift_helpers() {
        output_ << R"IR(
define internal i64 @vex_i64_shl(i64 %value, i64 %count) {
entry:
  %negative = icmp slt i64 %count, 0
  %too_large = icmp sge i64 %count, 64
  %invalid = or i1 %negative, %too_large
  br i1 %invalid, label %trap, label %ok
ok:
  %result = shl i64 %value, %count
  ret i64 %result
trap:
  call void @llvm.trap()
  unreachable
}
define internal i64 @vex_i64_shr(i64 %value, i64 %count) {
entry:
  %negative = icmp slt i64 %count, 0
  %too_large = icmp sge i64 %count, 64
  %invalid = or i1 %negative, %too_large
  br i1 %invalid, label %trap, label %ok
ok:
  %result = ashr i64 %value, %count
  ret i64 %result
trap:
  call void @llvm.trap()
  unreachable
}
define internal i8 @vex_u8_shl(i8 %value, i64 %count) {
entry:
  %negative = icmp slt i64 %count, 0
  %too_large = icmp sge i64 %count, 8
  %invalid = or i1 %negative, %too_large
  br i1 %invalid, label %trap, label %ok
ok:
  %shift = trunc i64 %count to i8
  %result = shl i8 %value, %shift
  ret i8 %result
trap:
  call void @llvm.trap()
  unreachable
}
define internal i8 @vex_u8_shr(i8 %value, i64 %count) {
entry:
  %negative = icmp slt i64 %count, 0
  %too_large = icmp sge i64 %count, 8
  %invalid = or i1 %negative, %too_large
  br i1 %invalid, label %trap, label %ok
ok:
  %shift = trunc i64 %count to i8
  %result = lshr i8 %value, %shift
  ret i8 %result
trap:
  call void @llvm.trap()
  unreachable
}
define internal i64 @vex_i64_rotl(i64 %value, i64 %count) {
entry:
  %shift = and i64 %count, 63
  %reverse = sub i64 64, %shift
  %reverse_mod = and i64 %reverse, 63
  %left = shl i64 %value, %shift
  %right = lshr i64 %value, %reverse_mod
  %result = or i64 %left, %right
  ret i64 %result
}
define internal i64 @vex_i64_rotr(i64 %value, i64 %count) {
entry:
  %shift = and i64 %count, 63
  %reverse = sub i64 64, %shift
  %reverse_mod = and i64 %reverse, 63
  %right = lshr i64 %value, %shift
  %left = shl i64 %value, %reverse_mod
  %result = or i64 %right, %left
  ret i64 %result
}
define internal i8 @vex_u8_rotl(i8 %value, i64 %count) {
entry:
  %shift64 = and i64 %count, 7
  %shift = trunc i64 %shift64 to i8
  %reverse = sub i8 8, %shift
  %reverse_mod = and i8 %reverse, 7
  %left = shl i8 %value, %shift
  %right = lshr i8 %value, %reverse_mod
  %result = or i8 %left, %right
  ret i8 %result
}
define internal i8 @vex_u8_rotr(i8 %value, i64 %count) {
entry:
  %shift64 = and i64 %count, 7
  %shift = trunc i64 %shift64 to i8
  %reverse = sub i8 8, %shift
  %reverse_mod = and i8 %reverse, 7
  %right = lshr i8 %value, %shift
  %left = shl i8 %value, %reverse_mod
  %result = or i8 %right, %left
  ret i8 %result
}
)IR";
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
  %header_end = getelementptr %vex.array, ptr null, i32 1
  %header_size = ptrtoint ptr %header_end to i64
  %array = call ptr @malloc(i64 %header_size)
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

define internal i64 @vex_array_capacity(ptr %array, i64 %element_size) {
entry:
  %size_ptr = getelementptr inbounds %vex.array, ptr %array, i32 0, i32 3
  %actual_size = load i64, ptr %size_ptr
  %size_ok = icmp eq i64 %actual_size, %element_size
  br i1 %size_ok, label %read_capacity, label %trap
read_capacity:
  %capacity_ptr = getelementptr inbounds %vex.array, ptr %array, i32 0, i32 2
  %capacity = load i64, ptr %capacity_ptr
  ret i64 %capacity
trap:
  call void @llvm.trap()
  unreachable
}

define internal void @vex_array_reserve(ptr %array, i64 %requested, i64 %element_size) {
entry:
  %size_ptr = getelementptr inbounds %vex.array, ptr %array, i32 0, i32 3
  %actual_size = load i64, ptr %size_ptr
  %size_ok = icmp eq i64 %actual_size, %element_size
  br i1 %size_ok, label %check_negative, label %trap
check_negative:
  %negative = icmp slt i64 %requested, 0
  br i1 %negative, label %trap, label %check_capacity
check_capacity:
  %capacity_ptr = getelementptr inbounds %vex.array, ptr %array, i32 0, i32 2
  %capacity = load i64, ptr %capacity_ptr
  %already_enough = icmp uge i64 %capacity, %requested
  br i1 %already_enough, label %done, label %check_size
check_size:
  %size_pair = call { i64, i1 } @llvm.umul.with.overflow.i64(i64 %requested, i64 %element_size)
  %byte_count = extractvalue { i64, i1 } %size_pair, 0
  %size_overflow = extractvalue { i64, i1 } %size_pair, 1
  br i1 %size_overflow, label %trap, label %allocate
allocate:
  %data_ptr = getelementptr inbounds %vex.array, ptr %array, i32 0, i32 0
  %old_data = load ptr, ptr %data_ptr
  %new_data = call ptr @realloc(ptr %old_data, i64 %byte_count)
  %allocation_failed = icmp eq ptr %new_data, null
  br i1 %allocation_failed, label %trap, label %update
update:
  store ptr %new_data, ptr %data_ptr
  store i64 %requested, ptr %capacity_ptr
  br label %done
done:
  ret void
trap:
  call void @llvm.trap()
  unreachable
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

define internal ptr @vex_byte_slice(ptr %source, i64 %start, i64 %count) {
entry:
  %source_size_ptr = getelementptr inbounds %vex.array, ptr %source, i32 0, i32 3
  %source_size = load i64, ptr %source_size_ptr
  %is_byte_array = icmp eq i64 %source_size, 1
  br i1 %is_byte_array, label %check_start, label %trap
check_start:
  %source_length_ptr = getelementptr inbounds %vex.array, ptr %source, i32 0, i32 1
  %source_length = load i64, ptr %source_length_ptr
  %start_negative = icmp slt i64 %start, 0
  %count_negative = icmp slt i64 %count, 0
  %start_after_end = icmp ugt i64 %start, %source_length
  %bad_start = or i1 %start_negative, %start_after_end
  %bad_range = or i1 %bad_start, %count_negative
  br i1 %bad_range, label %trap, label %check_count
check_count:
  %remaining = sub i64 %source_length, %start
  %count_valid = icmp ule i64 %count, %remaining
  br i1 %count_valid, label %allocate, label %trap
allocate:
  %result = call ptr @vex_array_new(i64 1)
  %empty = icmp eq i64 %count, 0
  br i1 %empty, label %return_result, label %copy
copy:
  call void @vex_array_reserve(ptr %result, i64 %count, i64 1)
  %source_data_ptr = getelementptr inbounds %vex.array, ptr %source, i32 0, i32 0
  %source_data = load ptr, ptr %source_data_ptr
  %source_start = getelementptr inbounds i8, ptr %source_data, i64 %start
  %result_data_ptr = getelementptr inbounds %vex.array, ptr %result, i32 0, i32 0
  %result_data = load ptr, ptr %result_data_ptr
  call void @llvm.memcpy.p0.p0.i64(ptr %result_data, ptr %source_start, i64 %count, i1 false)
  %result_length_ptr = getelementptr inbounds %vex.array, ptr %result, i32 0, i32 1
  store i64 %count, ptr %result_length_ptr
  br label %return_result
return_result:
  ret ptr %result
trap:
  call void @llvm.trap()
  unreachable
}

define internal i64 @vex_byte_read(ptr %array, i64 %offset, i64 %width, i1 %little) {
entry:
  %size_ptr = getelementptr inbounds %vex.array, ptr %array, i32 0, i32 3
  %size = load i64, ptr %size_ptr
  %size_ok = icmp eq i64 %size, 1
  br i1 %size_ok, label %check_offset, label %trap
check_offset:
  %length_ptr = getelementptr inbounds %vex.array, ptr %array, i32 0, i32 1
  %length = load i64, ptr %length_ptr
  %negative = icmp slt i64 %offset, 0
  %past_end = icmp ugt i64 %offset, %length
  %bad_offset = or i1 %negative, %past_end
  br i1 %bad_offset, label %trap, label %check_width
check_width:
  %remaining = sub i64 %length, %offset
  %fits = icmp uge i64 %remaining, %width
  br i1 %fits, label %read_start, label %trap
read_start:
  %data_ptr = getelementptr inbounds %vex.array, ptr %array, i32 0, i32 0
  %data = load ptr, ptr %data_ptr
  %base = getelementptr inbounds i8, ptr %data, i64 %offset
  br label %read_loop
read_loop:
  %index = phi i64 [ 0, %read_start ], [ %next_index, %read_body ]
  %accumulator = phi i64 [ 0, %read_start ], [ %next_value, %read_body ]
  %finished = icmp uge i64 %index, %width
  br i1 %finished, label %read_done, label %read_body
read_body:
  %byte_pointer = getelementptr inbounds i8, ptr %base, i64 %index
  %byte = load i8, ptr %byte_pointer
  %extended = zext i8 %byte to i64
  %last_index = sub i64 %width, 1
  %big_shift_index = sub i64 %last_index, %index
  %shift_index = select i1 %little, i64 %index, i64 %big_shift_index
  %shift = mul i64 %shift_index, 8
  %shifted = shl i64 %extended, %shift
  %next_value = or i64 %accumulator, %shifted
  %next_index = add i64 %index, 1
  br label %read_loop
read_done:
  ret i64 %accumulator
trap:
  call void @llvm.trap()
  unreachable
}

define internal void @vex_byte_write(ptr %array, i64 %offset, i64 %value, i64 %width, i1 %little) {
entry:
  %size_ptr = getelementptr inbounds %vex.array, ptr %array, i32 0, i32 3
  %size = load i64, ptr %size_ptr
  %size_ok = icmp eq i64 %size, 1
  br i1 %size_ok, label %check_value, label %trap
check_value:
  %negative_value = icmp slt i64 %value, 0
  %is_16 = icmp eq i64 %width, 2
  %maximum = select i1 %is_16, i64 65535, i64 4294967295
  %too_large = icmp ugt i64 %value, %maximum
  %invalid_value = or i1 %negative_value, %too_large
  br i1 %invalid_value, label %trap, label %check_offset
check_offset:
  %length_ptr = getelementptr inbounds %vex.array, ptr %array, i32 0, i32 1
  %length = load i64, ptr %length_ptr
  %negative_offset = icmp slt i64 %offset, 0
  %past_end = icmp ugt i64 %offset, %length
  %bad_offset = or i1 %negative_offset, %past_end
  br i1 %bad_offset, label %trap, label %check_width
check_width:
  %remaining = sub i64 %length, %offset
  %fits = icmp uge i64 %remaining, %width
  br i1 %fits, label %write_start, label %trap
write_start:
  %data_ptr = getelementptr inbounds %vex.array, ptr %array, i32 0, i32 0
  %data = load ptr, ptr %data_ptr
  %base = getelementptr inbounds i8, ptr %data, i64 %offset
  br label %write_loop
write_loop:
  %index = phi i64 [ 0, %write_start ], [ %next_index, %write_body ]
  %finished = icmp uge i64 %index, %width
  br i1 %finished, label %write_done, label %write_body
write_body:
  %last_index = sub i64 %width, 1
  %big_shift_index = sub i64 %last_index, %index
  %shift_index = select i1 %little, i64 %index, i64 %big_shift_index
  %shift = mul i64 %shift_index, 8
  %shifted = lshr i64 %value, %shift
  %byte = trunc i64 %shifted to i8
  %byte_pointer = getelementptr inbounds i8, ptr %base, i64 %index
  store i8 %byte, ptr %byte_pointer
  %next_index = add i64 %index, 1
  br label %write_loop
write_done:
  ret void
trap:
  call void @llvm.trap()
  unreachable
}

define internal i64 @vex_file_read({ ptr, i64 } %path, ptr %array) {
entry:
  %path_data = extractvalue { ptr, i64 } %path, 0
  %path_length = extractvalue { ptr, i64 } %path, 1
  %empty_path = icmp eq i64 %path_length, 0
  %embedded_nul = call ptr @memchr(ptr %path_data, i32 0, i64 %path_length)
  %has_nul = icmp ne ptr %embedded_nul, null
  %bad_path = or i1 %empty_path, %has_nul
  br i1 %bad_path, label %invalid_path, label %open_file
open_file:
  %mode = getelementptr inbounds [3 x i8], ptr @.vex_file_read_mode, i64 0, i64 0
  %file = call ptr @fopen(ptr %path_data, ptr %mode)
  %open_failed = icmp eq ptr %file, null
  br i1 %open_failed, label %invalid_path, label %check_array
check_array:
  %size_ptr = getelementptr inbounds %vex.array, ptr %array, i32 0, i32 3
  %element_size = load i64, ptr %size_ptr
  %byte_array = icmp eq i64 %element_size, 1
  br i1 %byte_array, label %setup, label %trap
setup:
  %length_ptr = getelementptr inbounds %vex.array, ptr %array, i32 0, i32 1
  %original_length = load i64, ptr %length_ptr
  br label %read_loop
read_loop:
  %current_length = load i64, ptr %length_ptr
  %requested_pair = call { i64, i1 } @llvm.uadd.with.overflow.i64(i64 %current_length, i64 4096)
  %requested = extractvalue { i64, i1 } %requested_pair, 0
  %overflow = extractvalue { i64, i1 } %requested_pair, 1
  %signed_limit_exceeded = icmp ugt i64 %requested, 9223372036854775807
  %request_invalid = or i1 %overflow, %signed_limit_exceeded
  br i1 %request_invalid, label %read_error, label %choose_capacity
choose_capacity:
  %capacity_ptr = getelementptr inbounds %vex.array, ptr %array, i32 0, i32 2
  %capacity = load i64, ptr %capacity_ptr
  %capacity_zero = icmp eq i64 %capacity, 0
  %double_pair = call { i64, i1 } @llvm.umul.with.overflow.i64(i64 %capacity, i64 2)
  %doubled = extractvalue { i64, i1 } %double_pair, 0
  %double_overflow = extractvalue { i64, i1 } %double_pair, 1
  %safe_doubled = select i1 %double_overflow, i64 %requested, i64 %doubled
  %initial_or_doubled = select i1 %capacity_zero, i64 4096, i64 %safe_doubled
  %growth_undersized = icmp ult i64 %initial_or_doubled, %requested
  %growth_capacity = select i1 %growth_undersized, i64 %requested, i64 %initial_or_doubled
  %already_enough = icmp uge i64 %capacity, %requested
  %target_capacity = select i1 %already_enough, i64 %capacity, i64 %growth_capacity
  br label %reserve
reserve:
  call void @vex_array_reserve(ptr %array, i64 %target_capacity, i64 1)
  %data_ptr = getelementptr inbounds %vex.array, ptr %array, i32 0, i32 0
  %data = load ptr, ptr %data_ptr
  %destination = getelementptr inbounds i8, ptr %data, i64 %current_length
  %count = call i64 @fread(ptr %destination, i64 1, i64 4096, ptr %file)
  %new_length = add i64 %current_length, %count
  store i64 %new_length, ptr %length_ptr
  %io_error = call i32 @ferror(ptr %file)
  %has_error = icmp ne i32 %io_error, 0
  br i1 %has_error, label %read_error, label %check_eof
check_eof:
  %full_chunk = icmp eq i64 %count, 4096
  br i1 %full_chunk, label %read_loop, label %close_success
close_success:
  %close_status = call i32 @fclose(ptr %file)
  %close_ok = icmp eq i32 %close_status, 0
  br i1 %close_ok, label %success, label %close_error
success:
  %final_length = load i64, ptr %length_ptr
  %appended = sub i64 %final_length, %original_length
  ret i64 %appended
read_error:
  store i64 %original_length, ptr %length_ptr
  %ignored_close = call i32 @fclose(ptr %file)
  ret i64 -1
close_error:
  store i64 %original_length, ptr %length_ptr
  ret i64 -1
invalid_path:
  ret i64 -1
trap:
  call void @llvm.trap()
  unreachable
}

define internal i64 @vex_file_write({ ptr, i64 } %path, ptr %array) {
entry:
  %path_data = extractvalue { ptr, i64 } %path, 0
  %path_length = extractvalue { ptr, i64 } %path, 1
  %empty_path = icmp eq i64 %path_length, 0
  %embedded_nul = call ptr @memchr(ptr %path_data, i32 0, i64 %path_length)
  %has_nul = icmp ne ptr %embedded_nul, null
  %bad_path = or i1 %empty_path, %has_nul
  br i1 %bad_path, label %invalid_path, label %open_file
open_file:
  %mode = getelementptr inbounds [3 x i8], ptr @.vex_file_write_mode, i64 0, i64 0
  %file = call ptr @fopen(ptr %path_data, ptr %mode)
  %open_failed = icmp eq ptr %file, null
  br i1 %open_failed, label %invalid_path, label %check_array
check_array:
  %size_ptr = getelementptr inbounds %vex.array, ptr %array, i32 0, i32 3
  %element_size = load i64, ptr %size_ptr
  %byte_array = icmp eq i64 %element_size, 1
  br i1 %byte_array, label %setup, label %trap
setup:
  %length_ptr = getelementptr inbounds %vex.array, ptr %array, i32 0, i32 1
  %length = load i64, ptr %length_ptr
  %data_ptr = getelementptr inbounds %vex.array, ptr %array, i32 0, i32 0
  %data = load ptr, ptr %data_ptr
  br label %write_loop
write_loop:
  %offset = phi i64 [ 0, %setup ], [ %next_offset, %continue_write ]
  %done = icmp uge i64 %offset, %length
  br i1 %done, label %close_success, label %write_body
write_body:
  %remaining = sub i64 %length, %offset
  %source = getelementptr inbounds i8, ptr %data, i64 %offset
  %count = call i64 @fwrite(ptr %source, i64 1, i64 %remaining, ptr %file)
  %no_progress = icmp eq i64 %count, 0
  br i1 %no_progress, label %write_error, label %continue_write
continue_write:
  %next_offset = add i64 %offset, %count
  br label %write_loop
close_success:
  %close_status = call i32 @fclose(ptr %file)
  %close_ok = icmp eq i32 %close_status, 0
  br i1 %close_ok, label %success, label %close_error
success:
  ret i64 %length
write_error:
  %ignored_close = call i32 @fclose(ptr %file)
  ret i64 -1
close_error:
  ret i64 -1
invalid_path:
  ret i64 -1
trap:
  call void @llvm.trap()
  unreachable
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
        const std::string allocation_count = emit_nested_fixed_stack_guard(function);

        std::vector<std::pair<IRValueId, IRValue>> ordered_values(
            value_types_.begin(), value_types_.end());
        std::sort(ordered_values.begin(), ordered_values.end(),
            [](const auto& left, const auto& right) {
                return left.first < right.first;
            });
        for (const auto& [id, type] : ordered_values) {
            std::string layout_error;
            const auto type_layout = target_layout_.layout_of(type, module_, &layout_error);
            if (!type_layout) {
                report("LLVM009", "cannot compute target alignment for value: " + layout_error,
                       type.span);
                return;
            }
            output_ << "  " << value_slot(id) << " = alloca " << llvm_type(type);
            if (!allocation_count.empty()) output_ << ", i64 " << allocation_count;
            output_ << ", align " << type_layout->alignment << "\n";
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

    std::string emit_nested_fixed_stack_guard(const IRFunction& function) {
        const auto values = ir_function_nested_fixed_stack_values(function, module_);
        if (values.empty()) return {};
        std::string valid = "true";
        std::string total = "0";
        for (const IRValue& value : values) {
            std::string layout_error;
            const auto expected_layout = target_layout_.layout_of(value, module_, &layout_error);
            if (!expected_layout) {
                report("LLVM009", "cannot compute nested aggregate layout: " + layout_error,
                       value.span);
                return {};
            }
            const std::string size = emit_type_size(value);
            const std::string target_size_ok = next_temp();
            output_ << "  " << target_size_ok << " = icmp eq i64 " << size
                    << ", " << expected_layout->size << "\n";
            const std::string object_ok = next_temp();
            output_ << "  " << object_ok << " = icmp ule i64 " << size
                    << ", " << max_nested_fixed_aggregate_bytes << "\n";
            const std::string object_valid = next_temp();
            const std::string size_matches_target = next_temp();
            output_ << "  " << size_matches_target << " = and i1 " << target_size_ok
                    << ", " << object_ok << "\n";
            output_ << "  " << object_valid << " = and i1 " << valid << ", "
                    << size_matches_target << "\n";
            valid = object_valid;
            const std::string pair = next_temp();
            const std::string next_total = next_temp();
            const std::string overflow = next_temp();
            output_ << "  " << pair << " = call { i64, i1 } @llvm.uadd.with.overflow.i64(i64 "
                    << total << ", i64 " << size << ")\n"
                    << "  " << next_total << " = extractvalue { i64, i1 } " << pair << ", 0\n"
                    << "  " << overflow << " = extractvalue { i64, i1 } " << pair << ", 1\n";
            const std::string no_overflow = next_temp();
            output_ << "  " << no_overflow << " = xor i1 " << overflow << ", true\n";
            const std::string within = next_temp();
            output_ << "  " << within << " = icmp ule i64 " << next_total
                    << ", " << max_function_nested_fixed_stack_bytes << "\n";
            const std::string sum_valid = next_temp();
            output_ << "  " << sum_valid << " = and i1 " << no_overflow << ", " << within << "\n";
            const std::string combined = next_temp();
            output_ << "  " << combined << " = and i1 " << valid << ", " << sum_valid << "\n";
            valid = combined;
            total = next_total;
        }
        const std::string count = "%vex_stack_alloc_count";
        output_ << "  " << count << " = select i1 " << valid << ", i64 1, i64 0\n"
                << "  br i1 " << valid << ", label %vex_stack_alloc, label %vex_stack_trap\n"
                << "vex_stack_trap:\n  call void @llvm.trap()\n  unreachable\n"
                << "vex_stack_alloc:\n";
        return count;
    }

    std::string next_temp() {
        return "%vex_tmp_" + std::to_string(next_temp_++);
    }

    std::string emit_type_size(const IRValue& value) {
        const std::string next = next_temp();
        const std::string size = next_temp();
        const std::string type = llvm_type(value);
        output_ << "  " << next << " = getelementptr " << type
                << ", ptr null, i64 1\n"
                << "  " << size << " = ptrtoint ptr " << next << " to i64\n";
        return size;
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
        case IRType::Byte: {
            std::uint16_t value = 0;
            const char* begin = instruction.immediate.data();
            const char* end = begin + instruction.immediate.size();
            const auto parsed = std::from_chars(begin, end, value, 10);
            if (parsed.ec != std::errc{} || parsed.ptr != end || value > 255u) {
                report("LLVM008", "byte constant must be in the range 0 through 255",
                       instruction.span);
                return;
            }
            output_ << "  " << result << " = add i8 0, " << value << "\n";
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
        case IRType::Enum:
            report("LLVM010", "enum values must be constructed with enum_make", instruction.span);
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
        if (type == IRType::Byte) {
            if (instruction.op == IROp::Less) predicate = "ult";
            else if (instruction.op == IROp::LessEqual) predicate = "ule";
            else if (instruction.op == IROp::Greater) predicate = "ugt";
            else if (instruction.op == IROp::GreaterEqual) predicate = "uge";
        }
        const char* compare_type = type == IRType::Bool ? "i1" :
            type == IRType::Byte ? "i8" : "i64";
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
        } else if (type == IRType::Byte) {
            const std::string widened = next_temp();
            output_ << "  " << widened << " = zext i8 " << value << " to i64\n"
                    << "  call i32 (ptr, ...) @printf(ptr @.vex_fmt_i64, i64 "
                    << widened << ")\n";
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
        case IROp::StringConcat: {
            const std::string result = next_temp();
            output_ << "  " << result
                    << " = call { ptr, i64 } @vex_string_concat({ ptr, i64 } "
                    << operands[0] << ", { ptr, i64 } " << operands[1] << ")\n";
            store_result(instruction, result);
            return;
        }
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
        case IROp::BitwiseAnd:
        case IROp::BitwiseOr:
        case IROp::BitwiseXor: {
            const IRType type = value_types_.at(instruction.operands.front()).type;
            const char* operation = instruction.op == IROp::BitwiseAnd ? "and" :
                instruction.op == IROp::BitwiseOr ? "or" : "xor";
            const std::string result = next_temp();
            output_ << "  " << result << " = " << operation << ' '
                    << llvm_type(type) << ' ' << operands[0] << ", "
                    << operands[1] << "\n";
            store_result(instruction, result);
            return;
        }
        case IROp::BitwiseNot: {
            const IRType type = value_types_.at(instruction.operands.front()).type;
            const std::string result = next_temp();
            output_ << "  " << result << " = xor " << llvm_type(type) << ' '
                    << operands[0] << ", -1\n";
            store_result(instruction, result);
            return;
        }
        case IROp::ShiftLeft:
        case IROp::ShiftRight:
        case IROp::RotateLeft:
        case IROp::RotateRight: {
            const IRType type = value_types_.at(instruction.operands.front()).type;
            const char* operation = instruction.op == IROp::ShiftLeft ? "shl" :
                instruction.op == IROp::ShiftRight ? "shr" :
                instruction.op == IROp::RotateLeft ? "rotl" : "rotr";
            const std::string result = next_temp();
            output_ << "  " << result << " = call " << llvm_type(type) << " @"
                    << (type == IRType::Byte ? "vex_u8_" : "vex_i64_") << operation
                    << '(' << llvm_type(type) << ' ' << operands[0]
                    << ", i64 " << operands[1] << ")\n";
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
            if (type == IRType::Float) {
                const std::string result = next_temp();
                const char* predicate = instruction.op == IROp::Less ? "olt" :
                    instruction.op == IROp::LessEqual ? "ole" :
                    instruction.op == IROp::Greater ? "ogt" : "oge";
                output_ << "  " << result << " = fcmp " << predicate
                        << " double " << operands[0] << ", " << operands[1] << "\n";
                store_result(instruction, result);
            } else {
                emit_integer_comparison(instruction, operands[0], operands[1], type);
            }
            return;
        }
        case IROp::IntToFloat: {
            const std::string result = next_temp();
            output_ << "  " << result << " = sitofp i64 " << operands[0]
                    << " to double\n";
            store_result(instruction, result);
            return;
        }
        case IROp::IntToByteChecked: {
            const std::string result = next_temp();
            output_ << "  " << result << " = call i8 @vex_int_to_byte(i64 "
                    << operands[0] << ")\n";
            store_result(instruction, result);
            return;
        }
        case IROp::ByteToInt: {
            const std::string result = next_temp();
            output_ << "  " << result << " = zext i8 " << operands[0]
                    << " to i64\n";
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
            const IRValue element = array_element_value(array);
            if (array.array_length == 0) {
                const std::string element_size = emit_type_size(element);
                const std::string dynamic_array = next_temp();
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
            const std::string element_type = llvm_type(element);
            std::string aggregate = "poison";
            for (std::size_t index = 0; index < operands.size(); ++index) {
                const std::string next = next_temp();
                output_ << "  " << next << " = insertvalue " << type << ' '
                        << aggregate << ", " << element_type << ' '
                        << operands[index] << ", " << index << "\n";
                aggregate = next;
            }
            store_result(instruction, aggregate);
            return;
        }
        case IROp::ArrayGet: {
            const IRValue& array = value_types_.at(instruction.operands[0]);
            const IRValue element = array_element_value(array);
            const std::string element_type = llvm_type(element);
            if (array.array_length == 0) {
                const std::string element_size = emit_type_size(element);
                const std::string pointer = next_temp();
                const std::string value = next_temp();
                output_ << "  " << pointer << " = call ptr @vex_array_get(ptr "
                        << operands[0] << ", i64 " << operands[1] << ", i64 "
                        << element_size << ")\n"
                        << "  " << value << " = load " << element_type
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
                    << "  " << value << " = load " << element_type
                    << ", ptr " << pointer << "\n";
            store_result(instruction, value);
            return;
        }
        case IROp::ArraySet: {
            const IRValue& array = value_types_.at(instruction.operands[0]);
            const IRValue& result_array = *instruction.result;
            const IRValue element = array_element_value(array);
            const std::string element_type = llvm_type(element);
            if (array.array_length == 0) {
                const std::string element_size = emit_type_size(element);
                const std::string pointer = next_temp();
                output_ << "  " << pointer << " = call ptr @vex_array_get(ptr "
                        << operands[0] << ", i64 " << operands[1] << ", i64 "
                        << element_size << ")\n"
                        << "  store " << element_type << ' '
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
                    << "  store " << element_type << ' ' << operands[2]
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
        case IROp::StringLength: {
            const std::string result = next_temp();
            output_ << "  " << result << " = extractvalue { ptr, i64 } "
                    << operands[0] << ", 1\n";
            store_result(instruction, result);
            return;
        }
        case IROp::StringJoin: {
            const std::string result = next_temp();
            output_ << "  " << result << " = call { ptr, i64 } @vex_string_join(ptr "
                    << operands[0] << ")\n";
            store_result(instruction, result);
            return;
        }
        case IROp::ArrayCapacity: {
            const IRValue& array = value_types_.at(instruction.operands[0]);
            const IRValue element = array_element_value(array);
            const std::string element_size = emit_type_size(element);
            const std::string result = next_temp();
            output_ << "  " << result << " = call i64 @vex_array_capacity(ptr "
                    << operands[0] << ", i64 " << element_size << ")\n";
            store_result(instruction, result);
            return;
        }
        case IROp::ArrayReserve: {
            const IRValue& array = value_types_.at(instruction.operands[0]);
            const IRValue element = array_element_value(array);
            const std::string element_size = emit_type_size(element);
            output_ << "  call void @vex_array_reserve(ptr " << operands[0]
                    << ", i64 " << operands[1] << ", i64 " << element_size << ")\n";
            return;
        }
        case IROp::ByteSlice: {
            const std::string result = next_temp();
            output_ << "  " << result << " = call ptr @vex_byte_slice(ptr "
                    << operands[0] << ", i64 " << operands[1] << ", i64 "
                    << operands[2] << ")\n";
            store_result(instruction, result);
            return;
        }
        case IROp::ByteRead:
        case IROp::ByteWrite: {
            const std::string width = instruction.immediate.starts_with("2") ? "2" : "4";
            const std::string little = instruction.immediate.ends_with("le") ? "true" : "false";
            if (instruction.op == IROp::ByteRead) {
                const std::string result = next_temp();
                output_ << "  " << result << " = call i64 @vex_byte_read(ptr "
                        << operands[0] << ", i64 " << operands[1] << ", i64 "
                        << width << ", i1 " << little << ")\n";
                store_result(instruction, result);
            } else {
                output_ << "  call void @vex_byte_write(ptr " << operands[0]
                        << ", i64 " << operands[1] << ", i64 " << operands[2]
                        << ", i64 " << width << ", i1 " << little << ")\n";
            }
            return;
        }
        case IROp::FileRead:
        case IROp::FileWrite: {
            const std::string result = next_temp();
            output_ << "  " << result << " = call i64 @"
                    << (instruction.op == IROp::FileRead ? "vex_file_read" : "vex_file_write")
                    << "({ ptr, i64 } " << operands[0] << ", ptr " << operands[1] << ")\n";
            store_result(instruction, result);
            return;
        }
        case IROp::ArrayPush: {
            const IRValue& array = value_types_.at(instruction.operands[0]);
            const IRValue element = array_element_value(array);
            const std::string element_size = emit_type_size(element);
            output_ << "  call void @vex_array_push(ptr " << operands[0]
                    << ", ptr " << value_slot(instruction.operands[1])
                    << ", i64 " << element_size << ")\n";
            return;
        }
        case IROp::ArrayPop: {
            const IRValue& array = value_types_.at(instruction.operands[0]);
            const IRValue element = array_element_value(array);
            const std::string element_size = emit_type_size(element);
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
                    << "  call void @vex_string_cleanup()\n"
                    << "  ret i32 0\n";
        } else {
            output_ << "  %vex_status = call i64 "
                    << function_symbol(entry_->name) << "()\n"
                    << "  %vex_underflow = icmp slt i64 %vex_status, 0\n"
                    << "  %vex_overflow = icmp sgt i64 %vex_status, 255\n"
                    << "  %vex_invalid = or i1 %vex_underflow, %vex_overflow\n"
                    << "  br i1 %vex_invalid, label %vex_invalid_exit, label %vex_valid_exit\n"
                    << "vex_invalid_exit:\n"
                    << "  call void @vex_array_cleanup()\n"
                    << "  call void @vex_string_cleanup()\n"
                    << "  call void @llvm.trap()\n"
                    << "  unreachable\n"
                    << "vex_valid_exit:\n"
                    << "  call void @vex_array_cleanup()\n"
                    << "  call void @vex_string_cleanup()\n"
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
    TargetDataLayout target_layout_;
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
