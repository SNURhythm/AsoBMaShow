# Read generated data rather than inserting launcher values into CMake code.
# Let CMake expand its own list syntax, including escaped literal semicolons.
file(READ "${LAUNCHER_FILE}" launcher)
set(encoded_arguments "")
foreach(argument IN LISTS launcher)
    string(HEX "${argument}" encoded)
    list(APPEND encoded_arguments "x${encoded}")
endforeach()
execute_process(
    COMMAND "${PYTHON_EXECUTABLE}" -c
            "import json, os, sys; print(json.dumps([os.fsdecode(bytes.fromhex(arg[1:])) for arg in sys.argv[1:]]))"
            ${encoded_arguments}
    COMMAND_ERROR_IS_FATAL ANY)
