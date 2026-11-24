if(NOT DEFINED VEX_EXECUTABLE OR NOT DEFINED LLVM_CLANG OR
   NOT DEFINED SOURCE_FILE OR NOT DEFINED ARRAY_BOUNDS_SOURCE_FILE OR
   NOT DEFINED REMAINDER_ZERO_SOURCE_FILE OR
   NOT DEFINED DYNAMIC_ARRAY_POP_EMPTY_SOURCE_FILE OR
   NOT DEFINED DYNAMIC_ARRAY_BOUNDS_SOURCE_FILE OR
   NOT DEFINED STRUCT_SOURCE_FILE OR NOT DEFINED STRUCT_LAYOUT_SOURCE_FILE OR
   NOT DEFINED ENUM_SOURCE_FILE OR
   NOT DEFINED COMPOUND_SOURCE_FILE OR NOT DEFINED PROJECTED_PLACES_SOURCE_FILE OR
   NOT DEFINED STRING_CONCAT_SOURCE_FILE OR
   NOT DEFINED BITWISE_SOURCE_FILE OR NOT DEFINED BYTE_SOURCE_FILE OR
   NOT DEFINED BYTE_OVERFLOW_HIGH_SOURCE_FILE OR
   NOT DEFINED BYTE_OVERFLOW_NEGATIVE_SOURCE_FILE OR
   NOT DEFINED BINARY_IO_SOURCE_FILE OR
   NOT DEFINED BINARY_READ_BOUNDS_SOURCE_FILE OR
   NOT DEFINED BINARY_WRITE_OVERFLOW_SOURCE_FILE OR
   NOT DEFINED BINARY_SLICE_BOUNDS_SOURCE_FILE OR
   NOT DEFINED BINARY_IMMUTABLE_BUFFER_SOURCE_FILE OR NOT DEFINED POINTERS_SOURCE_FILE OR NOT DEFINED WORK_DIR)
    message(FATAL_ERROR "Missing VEX LLVM backend integration-test argument")
endif()

file(MAKE_DIRECTORY "${WORK_DIR}")
if(WIN32)
    set(generated_executable "${WORK_DIR}/generated.exe")
else()
    set(generated_executable "${WORK_DIR}/generated")
endif()

execute_process(
    COMMAND "${VEX_EXECUTABLE}" emit-llvm "${SOURCE_FILE}"
    OUTPUT_FILE "${WORK_DIR}/generated.ll"
    ERROR_VARIABLE emitter_error
    RESULT_VARIABLE emitter_result
)
if(NOT emitter_result EQUAL 0)
    message(FATAL_ERROR "vex emit-llvm failed (${emitter_result}): ${emitter_error}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "VEX_LLVM_CC=${LLVM_CLANG}"
            "${VEX_EXECUTABLE}" build-llvm "${SOURCE_FILE}" -o "${generated_executable}"
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error
)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR "vex build-llvm failed (${build_result}): ${build_output}${build_error}")
endif()

execute_process(
    COMMAND "${generated_executable}"
    RESULT_VARIABLE program_result
    OUTPUT_VARIABLE program_output
    ERROR_VARIABLE program_error
)
if(NOT program_result EQUAL 0)
    message(FATAL_ERROR "LLVM-generated program exited with ${program_result}: ${program_error}")
endif()

