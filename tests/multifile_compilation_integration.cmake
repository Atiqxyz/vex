if(NOT DEFINED VEX_EXECUTABLE OR NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "VEX_EXECUTABLE and SOURCE_DIR are required")
endif()

execute_process(
    COMMAND "${VEX_EXECUTABLE}" check
        "${SOURCE_DIR}/fixtures/multifile/main.vex"
        "${SOURCE_DIR}/fixtures/multifile/helpers.vex"
    RESULT_VARIABLE CHECK_STATUS
    OUTPUT_VARIABLE CHECK_OUTPUT
    ERROR_VARIABLE CHECK_ERROR
)
if(NOT CHECK_STATUS EQUAL 0)
    message(FATAL_ERROR "multi-file semantic check failed:\n${CHECK_OUTPUT}\n${CHECK_ERROR}")
endif()

execute_process(
    COMMAND "${VEX_EXECUTABLE}" ir
        "${SOURCE_DIR}/fixtures/multifile/main.vex"
        "${SOURCE_DIR}/fixtures/multifile/helpers.vex"
    RESULT_VARIABLE IR_STATUS
    OUTPUT_VARIABLE IR_OUTPUT
    ERROR_VARIABLE IR_ERROR
)
if(NOT IR_STATUS EQUAL 0)
    message(FATAL_ERROR "multi-file IR lowering failed:\n${IR_OUTPUT}\n${IR_ERROR}")
endif()
if(NOT IR_OUTPUT MATCHES "compute_total")
    message(FATAL_ERROR "multi-file IR does not contain the cross-file function call")
endif()

execute_process(
    COMMAND "${VEX_EXECUTABLE}" check
        "${SOURCE_DIR}/fixtures/multifile/main.vex"
        "${SOURCE_DIR}/fixtures/multifile/helpers.vex"
        "${SOURCE_DIR}/fixtures/multifile/helpers.vex"
    RESULT_VARIABLE DUPLICATE_STATUS
    OUTPUT_VARIABLE DUPLICATE_OUTPUT
    ERROR_VARIABLE DUPLICATE_ERROR
)
if(DUPLICATE_STATUS EQUAL 0)
    message(FATAL_ERROR "duplicate declarations across source files were unexpectedly accepted")
endif()
if(NOT "${DUPLICATE_OUTPUT}${DUPLICATE_ERROR}" MATCHES "duplicate|already")
    message(FATAL_ERROR "duplicate source declaration did not produce a useful diagnostic")
endif()

# Recursive relative imports, repeated imports, and cycles share the current
# merged-namespace translation-unit model.
execute_process(
    COMMAND "${VEX_EXECUTABLE}" check "${SOURCE_DIR}/fixtures/imports/main.vex"
    RESULT_VARIABLE IMPORT_CHECK_STATUS
    OUTPUT_VARIABLE IMPORT_CHECK_OUTPUT
    ERROR_VARIABLE IMPORT_CHECK_ERROR
)
if(NOT IMPORT_CHECK_STATUS EQUAL 0)
    message(FATAL_ERROR "recursive import check failed:\n${IMPORT_CHECK_OUTPUT}\n${IMPORT_CHECK_ERROR}")
endif()

execute_process(
    COMMAND "${VEX_EXECUTABLE}" ir "${SOURCE_DIR}/fixtures/imports/main.vex"
    RESULT_VARIABLE IMPORT_IR_STATUS
    OUTPUT_VARIABLE IMPORT_IR_OUTPUT
    ERROR_VARIABLE IMPORT_IR_ERROR
)
if(NOT IMPORT_IR_STATUS EQUAL 0)
    message(FATAL_ERROR "recursive import IR lowering failed:\n${IMPORT_IR_OUTPUT}\n${IMPORT_IR_ERROR}")
endif()
if(NOT IMPORT_IR_OUTPUT MATCHES "double_base" OR NOT IMPORT_IR_OUTPUT MATCHES "base_value")
    message(FATAL_ERROR "recursive import declarations were not included in the IR")
endif()

