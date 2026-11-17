#pragma once

#include "vex/ir.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace vex {

enum class TargetEndianness { Little, Big };

struct TargetLayoutConfig {
    std::uint64_t pointer_size{8};
    std::uint64_t pointer_alignment{8};
    std::uint64_t int_alignment{8};
    std::uint64_t float_alignment{8};
    std::uint64_t maximum_object_size{UINT64_MAX};
    TargetEndianness endianness{TargetEndianness::Little};
};

struct TypeLayout {
    std::uint64_t size{0};
    std::uint64_t alignment{1};
    std::uint64_t stride{0};
};

struct FieldLayout {
    std::string name;
    std::uint64_t offset{0};
    TypeLayout type{};
};

struct StructLayout {
    std::uint64_t size{0};
    std::uint64_t alignment{1};
    std::vector<FieldLayout> fields;
};

// Layout-only enum descriptor used while defining the inline tagged-union ABI.
// It becomes part of the main typed IR model when enum syntax and values land.
struct EnumVariantLayoutDescriptor {
    std::string name;
    std::vector<IRStructField> fields;
};

struct EnumLayoutDescriptor {
    std::uint32_t id{0};
    std::string name;
    std::vector<EnumVariantLayoutDescriptor> variants;
};

struct EnumVariantLayout {
    std::string name;
    std::uint32_t tag{0};
    std::uint64_t payload_size{0};
    std::uint64_t payload_alignment{1};
    std::vector<FieldLayout> fields;
};

struct EnumLayout {
    std::uint64_t size{0};
    std::uint64_t alignment{1};
    std::uint64_t discriminant_size{4};
    std::uint64_t discriminant_alignment{4};
    std::uint64_t payload_offset{0};
    std::uint64_t payload_size{0};
    std::uint64_t payload_alignment{1};
    std::vector<EnumVariantLayout> variants;
};

// Default layout describes the 64-bit targets currently supported by VEX.
class TargetDataLayout {
public:
    explicit TargetDataLayout(TargetLayoutConfig config = {}) noexcept;
    [[nodiscard]] const TargetLayoutConfig& config() const noexcept;
    [[nodiscard]] std::optional<TypeLayout> layout_of(
        const IRValue& value, const IRModule& module, std::string* error = nullptr) const;
    [[nodiscard]] std::optional<TypeLayout> layout_of_array_element(
        const IRValue& array, const IRModule& module, std::string* error = nullptr) const;
    [[nodiscard]] std::optional<StructLayout> layout_of_struct(
        std::uint32_t struct_type_id, const IRModule& module,
        std::string* error = nullptr) const;
    [[nodiscard]] std::optional<EnumLayout> layout_of_enum(
        const EnumLayoutDescriptor& enumeration, const IRModule& module,
        std::string* error = nullptr) const;

private:
    TargetLayoutConfig config_;
};

} // namespace vex