string(REPLACE "\r\n" "\n" program_output "${program_output}")
set(expected_output "12\n19\n12\n19\n2\nVEX\nfalse\n1\narrays\ntrue\n-1\n1\n-1\n-1\n0\n1\n2\n-1\n3\n4\n34\n5\n6\n11\n0\n7\n7\n10\n11\ntrue\ntrue\n12\n14\n25\n2\n4\n2\n4\n2\n4\n120\ntrue\ntrue\n30\nVEX\ncompiler\ntrue\nbranch\ntick\ntick\n144\n12\n0\n1\n8\n5\n10\n18\n9\n8\n0\n9223372036854775807\n0\n2\n40\n0\n2\n7\n20\n1\n7\n3.5\nruntime\ntrue\n2\n9\n11\n2\n5\n10\n9\n1\n2\n")
if(NOT "${program_output}" STREQUAL "${expected_output}")
    message(FATAL_ERROR "LLVM-generated program produced unexpected output.\nExpected:\n${expected_output}\nActual:\n${program_output}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "VEX_LLVM_CC=${LLVM_CLANG}"
            "${VEX_EXECUTABLE}" run-llvm "${SOURCE_FILE}"
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE run_output
    ERROR_VARIABLE run_error
)
if(NOT run_result EQUAL 0)
    message(FATAL_ERROR "vex run-llvm failed (${run_result}): ${run_output}${run_error}")
endif()
string(REPLACE "\r\n" "\n" run_output "${run_output}")
if(NOT "${run_output}" STREQUAL "${expected_output}")
    message(FATAL_ERROR "vex run-llvm produced unexpected output.\nExpected:\n${expected_output}\nActual:\n${run_output}")
endif()


# Verify structure aggregates through LLVM compilation and execution.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "VEX_LLVM_CC=${LLVM_CLANG}"
            "${VEX_EXECUTABLE}" run-llvm "${STRUCT_SOURCE_FILE}"
    RESULT_VARIABLE struct_result
    OUTPUT_VARIABLE struct_output
    ERROR_VARIABLE struct_error
)
if(NOT struct_result EQUAL 0)
    message(FATAL_ERROR "LLVM structure program failed: ${struct_output}${struct_error}")
endif()
string(REPLACE "\r\n" "\n" struct_output "${struct_output}")
set(struct_expected "7\n1\n6\n1\n3\n4\n4\n6\n11\n10\n50\n10\n32\n11\n99\n10\n100\n77\n88\n2\n")
if(NOT "${struct_output}" STREQUAL "${struct_expected}")
    message(FATAL_ERROR "LLVM structure value semantics mismatch. Expected:${struct_expected}Actual:${struct_output}")
endif()

# Stress native aggregate layout, including tail padding and dynamic growth.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "VEX_LLVM_CC=${LLVM_CLANG}" "${VEX_EXECUTABLE}" run-llvm "${STRUCT_LAYOUT_SOURCE_FILE}"
    RESULT_VARIABLE layout_result
    OUTPUT_VARIABLE layout_output
    ERROR_VARIABLE layout_error
)
if(NOT layout_result EQUAL 0)
    message(FATAL_ERROR "LLVM backend layout program failed: ${layout_output}${layout_error}")
endif()
string(REPLACE "\r\n" "\n" layout_output "${layout_output}")
set(layout_expected "true\ntrue\n1.5\n17\n3.5\ntrue\n18\n3.5\n2.5\n8.5\n3.5\n99.5\n7.5\n9.25\nfalse\n17\n42.5\n41\n42.5\n4\n37\n2026\n")
if(NOT "${layout_output}" STREQUAL "${layout_expected}")
    message(FATAL_ERROR "LLVM backend mixed-layout structure behavior mismatch. Expected:${layout_expected}Actual:${layout_output}")
endif()

# Exercise enum constructors, struct fields, fixed-array and dynamic-array payloads, and exhaustive matching.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "VEX_LLVM_CC=${LLVM_CLANG}"
            "${VEX_EXECUTABLE}" run-llvm "${ENUM_SOURCE_FILE}"
    RESULT_VARIABLE enum_result OUTPUT_VARIABLE enum_output ERROR_VARIABLE enum_error
)
if(NOT enum_result EQUAL 0)
    message(FATAL_ERROR "LLVM backend enum program failed: ${enum_output}${enum_error}")
