#pragma once

#include <cstdint>

namespace vex {

inline constexpr std::uint64_t max_nested_fixed_aggregate_bytes = 256u * 1024u;
inline constexpr std::uint64_t max_function_nested_fixed_stack_bytes = 512u * 1024u;

} // namespace vex
