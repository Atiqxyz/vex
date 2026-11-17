#include "vex/target_layout.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace vex {
namespace {

bool is_power_of_two(const std::uint64_t value) noexcept {
    return value != 0u && (value & (value - 1u)) == 0u;
}

std::optional<std::uint64_t> checked_add(
    const std::uint64_t left, const std::uint64_t right) noexcept {
    if (left > std::numeric_limits<std::uint64_t>::max() - right)
        return std::nullopt;
    return left + right;
}

std::optional<std::uint64_t> checked_multiply(
    const std::uint64_t left, const std::uint64_t right) noexcept {
    if (right != 0u && left > std::numeric_limits<std::uint64_t>::max() / right)
        return std::nullopt;
    return left * right;
}

std::optional<std::uint64_t> align_up(
    const std::uint64_t value, const std::uint64_t alignment) noexcept {
    if (!is_power_of_two(alignment)) return std::nullopt;
    const std::uint64_t remainder = value & (alignment - 1u);
    if (remainder == 0u) return value;
    return checked_add(value, alignment - remainder);
}

class LayoutComputer {
public:
    LayoutComputer(const TargetLayoutConfig& config, const IRModule& module,
                   std::string* error)
        : config_(config), module_(module), error_(error) {}

    std::optional<TypeLayout> run(const IRValue& value) {
        if (!valid_config()) return std::nullopt;
        return layout_of(value);
    }

    std::optional<StructLayout> run_struct(const std::uint32_t id) {
        if (!valid_config()) return std::nullopt;
        return layout_struct(id);
    }

    std::optional<TypeLayout> run_array_element(const IRValue& array) {
        if (!valid_config()) return std::nullopt;
        if (array.type != IRType::Array) {
            fail("array element layout requires an array type");
            return std::nullopt;
        }
        if (!layout_of(array)) return std::nullopt;
        return layout_of(ir_array_element_value(array));
    }

    std::optional<EnumLayout> run_enum(const EnumLayoutDescriptor& enumeration) {
        if (!valid_config()) return std::nullopt;
        if (enumeration.id == 0u || enumeration.name.empty() ||
            enumeration.variants.empty() || enumeration.variants.size() > UINT32_MAX) {
            fail("enum layout descriptor requires a name, nonzero identity, and at least one variant");
            return std::nullopt;
        }

        EnumLayout result;
        std::vector<std::string> variant_names;
        std::uint64_t maximum_payload_size = 0u;
        std::uint64_t maximum_payload_alignment = 1u;
        for (std::size_t variant_index = 0; variant_index < enumeration.variants.size(); ++variant_index) {
            const EnumVariantLayoutDescriptor& variant = enumeration.variants[variant_index];
            if (variant.name.empty() ||
                std::find(variant_names.begin(), variant_names.end(), variant.name) != variant_names.end()) {
                fail("enum variant names must be nonempty and unique");
                return std::nullopt;
            }
            variant_names.push_back(variant.name);

            EnumVariantLayout variant_layout;
            variant_layout.name = variant.name;
            variant_layout.tag = static_cast<std::uint32_t>(variant_index);
            std::uint64_t cursor = 0u;
            std::uint64_t alignment = 1u;
            std::vector<std::string> field_names;
            for (const IRStructField& field : variant.fields) {
                if (field.name.empty() ||
                    std::find(field_names.begin(), field_names.end(), field.name) != field_names.end()) {
                    fail("enum variant payload field names must be nonempty and unique");
                    return std::nullopt;
                }
                field_names.push_back(field.name);
                const auto field_layout = layout_of(ir_struct_field_value(field));
                if (!field_layout) return std::nullopt;
                const auto offset = align_up(cursor, field_layout->alignment);
                if (!offset) {
                    fail("enum variant payload field offset overflows the target size type");
                    return std::nullopt;
                }
                const auto end = checked_add(*offset, field_layout->size);
                if (!end || *end > config_.maximum_object_size) {
                    fail("enum variant payload exceeds the target object-size limit");
                    return std::nullopt;
                }
                variant_layout.fields.push_back(FieldLayout{field.name, *offset, *field_layout});
                cursor = *end;
                alignment = std::max(alignment, field_layout->alignment);
            }
            const auto payload_size = align_up(cursor, alignment);
            if (!payload_size || *payload_size > config_.maximum_object_size) {
                fail("enum variant payload tail padding exceeds the target object-size limit");
                return std::nullopt;
            }
            variant_layout.payload_size = *payload_size;
            variant_layout.payload_alignment = alignment;
            maximum_payload_size = std::max(maximum_payload_size, *payload_size);
            maximum_payload_alignment = std::max(maximum_payload_alignment, alignment);
            result.variants.push_back(std::move(variant_layout));
        }

        const auto payload_offset = align_up(result.discriminant_size,
                                             maximum_payload_alignment);
        if (!payload_offset) {
            fail("enum payload offset overflows the target alignment");
            return std::nullopt;
        }
        const auto payload_end = checked_add(*payload_offset, maximum_payload_size);
        if (!payload_end) {
            fail("enum size overflows the target size type");
            return std::nullopt;
        }
        result.alignment = std::max(result.discriminant_alignment,
                                    maximum_payload_alignment);
        const auto total_size = align_up(*payload_end, result.alignment);
        if (!total_size || *total_size > config_.maximum_object_size) {
            fail("enum aggregate exceeds the target object-size limit");
            return std::nullopt;
        }
        result.payload_offset = *payload_offset;
        result.payload_size = maximum_payload_size;
        result.payload_alignment = maximum_payload_alignment;
        result.size = *total_size;
        return result;
    }

private:
    bool valid_config() {
        if (!is_power_of_two(config_.pointer_alignment) ||
            !is_power_of_two(config_.int_alignment) ||
            !is_power_of_two(config_.float_alignment) ||
            (config_.pointer_size != 4u && config_.pointer_size != 8u) ||
            config_.pointer_alignment > config_.maximum_object_size ||
            config_.maximum_object_size == 0u) {
            fail("target data-layout configuration is unsupported or invalid");
            return false;
        }
        return true;
    }