endif()
string(REPLACE "\r\n" "\n" enum_output "${enum_output}")
set(enum_expected "42\n1\n504\n34\n216\n220\n3\ntrue\n34\n216\n2\n")
if(NOT "${enum_output}" STREQUAL "${enum_expected}")
    message(FATAL_ERROR "LLVM backend enum behavior mismatch. Expected:${enum_expected}Actual:${enum_output}")
endif()

# Exercise nested compound writes and once-only path/RHS evaluation.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "VEX_LLVM_CC=${LLVM_CLANG}"
            "${VEX_EXECUTABLE}" run-llvm "${COMPOUND_SOURCE_FILE}"
    RESULT_VARIABLE compound_result OUTPUT_VARIABLE compound_output ERROR_VARIABLE compound_error
)
if(NOT compound_result EQUAL 0)
    message(FATAL_ERROR "LLVM backend compound test failed: ${compound_output}${compound_error}")
endif()
string(REPLACE "\r\n" "\n" compound_output "${compound_output}")
set(compound_expected "15\n8\n8\n2\n10\n20\n4\n7\n8\n14\n6\n-1\n-6\n15\n")
if(NOT "${compound_output}" STREQUAL "${compound_expected}")
    message(FATAL_ERROR "Compound assignment behavior mismatch. Expected:${compound_expected}Actual:${compound_output}")
endif()

# Exercise nested field/element projections and a reallocating RHS through LLVM.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "VEX_LLVM_CC=${LLVM_CLANG}"
            "${VEX_EXECUTABLE}" run-llvm "${PROJECTED_PLACES_SOURCE_FILE}"
    RESULT_VARIABLE projected_result OUTPUT_VARIABLE projected_output ERROR_VARIABLE projected_error
)
if(NOT projected_result EQUAL 0)
    message(FATAL_ERROR "LLVM backend projected-place test failed: ${projected_output}${projected_error}")
endif()
string(REPLACE "\r\n" "\n" projected_output "${projected_output}")
if(NOT "${projected_output}" STREQUAL "1\n2\n3\n7\n17\n")
    message(FATAL_ERROR "LLVM backend projected-place behavior mismatch. Actual:${projected_output}")
endif()

# Capacity is an absolute minimum; reservation preserves length, elements and aliases.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "VEX_LLVM_CC=${LLVM_CLANG}"
            "${VEX_EXECUTABLE}" run-llvm "${DYNAMIC_ARRAY_CAPACITY_SOURCE_FILE}"
    RESULT_VARIABLE capacity_result OUTPUT_VARIABLE capacity_output ERROR_VARIABLE capacity_error
)
if(NOT capacity_result EQUAL 0)
    message(FATAL_ERROR "LLVM backend dynamic-array capacity test failed: ${capacity_output}${capacity_error}")
endif()
string(REPLACE "\r\n" "\n" capacity_output "${capacity_output}")
set(capacity_expected "0\ntrue\n0\ntrue\n2\n7\n8\ntrue\n3\n9\ntrue\ntrue\n1.5\n6.5\n")
if(NOT "${capacity_output}" STREQUAL "${capacity_expected}")
    message(FATAL_ERROR "Dynamic-array capacity behavior mismatch. Expected:${capacity_expected}Actual:${capacity_output}")
endif()
foreach(reserve_failure_source IN ITEMS
        "${DYNAMIC_ARRAY_NEGATIVE_RESERVE_SOURCE_FILE}"
        "${DYNAMIC_ARRAY_RESERVE_OVERFLOW_SOURCE_FILE}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env "VEX_LLVM_CC=${LLVM_CLANG}"
                "${VEX_EXECUTABLE}" run-llvm "${reserve_failure_source}"
        RESULT_VARIABLE reserve_failure_result
        OUTPUT_VARIABLE reserve_failure_output
        ERROR_VARIABLE reserve_failure_error
    )
    if("${reserve_failure_result}" STREQUAL "0")
        message(FATAL_ERROR "LLVM backend did not trap for invalid reservation: ${reserve_failure_source}")
    endif()