if(DEFINED VEX_HAS_C_COMPILER AND NOT VEX_HAS_C_COMPILER STREQUAL "")
    execute_process(
        COMMAND "${VEX_EXECUTABLE}" run-c "${SOURCE_DIR}/fixtures/imports/main.vex"
        RESULT_VARIABLE IMPORT_RUN_STATUS
        OUTPUT_VARIABLE IMPORT_RUN_OUTPUT
        ERROR_VARIABLE IMPORT_RUN_ERROR
    )
    if(NOT IMPORT_RUN_STATUS EQUAL 0)
        message(FATAL_ERROR "native import execution failed:\n${IMPORT_RUN_OUTPUT}\n${IMPORT_RUN_ERROR}")
    endif()
    string(REPLACE "\r\n" "\n" IMPORT_RUN_OUTPUT "${IMPORT_RUN_OUTPUT}")
    if(NOT IMPORT_RUN_OUTPUT STREQUAL "42\n")
        message(FATAL_ERROR "C backend import execution returned unexpected output: ${IMPORT_RUN_OUTPUT}")
    endif()
endif()

execute_process(
    COMMAND "${VEX_EXECUTABLE}" run-llvm "${SOURCE_DIR}/fixtures/imports/main.vex"
    RESULT_VARIABLE IMPORT_LLVM_RUN_STATUS
    OUTPUT_VARIABLE IMPORT_LLVM_RUN_OUTPUT
    ERROR_VARIABLE IMPORT_LLVM_RUN_ERROR
)
if(NOT IMPORT_LLVM_RUN_STATUS EQUAL 0)
    message(FATAL_ERROR "LLVM import execution failed:\n${IMPORT_LLVM_RUN_OUTPUT}\n${IMPORT_LLVM_RUN_ERROR}")
endif()
string(REPLACE "\r\n" "\n" IMPORT_LLVM_RUN_OUTPUT "${IMPORT_LLVM_RUN_OUTPUT}")
if(NOT IMPORT_LLVM_RUN_OUTPUT STREQUAL "42\n")
    message(FATAL_ERROR "LLVM backend import execution returned unexpected output: ${IMPORT_LLVM_RUN_OUTPUT}")
endif()

execute_process(
    COMMAND "${VEX_EXECUTABLE}" check "${SOURCE_DIR}/fixtures/imports/invalid_main.vex"
    RESULT_VARIABLE INVALID_IMPORT_SOURCE_STATUS
    OUTPUT_VARIABLE INVALID_IMPORT_SOURCE_OUTPUT
    ERROR_VARIABLE INVALID_IMPORT_SOURCE_ERROR
)
set(INVALID_IMPORT_SOURCE_DIAGNOSTIC "${INVALID_IMPORT_SOURCE_OUTPUT}${INVALID_IMPORT_SOURCE_ERROR}")
if(INVALID_IMPORT_SOURCE_STATUS EQUAL 0 OR
   NOT INVALID_IMPORT_SOURCE_DIAGNOSTIC MATCHES "invalid.vex:2" OR
   NOT INVALID_IMPORT_SOURCE_DIAGNOSTIC MATCHES "SEM001")
    message(FATAL_ERROR "semantic diagnostics from an imported file must retain its path and line:\n${INVALID_IMPORT_SOURCE_DIAGNOSTIC}")
endif()

execute_process(
    COMMAND "${VEX_EXECUTABLE}" check "${SOURCE_DIR}/fixtures/imports/missing.vex"
    RESULT_VARIABLE MISSING_IMPORT_STATUS
    OUTPUT_VARIABLE MISSING_IMPORT_OUTPUT
    ERROR_VARIABLE MISSING_IMPORT_ERROR
)
set(MISSING_IMPORT_DIAGNOSTIC "${MISSING_IMPORT_OUTPUT}${MISSING_IMPORT_ERROR}")
if(MISSING_IMPORT_STATUS EQUAL 0 OR
   NOT MISSING_IMPORT_DIAGNOSTIC MATCHES "missing.vex:1" OR
   NOT MISSING_IMPORT_DIAGNOSTIC MATCHES "MOD001")
    message(FATAL_ERROR "missing imports must fail with a source-located MOD001 diagnostic:\n${MISSING_IMPORT_DIAGNOSTIC}")
endif()
