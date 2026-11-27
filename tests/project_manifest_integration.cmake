if(NOT DEFINED VEX_EXECUTABLE OR NOT DEFINED WORK_DIR)
    message(FATAL_ERROR "VEX_EXECUTABLE and WORK_DIR are required")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

execute_process(
    COMMAND "${VEX_EXECUTABLE}" init module_app
    WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE INIT_STATUS
    OUTPUT_VARIABLE INIT_OUTPUT
    ERROR_VARIABLE INIT_ERROR
)
if(NOT INIT_STATUS EQUAL 0)
    message(FATAL_ERROR "vex init failed: ${INIT_OUTPUT}${INIT_ERROR}")
endif()
if(NOT EXISTS "${WORK_DIR}/vex.toml" OR
   NOT EXISTS "${WORK_DIR}/src/main.vex")
    message(FATAL_ERROR "vex init did not create the manifest and entry source")
endif()

# Replace the starter program with a real project whose entry imports a module.
file(MAKE_DIRECTORY "${WORK_DIR}/lib")
file(WRITE "${WORK_DIR}/vex.toml"
"[project]\nname = \"module_app\"\nentry = \"src/main.vex\"\nbackend = \"llvm\"\noutput = \"build/module_app.exe\"\n")
file(WRITE "${WORK_DIR}/src/main.vex"
"import \"../lib/math.vex\" as math\nmake main() {\n    say(math.twice(21))\n}\n")
file(WRITE "${WORK_DIR}/lib/math.vex"
"pub make twice(value: int) -> int {\n    return value * 2\n}\n")

execute_process(
    COMMAND "${VEX_EXECUTABLE}" check
    WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE CHECK_STATUS
    OUTPUT_VARIABLE CHECK_OUTPUT
    ERROR_VARIABLE CHECK_ERROR
)
if(NOT CHECK_STATUS EQUAL 0)
    message(FATAL_ERROR "project-aware check failed: ${CHECK_OUTPUT}${CHECK_ERROR}")
endif()

execute_process(
    COMMAND "${VEX_EXECUTABLE}" ir
    WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE IR_STATUS
    OUTPUT_VARIABLE IR_OUTPUT
    ERROR_VARIABLE IR_ERROR
)
if(NOT IR_STATUS EQUAL 0 OR NOT IR_OUTPUT MATCHES "twice")
    message(FATAL_ERROR "project-aware IR generation failed: ${IR_OUTPUT}${IR_ERROR}")
endif()

execute_process(
    COMMAND "${VEX_EXECUTABLE}" build
    WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE BUILD_STATUS
    OUTPUT_VARIABLE BUILD_OUTPUT
    ERROR_VARIABLE BUILD_ERROR
)
if(NOT BUILD_STATUS EQUAL 0)
    message(FATAL_ERROR "project-aware build failed: ${BUILD_OUTPUT}${BUILD_ERROR}")
endif()
if(NOT EXISTS "${WORK_DIR}/build/module_app.exe")
    message(FATAL_ERROR "project build did not create the configured output")
endif()

execute_process(
    COMMAND "${VEX_EXECUTABLE}" run
    WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE RUN_STATUS
    OUTPUT_VARIABLE RUN_OUTPUT
    ERROR_VARIABLE RUN_ERROR
)
if(NOT RUN_STATUS EQUAL 0)
    message(FATAL_ERROR "project-aware run failed: ${RUN_OUTPUT}${RUN_ERROR}")
endif()
string(REPLACE "\r\n" "\n" RUN_OUTPUT "${RUN_OUTPUT}")
if(NOT RUN_OUTPUT STREQUAL "42\n")
    message(FATAL_ERROR "project run returned unexpected output: ${RUN_OUTPUT}")
endif()

# Project discovery walks upward from nested source directories.
execute_process(
    COMMAND "${VEX_EXECUTABLE}" check
    WORKING_DIRECTORY "${WORK_DIR}/src"
    RESULT_VARIABLE PARENT_CHECK_STATUS
    OUTPUT_VARIABLE PARENT_CHECK_OUTPUT
    ERROR_VARIABLE PARENT_CHECK_ERROR
)
if(NOT PARENT_CHECK_STATUS EQUAL 0)
    message(FATAL_ERROR "project discovery from a subdirectory failed: ${PARENT_CHECK_OUTPUT}${PARENT_CHECK_ERROR}")
