if(NOT DEFINED VEX_EXECUTABLE OR NOT DEFINED C_COMPILER OR
   NOT DEFINED C_COMPILER_ID OR NOT DEFINED SOURCE_FILE OR NOT DEFINED WORK_DIR)
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
        COMMAND "${C_COMPILER}" -std=c11 "${generated_c}" -o "${generated_executable}"
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
set(expected_output "30\nVEX\ncompiler\ntrue\nbranch\ntick\ntick\n")
if(NOT "${program_output}" STREQUAL "${expected_output}")
    message(FATAL_ERROR "Generated VEX program produced unexpected output.\nExpected:\n${expected_output}\nActual:\n${program_output}")
endif()