    void fail(const char* message) {
        if (error_ && error_->empty()) *error_ = message;
    }

    std::optional<TypeLayout> finish(
        const std::uint64_t size, const std::uint64_t alignment) {
        const auto stride = align_up(size, alignment);
        if (!stride || size > config_.maximum_object_size ||
            *stride > config_.maximum_object_size) {
            fail("type layout exceeds the target object-size limit");
            return std::nullopt;
        }
        return TypeLayout{size, alignment, *stride};
    }

    std::optional<TypeLayout> layout_of(const IRValue& value) {
        switch (value.type) {
        case IRType::Void:
            fail("void has no object layout");
            return std::nullopt;
        case IRType::Int:
            return finish(8u, config_.int_alignment);
        case IRType::Byte:
        case IRType::Bool:
            return finish(1u, 1u);
        case IRType::Float:
            return finish(8u, config_.float_alignment);
        case IRType::String: {
            // VEX string is { pointer, uint64 byte_length }.
            const auto length_offset = align_up(config_.pointer_size, config_.int_alignment);
            if (!length_offset) {
                fail("string descriptor alignment overflow");
                return std::nullopt;
            }
            const auto end = checked_add(*length_offset, 8u);
            if (!end) {
                fail("string descriptor size overflow");
                return std::nullopt;
            }
            return finish(*end, std::max(config_.pointer_alignment, config_.int_alignment));
        }
        case IRType::Struct:
            return layout_struct_type(value.struct_type_id);
        case IRType::Array:
            return layout_array(value);
        }
        fail("unknown IR type has no target layout");
        return std::nullopt;
    }

    std::optional<TypeLayout> layout_array(const IRValue& value) {
        const std::vector<std::uint32_t> dimensions = ir_array_dimensions(value);
        if (dimensions.empty() || dimensions.size() > 64u ||
            value.element_type == IRType::Void || value.element_type == IRType::Array) {
            fail("array has an invalid recursive type descriptor");
            return std::nullopt;
        }
        if (value.array_length != dimensions.front() || value.struct_type_id != 0u ||
            (value.element_type == IRType::Struct &&
             (value.element_struct_type_id == 0u ||
              value.element_struct_type_id > module_.struct_types.size())) ||
            (value.element_type != IRType::Struct && value.element_struct_type_id != 0u)) {
            fail("array has inconsistent leaf type or outer-dimension metadata");
            return std::nullopt;
        }
        for (const std::uint32_t dimension : dimensions) {
            if (dimension > 65536u) {
                fail("array dimension exceeds the supported target-layout limit");
                return std::nullopt;
            }
        }

        // A dynamic outer dimension is a pointer value. Its element stride is
        // queried by laying out ir_array_element_value(value).
        if (dimensions.front() == 0u)
            return finish(config_.pointer_size, config_.pointer_alignment);

        const IRValue element = ir_array_element_value(value);
        const auto element_layout = layout_of(element);
        if (!element_layout) return std::nullopt;
        const auto total = checked_multiply(
            static_cast<std::uint64_t>(dimensions.front()), element_layout->stride);
        if (!total) {
            fail("fixed array size overflows the target size type");
            return std::nullopt;
        }
        return finish(*total, element_layout->alignment);
    }