endif()

# The configured backend controls the generic run command.
if(DEFINED VEX_HAS_C_COMPILER AND VEX_HAS_C_COMPILER)
    file(WRITE "${WORK_DIR}/vex.toml"
    "[project]\nname = \"module_app\"\nentry = \"src/main.vex\"\nbackend = \"c\"\noutput = \"build/module_app\"\n")
    execute_process(
        COMMAND "${VEX_EXECUTABLE}" run
        WORKING_DIRECTORY "${WORK_DIR}"
        RESULT_VARIABLE C_BACKEND_RUN_STATUS
        OUTPUT_VARIABLE C_BACKEND_RUN_OUTPUT
        ERROR_VARIABLE C_BACKEND_RUN_ERROR
    )
    if(NOT C_BACKEND_RUN_STATUS EQUAL 0)
        message(FATAL_ERROR "project-selected C backend run failed: ${C_BACKEND_RUN_OUTPUT}${C_BACKEND_RUN_ERROR}")
    endif()
    string(REPLACE "\r\n" "\n" C_BACKEND_RUN_OUTPUT "${C_BACKEND_RUN_OUTPUT}")
    if(NOT C_BACKEND_RUN_OUTPUT STREQUAL "42\n")
        message(FATAL_ERROR "project-selected C backend returned unexpected output: ${C_BACKEND_RUN_OUTPUT}")
    endif()
endif()

# A configured output may not overwrite the entry source or manifest.
file(WRITE "${WORK_DIR}/vex.toml"
"[project]\nname = \"module_app\"\nentry = \"src/main.vex\"\nbackend = \"llvm\"\noutput = \"src/main.vex\"\n")
execute_process(
    COMMAND "${VEX_EXECUTABLE}" build
    WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE CONFIG_OUTPUT_COLLISION_STATUS
    OUTPUT_VARIABLE CONFIG_OUTPUT_COLLISION
    ERROR_VARIABLE CONFIG_OUTPUT_COLLISION_ERROR
)
set(CONFIG_OUTPUT_COLLISION_DIAGNOSTIC "${CONFIG_OUTPUT_COLLISION}${CONFIG_OUTPUT_COLLISION_ERROR}")
if(CONFIG_OUTPUT_COLLISION_STATUS EQUAL 0 OR
   NOT CONFIG_OUTPUT_COLLISION_DIAGNOSTIC MATCHES "must not overwrite")
    message(FATAL_ERROR "a configured output that overwrites its entry source was accepted: ${CONFIG_OUTPUT_COLLISION_DIAGNOSTIC}")
endif()

file(WRITE "${WORK_DIR}/vex.toml"
"[project]\nname = \"module_app\"\nentry = \"src/main.vex\"\nbackend = \"llvm\"\noutput = \"build/module_app\"\n")
execute_process(
    COMMAND "${VEX_EXECUTABLE}" build -o src/main.vex
    WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE OVERRIDE_OUTPUT_COLLISION_STATUS
    OUTPUT_VARIABLE OVERRIDE_OUTPUT_COLLISION
    ERROR_VARIABLE OVERRIDE_OUTPUT_COLLISION_ERROR
)
set(OVERRIDE_OUTPUT_COLLISION_DIAGNOSTIC "${OVERRIDE_OUTPUT_COLLISION}${OVERRIDE_OUTPUT_COLLISION_ERROR}")
if(OVERRIDE_OUTPUT_COLLISION_STATUS EQUAL 0 OR
   NOT OVERRIDE_OUTPUT_COLLISION_DIAGNOSTIC MATCHES "must not overwrite")
    message(FATAL_ERROR "a -o override that overwrites the entry source was accepted: ${OVERRIDE_OUTPUT_COLLISION_DIAGNOSTIC}")
endif()