endforeach()

# Exercise empty strings, embedded NUL bytes, and string +=.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "VEX_LLVM_CC=${LLVM_CLANG}"
            "${VEX_EXECUTABLE}" run-llvm "${STRING_CONCAT_SOURCE_FILE}"
    RESULT_VARIABLE string_concat_result OUTPUT_VARIABLE string_concat_output ERROR_VARIABLE string_concat_error
)
if(NOT string_concat_result EQUAL 0)
    message(FATAL_ERROR "LLVM backend string concatenation test failed: ${string_concat_output}${string_concat_error}")
endif()
string(REPLACE "\r\n" "\n" string_concat_output "${string_concat_output}")
set(string_concat_expected "VEX Lang\nAB\ntrue\nVEX\nVEX\ntrue\ntrue\ntrue\ntrue\ntrue\n8\n3\n6\n0\ntrue\ntrue\ntrue\ntrue\n5\n100000\n")
if(NOT "${string_concat_output}" STREQUAL "${string_concat_expected}")
    message(FATAL_ERROR "String concatenation behavior mismatch. Expected:${string_concat_expected}Actual:${string_concat_output}")
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "VEX_LLVM_CC=${LLVM_CLANG}"
            "${VEX_EXECUTABLE}" run-llvm "${ARRAY_BOUNDS_SOURCE_FILE}"
    COMMAND "${CMAKE_COMMAND}" -E env "VEX_LLVM_CC=${LLVM_CLANG}"
            "${VEX_EXECUTABLE}" run-llvm "${ARRAY_BOUNDS_SOURCE_FILE}"
    RESULT_VARIABLE bounds_result
    OUTPUT_VARIABLE bounds_output
    ERROR_VARIABLE bounds_error
)
if(bounds_result EQUAL 0)
    message(FATAL_ERROR "LLVM backend did not reject an out-of-bounds array index")
endif()

execute_process(
    COMMAND "${VEX_EXECUTABLE}" run-llvm "${REMAINDER_ZERO_SOURCE_FILE}"
    RESULT_VARIABLE remainder_zero_result
    OUTPUT_VARIABLE remainder_zero_output
    ERROR_VARIABLE remainder_zero_error
)
if(remainder_zero_result EQUAL 0)
    message(FATAL_ERROR "LLVM backend did not trap on remainder by zero")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "VEX_LLVM_CC=${LLVM_CLANG}"
            "${VEX_EXECUTABLE}" run-llvm "${DYNAMIC_ARRAY_POP_EMPTY_SOURCE_FILE}"
    RESULT_VARIABLE dynamic_pop_result
    OUTPUT_VARIABLE dynamic_pop_output
    ERROR_VARIABLE dynamic_pop_error
)
if("${dynamic_pop_result}" STREQUAL "0")
    message(FATAL_ERROR "LLVM backend did not trap when popping an empty dynamic array")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "VEX_LLVM_CC=${LLVM_CLANG}"
            "${VEX_EXECUTABLE}" run-llvm "${DYNAMIC_ARRAY_BOUNDS_SOURCE_FILE}"
    RESULT_VARIABLE dynamic_bounds_result
    OUTPUT_VARIABLE dynamic_bounds_output
    ERROR_VARIABLE dynamic_bounds_error
)
if("${dynamic_bounds_result}" STREQUAL "0")
    message(FATAL_ERROR "LLVM backend did not trap on dynamic-array out-of-bounds indexing")
endif()


# Execute bitwise operators through LLVM code generation and native compilation.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "VEX_LLVM_CC=${LLVM_CLANG}"
            "${VEX_EXECUTABLE}" run-llvm "${BITWISE_SOURCE_FILE}"
    RESULT_VARIABLE bitwise_result
    OUTPUT_VARIABLE bitwise_output
    ERROR_VARIABLE bitwise_error
)
if(NOT bitwise_result EQUAL 0)
    message(FATAL_ERROR "LLVM bitwise program failed: ${bitwise_output}${bitwise_error}")