    std::optional<TypeLayout> layout_struct_type(const std::uint32_t id) {
        const auto aggregate = layout_struct(id);
        if (!aggregate) return std::nullopt;
        return finish(aggregate->size, aggregate->alignment);
    }

    std::optional<StructLayout> layout_struct(const std::uint32_t id) {
        if (id == 0u || id > module_.struct_types.size()) {
            fail("structure type identifier is outside the module type table");
            return std::nullopt;
        }
        if (std::find(active_structs_.begin(), active_structs_.end(), id) !=
            active_structs_.end()) {
            fail("structure contains an illegal recursive by-value layout");
            return std::nullopt;
        }

        const IRStructType& structure = module_.struct_types[id - 1u];
        if (structure.id != id) {
            fail("structure type table is not indexed by canonical type identifier");
            return std::nullopt;
        }

        active_structs_.push_back(id);
        StructLayout result;
        std::uint64_t cursor = 0u;
        std::uint64_t aggregate_alignment = 1u;
        for (const IRStructField& field : structure.fields) {
            const auto field_type = layout_of(ir_struct_field_value(field));
            if (!field_type) {
                active_structs_.pop_back();
                return std::nullopt;
            }
            const auto offset = align_up(cursor, field_type->alignment);
            if (!offset) {
                active_structs_.pop_back();
                fail("structure field offset overflows the target size type");
                return std::nullopt;
            }
            const auto end = checked_add(*offset, field_type->size);
            if (!end || *end > config_.maximum_object_size) {
                active_structs_.pop_back();
                fail("structure size overflows or exceeds the target object-size limit");
                return std::nullopt;
            }
            result.fields.push_back(FieldLayout{field.name, *offset, *field_type});
            cursor = *end;
            aggregate_alignment = std::max(aggregate_alignment, field_type->alignment);
        }
        active_structs_.pop_back();
        const auto final_size = align_up(cursor, aggregate_alignment);
        if (!final_size || *final_size > config_.maximum_object_size) {
            fail("structure tail padding exceeds the target object-size limit");
            return std::nullopt;
        }
        result.size = *final_size;
        result.alignment = aggregate_alignment;
        return result;
    }

    const TargetLayoutConfig& config_;
    const IRModule& module_;
    std::string* error_{nullptr};
    std::vector<std::uint32_t> active_structs_;
};

} // namespace

TargetDataLayout::TargetDataLayout(TargetLayoutConfig config) noexcept
    : config_(std::move(config)) {}

const TargetLayoutConfig& TargetDataLayout::config() const noexcept {
    return config_;
}

std::optional<TypeLayout> TargetDataLayout::layout_of(
    const IRValue& value, const IRModule& module, std::string* error) const {
    if (error) error->clear();
    return LayoutComputer(config_, module, error).run(value);
}

std::optional<TypeLayout> TargetDataLayout::layout_of_array_element(
    const IRValue& array, const IRModule& module, std::string* error) const {
    if (error) error->clear();
    return LayoutComputer(config_, module, error).run_array_element(array);
}

std::optional<StructLayout> TargetDataLayout::layout_of_struct(
    const std::uint32_t struct_type_id, const IRModule& module,
    std::string* error) const {
    if (error) error->clear();
    return LayoutComputer(config_, module, error).run_struct(struct_type_id);
}

std::optional<EnumLayout> TargetDataLayout::layout_of_enum(
    const EnumLayoutDescriptor& enumeration, const IRModule& module,
    std::string* error) const {
    if (error) error->clear();
    return LayoutComputer(config_, module, error).run_enum(enumeration);
}

} // namespace vex
