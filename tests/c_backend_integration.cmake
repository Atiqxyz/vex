if(NOT DEFINED VEX_EXECUTABLE OR NOT DEFINED C_COMPILER OR
   NOT DEFINED C_COMPILER_ID OR NOT DEFINED SOURCE_FILE OR
   NOT DEFINED ARRAY_BOUNDS_SOURCE_FILE OR NOT DEFINED WORK_DIR)
    message(FATAL_ERROR "Missing VEX C backend integration-test argument")
endif()

file(MAKE_DIRECTORY "${WORK_DIR}")
set(generated_c "${WORK_DIR}/generated.c")
if(WIN32)
    set(generated_executable "${WORK_DIR}/generated.exe")
else()
    set(generated_executable "${WORK_DIR}/generated")
endif()

execute_process(
    COMMAND "${VEX_EXECUTABLE}" emit-c "${SOURCE_FILE}"
    OUTPUT_FILE "${generated_c}"
    ERROR_VARIABLE emitter_error
    RESULT_VARIABLE emitter_result
)
if(NOT emitter_result EQUAL 0)
    message(FATAL_ERROR "vex emit-c failed (${emitter_result}): ${emitter_error}")
endif()

if(C_COMPILER_ID STREQUAL "MSVC")
    # Derive the Visual Studio installation root from the configured cl.exe
    # path: .../VC/Tools/MSVC/<version>/bin/Hostx64/x64/cl.exe.
    get_filename_component(msvc_installation_path "${C_COMPILER}" DIRECTORY)
    foreach(_level RANGE 1 7)
        get_filename_component(msvc_installation_path "${msvc_installation_path}" DIRECTORY)
    endforeach()

    set(vsdevcmd "${msvc_installation_path}/Common7/Tools/VsDevCmd.bat")
    if(NOT EXISTS "${vsdevcmd}")
        message(FATAL_ERROR "Could not find VsDevCmd.bat beside the configured MSVC installation")
    endif()

    set(msvc_script "${WORK_DIR}/compile_generated_c.bat")
    file(WRITE "${msvc_script}"
        "@echo off\n"
        "call \"${vsdevcmd}\" -arch=x64 -host_arch=x64\n"
        "if errorlevel 1 exit /b 1\n"
        "cl /nologo /std:c11 /TC \"${generated_c}\" \"/Fe:${generated_executable}\"\n"
        "exit /b %errorlevel%\n"
    )
    execute_process(
        COMMAND cmd /D /C "${msvc_script}"
        RESULT_VARIABLE compiler_result
        OUTPUT_VARIABLE compiler_output
        ERROR_VARIABLE compiler_error
    )
else()
    execute_process(
        COMMAND "${C_COMPILER}" -std=c11 "${generated_c}" -o "${generated_executable}" -lm
        RESULT_VARIABLE compiler_result
        OUTPUT_VARIABLE compiler_output
        ERROR_VARIABLE compiler_error
    )
endif()
if(NOT compiler_result EQUAL 0)
    message(FATAL_ERROR "C compiler rejected VEX output (${compiler_result}): ${compiler_output}${compiler_error}")
endif()

execute_process(
    COMMAND "${generated_executable}"
    RESULT_VARIABLE program_result
    OUTPUT_VARIABLE program_output
    ERROR_VARIABLE program_error
)
if(NOT program_result EQUAL 0)
    message(FATAL_ERROR "Generated VEX program exited with ${program_result}: ${program_error}")
endif()

string(REPLACE "\r\n" "\n" program_output "${program_output}")
set(expected_output "30\nVEX\ncompiler\ntrue\nbranch\ntick\ntick\n144\n12\n0\n1\n8\n5\n10\n18\n")
if(NOT "${program_output}" STREQUAL "${expected_output}")
    message(FATAL_ERROR "Generated VEX program produced unexpected output.\nExpected:\n${expected_output}\nActual:\n${program_output}")
endif()

# Exercise the public native-build command independently of the manual backend
# pipeline above. This verifies compiler discovery, generated C compilation,
# output path handling, and execution of the resulting native binary.
if(WIN32)
    set(native_executable "${WORK_DIR}/vex_build_output.exe")
else()
    set(native_executable "${WORK_DIR}/vex_build_output")
endif()

execute_process(
    COMMAND "${VEX_EXECUTABLE}" build-c "${SOURCE_FILE}" -o "${native_executable}"
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error
)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR "vex build-c failed (${build_result}): ${build_output}${build_error}")
endif()

execute_process(
    COMMAND "${native_executable}"
    RESULT_VARIABLE native_result
    OUTPUT_VARIABLE native_output
    ERROR_VARIABLE native_error
)
if(NOT native_result EQUAL 0)
    message(FATAL_ERROR "Native VEX executable exited with ${native_result}: ${native_error}")
endif()
string(REPLACE "\r\n" "\n" native_output "${native_output}")
if(NOT "${native_output}" STREQUAL "${expected_output}")
    message(FATAL_ERROR "vex build executable produced unexpected output.\nExpected:\n${expected_output}\nActual:\n${native_output}")
endif()

# The run command builds in an isolated temporary directory, executes the
# binary, forwards its stdout, and cleans up the temporary artifacts.
execute_process(
    COMMAND "${VEX_EXECUTABLE}" run-c "${SOURCE_FILE}"
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE run_output
    ERROR_VARIABLE run_error
)
if(NOT run_result EQUAL 0)
    message(FATAL_ERROR "vex run-c failed (${run_result}): ${run_output}${run_error}")
endif()
string(REPLACE "\r\n" "\n" run_output "${run_output}")
if(NOT "${run_output}" STREQUAL "${expected_output}")
    message(FATAL_ERROR "vex run produced unexpected output.\nExpected:\n${expected_output}\nActual:\n${run_output}")
endif()

execute_process(
    COMMAND "${VEX_EXECUTABLE}" run-c "${ARRAY_BOUNDS_SOURCE_FILE}"
    RESULT_VARIABLE bounds_result
    OUTPUT_VARIABLE bounds_output
    ERROR_VARIABLE bounds_error
)
if(bounds_result EQUAL 0)
    message(FATAL_ERROR "C backend did not reject an out-of-bounds array index")
endif()
