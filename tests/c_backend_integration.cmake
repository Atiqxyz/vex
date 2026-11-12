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
    find_program(VSWHERE_EXECUTABLE vswhere
        HINTS
            "$ENV{ProgramFiles}/Microsoft Visual Studio/Installer"
            "$ENV{ProgramFiles(x86)}/Microsoft Visual Studio/Installer"
    )
    if(NOT VSWHERE_EXECUTABLE)
        message(FATAL_ERROR "vswhere.exe is required to initialize the MSVC C compiler environment")
    endif()

    execute_process(
        COMMAND "${VSWHERE_EXECUTABLE}" -latest -products "*" 
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64
            -property installationPath
        OUTPUT_VARIABLE vs_installation_path
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE vswhere_result
    )
    if(NOT vswhere_result EQUAL 0 OR vs_installation_path STREQUAL "")
        message(FATAL_ERROR "Could not locate a Visual Studio C++ toolchain installation")
    endif()

    set(vsdevcmd "${vs_installation_path}/Common7/Tools/VsDevCmd.bat")
    set(msvc_command
        "call \"${vsdevcmd}\" -arch=x64 -host_arch=x64 && cl /nologo /std:c11 /TC \"${generated_c}\" \"/Fe:${generated_executable}\""
    )
    execute_process(
        COMMAND cmd /D /C "${msvc_command}"
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