endif()
string(REPLACE "\r\n" "\n" bitwise_output "${bitwise_output}")
set(bitwise_expected "8\n14\n6\n-6\n15\n-1\n-9223372036854775808\n-9223372036854775808\n-9223372036854775808\n-9223372036854775805\n3\nfalse\n48\n-4\n-3\n-9223372036854775808\n2\n64\n8\n6\n1\n2\n-9223372036854775808\n3\n-9223372036854775808\n3\n192\n129\n3\n")
if(NOT "${bitwise_output}" STREQUAL "${bitwise_expected}")
    message(FATAL_ERROR "LLVM bitwise behavior mismatch. Expected:${bitwise_expected}Actual:${bitwise_output}")
endif()

# Run byte values through LLVM code generation and native execution.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "VEX_LLVM_CC=${LLVM_CLANG}"
            "${VEX_EXECUTABLE}" run-llvm "${BYTE_SOURCE_FILE}"
    RESULT_VARIABLE byte_result
    OUTPUT_VARIABLE byte_output
    ERROR_VARIABLE byte_error
)
if(NOT byte_result EQUAL 0)
    message(FATAL_ERROR "LLVM byte program failed: ${byte_output}${byte_error}")
endif()
string(REPLACE "\r\n" "\n" byte_output "${byte_output}")
set(byte_expected "160\n238\n102\n85\ntrue\n255\n255\n3\n7\n2\n3\n170\n425\n")
if(NOT "${byte_output}" STREQUAL "${byte_expected}")
    message(FATAL_ERROR "LLVM byte behavior mismatch. Expected:${byte_expected}Actual:${byte_output}")
endif()

# Exercise binary buffers, endian-aware integer access, and whole-file I/O.
set(binary_output_file "${WORK_DIR}/vex_binary_io_test.dat")
set(binary_large_output_file "${WORK_DIR}/vex_binary_io_large_test.dat")
file(REMOVE "${binary_output_file}" "${binary_large_output_file}")
file(REMOVE "${WORK_DIR}/vex_file_that_does_not_exist_vex_test.bin")
file(REMOVE_RECURSE "${WORK_DIR}/vex_missing_directory_vex_test")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "VEX_LLVM_CC=${LLVM_CLANG}"
            "${VEX_EXECUTABLE}" run-llvm "${BINARY_IO_SOURCE_FILE}"
    WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE binary_result
    OUTPUT_VARIABLE binary_output
    ERROR_VARIABLE binary_error
)
if(NOT binary_result EQUAL 0)
    message(FATAL_ERROR "LLVM backend binary I/O test failed: ${binary_output}${binary_error}")
endif()
string(REPLACE "\r\n" "\n" binary_output "${binary_output}")
set(binary_expected "4660\n4660\n305419896\n305419896\n4\n18\n18\n52\n120\n99\n18\n12\n12\n13\n255\n305419896\n-1\n1\n9\n-1\n10000\n10000\n10000\n173\n173\n")
if(NOT "${binary_output}" STREQUAL "${binary_expected}")
    message(FATAL_ERROR "LLVM backend binary I/O mismatch. Expected:${binary_expected}Actual:${binary_output}")
endif()
file(SIZE "${binary_output_file}" binary_file_size)
if(NOT binary_file_size EQUAL 12)
    message(FATAL_ERROR "LLVM backend binary output file has size ${binary_file_size}, expected 12")
endif()
file(SIZE "${binary_large_output_file}" binary_large_file_size)
if(NOT binary_large_file_size EQUAL 10000)
    message(FATAL_ERROR "Binary output file has size ${binary_large_file_size}, expected 10000")
endif()
file(REMOVE "${binary_output_file}" "${binary_large_output_file}")