execute_process(
    COMMAND "${VEX_EXECUTABLE}" build -o vex.toml
    WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE MANIFEST_OUTPUT_COLLISION_STATUS
    OUTPUT_VARIABLE MANIFEST_OUTPUT_COLLISION
    ERROR_VARIABLE MANIFEST_OUTPUT_COLLISION_ERROR
)
set(MANIFEST_OUTPUT_COLLISION_DIAGNOSTIC "${MANIFEST_OUTPUT_COLLISION}${MANIFEST_OUTPUT_COLLISION_ERROR}")
if(MANIFEST_OUTPUT_COLLISION_STATUS EQUAL 0 OR
   NOT MANIFEST_OUTPUT_COLLISION_DIAGNOSTIC MATCHES "must not overwrite")
    message(FATAL_ERROR "a -o override that overwrites the project manifest was accepted: ${MANIFEST_OUTPUT_COLLISION_DIAGNOSTIC}")
endif()

# init must never silently replace existing project files.
execute_process(
    COMMAND "${VEX_EXECUTABLE}" init replacement
    WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE SECOND_INIT_STATUS
    OUTPUT_VARIABLE SECOND_INIT_OUTPUT
    ERROR_VARIABLE SECOND_INIT_ERROR
)
if(SECOND_INIT_STATUS EQUAL 0)
    message(FATAL_ERROR "vex init unexpectedly overwrote an existing project")
endif()

# Unknown settings and unsupported backends fail clearly rather than being ignored.
file(WRITE "${WORK_DIR}/vex.toml"
"[project]\nname = \"module_app\"\nentry = \"src/main.vex\"\nbackend = \"unknown\"\n")
execute_process(
    COMMAND "${VEX_EXECUTABLE}" check
    WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE BAD_BACKEND_STATUS
    OUTPUT_VARIABLE BAD_BACKEND_OUTPUT
    ERROR_VARIABLE BAD_BACKEND_ERROR
)
set(BAD_BACKEND_DIAGNOSTIC "${BAD_BACKEND_OUTPUT}${BAD_BACKEND_ERROR}")
if(BAD_BACKEND_STATUS EQUAL 0 OR
   NOT BAD_BACKEND_DIAGNOSTIC MATCHES "backend must be either")
    message(FATAL_ERROR "an unsupported project backend was not rejected: ${BAD_BACKEND_DIAGNOSTIC}")
endif()

file(WRITE "${WORK_DIR}/vex.toml"
"[project]\nname = \"module_app\"\nentry = \"src/main.vex\"\nbackend = \"llvm\"\nunknown_setting = \"value\"\n")
execute_process(
    COMMAND "${VEX_EXECUTABLE}" check
    WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE UNKNOWN_SETTING_STATUS
    OUTPUT_VARIABLE UNKNOWN_SETTING_OUTPUT
    ERROR_VARIABLE UNKNOWN_SETTING_ERROR
)
set(UNKNOWN_SETTING_DIAGNOSTIC "${UNKNOWN_SETTING_OUTPUT}${UNKNOWN_SETTING_ERROR}")
if(UNKNOWN_SETTING_STATUS EQUAL 0 OR
   NOT UNKNOWN_SETTING_DIAGNOSTIC MATCHES "unknown project setting")
    message(FATAL_ERROR "an unknown project setting was not rejected: ${UNKNOWN_SETTING_DIAGNOSTIC}")
endif()

file(WRITE "${WORK_DIR}/vex.toml"
"[project]\nname = \"module_app\"\nentry = \"../outside.vex\"\nbackend = \"llvm\"\n")
execute_process(
    COMMAND "${VEX_EXECUTABLE}" check
    WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE ESCAPING_ENTRY_STATUS
    OUTPUT_VARIABLE ESCAPING_ENTRY_OUTPUT
    ERROR_VARIABLE ESCAPING_ENTRY_ERROR
)
set(ESCAPING_ENTRY_DIAGNOSTIC "${ESCAPING_ENTRY_OUTPUT}${ESCAPING_ENTRY_ERROR}")
if(ESCAPING_ENTRY_STATUS EQUAL 0 OR
   NOT ESCAPING_ENTRY_DIAGNOSTIC MATCHES "without '..' components")
    message(FATAL_ERROR "a project entry escaping the project root was not rejected: ${ESCAPING_ENTRY_DIAGNOSTIC}")
endif()
