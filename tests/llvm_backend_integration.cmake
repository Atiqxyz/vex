if(NOT DEFINED VEX_EXECUTABLE OR NOT DEFINED LLVM_CLANG OR
   NOT DEFINED SOURCE_FILE OR NOT DEFINED ARRAY_BOUNDS_SOURCE_FILE OR
   NOT DEFINED REMAINDER_ZERO_SOURCE_FILE OR
   NOT DEFINED DYNAMIC_ARRAY_POP_EMPTY_SOURCE_FILE OR
   NOT DEFINED DYNAMIC_ARRAY_BOUNDS_SOURCE_FILE OR
   NOT DEFINED STRUCT_SOURCE_FILE OR NOT DEFINED WORK_DIR)
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

execute_process(
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