foreach(binary_failure_source IN ITEMS
        "${BINARY_READ_BOUNDS_SOURCE_FILE}"
        "${BINARY_WRITE_OVERFLOW_SOURCE_FILE}"
        "${BINARY_SLICE_BOUNDS_SOURCE_FILE}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env "VEX_LLVM_CC=${LLVM_CLANG}"
                "${VEX_EXECUTABLE}" run-llvm "${binary_failure_source}"
        RESULT_VARIABLE binary_failure_result
        OUTPUT_VARIABLE binary_failure_output
        ERROR_VARIABLE binary_failure_error
    )
    if("${binary_failure_result}" STREQUAL "0")
        message(FATAL_ERROR "LLVM backend failed to reject invalid binary operation: ${binary_failure_source}")
    endif()
endforeach()

execute_process(
    COMMAND "${VEX_EXECUTABLE}" check "${BINARY_IMMUTABLE_BUFFER_SOURCE_FILE}"
    RESULT_VARIABLE immutable_buffer_result
    OUTPUT_VARIABLE immutable_buffer_output
    ERROR_VARIABLE immutable_buffer_error
)
if("${immutable_buffer_result}" STREQUAL "0")
    message(FATAL_ERROR "LLVM semantic checker allowed mutation of an immutable byte buffer")
endif()


foreach(byte_failure_source IN ITEMS
        "${BYTE_OVERFLOW_HIGH_SOURCE_FILE}"
        "${BYTE_OVERFLOW_NEGATIVE_SOURCE_FILE}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env "VEX_LLVM_CC=${LLVM_CLANG}"
                "${VEX_EXECUTABLE}" run-llvm "${byte_failure_source}"
        RESULT_VARIABLE byte_failure_result
        OUTPUT_VARIABLE byte_failure_output
        ERROR_VARIABLE byte_failure_error
    )
    if("${byte_failure_result}" STREQUAL "0")
        message(FATAL_ERROR "LLVM backend did not trap for out-of-range int-to-byte conversion: ${byte_failure_source}")
    endif()
endforeach()

# Invalid shift counts must trap before LLVM can produce poison values.
foreach(case IN ITEMS "int_negative" "int_width" "byte_width")
    if(case STREQUAL "int_negative")
        set(source "make main() {\n    say(1 << -1)\n}\n")
    elseif(case STREQUAL "int_width")
        set(source "make main() {\n    say(1 >> 64)\n}\n")
    else()
        set(source "make main() {\n    say(to_byte(1) << 8)\n}\n")
    endif()
    set(source_path "${WORK_DIR}/invalid_shift_${case}.vex")
    file(WRITE "${source_path}" "${source}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env "VEX_LLVM_CC=${LLVM_CLANG}"
        "${VEX_EXECUTABLE}" run-llvm "${source_path}"
        RESULT_VARIABLE shift_result OUTPUT_VARIABLE shift_output ERROR_VARIABLE shift_error)
    if("${shift_result}" STREQUAL "0")
        message(FATAL_ERROR "LLVM backend did not trap for invalid shift count: ${case}")
    endif()
endforeach()

# First-class typed pointers must preserve aliasing across an addressable local.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "VEX_LLVM_CC=${LLVM_CLANG}"
            "${VEX_EXECUTABLE}" run-llvm "${POINTERS_SOURCE_FILE}"
    RESULT_VARIABLE pointer_result
    OUTPUT_VARIABLE pointer_output
    ERROR_VARIABLE pointer_error
)
if(NOT pointer_result EQUAL 0)
    message(FATAL_ERROR "LLVM backend pointer fixture failed: ${pointer_error}")
endif()
if(NOT pointer_output STREQUAL "42\n43\n43\n43\n44\n11\n11\n22\n22\n23\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\n10\n16\n11\n11\n11\n")
    message(FATAL_ERROR "LLVM backend pointer fixture produced unexpected output: [${pointer_output}]")
endif()
