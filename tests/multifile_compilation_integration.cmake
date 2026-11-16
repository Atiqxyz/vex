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
