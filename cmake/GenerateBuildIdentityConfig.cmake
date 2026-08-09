if(NOT DEFINED ASOBMASHOW_SOURCE_DIR OR
   NOT DEFINED ASOBMASHOW_BUILD_CONFIGURATION OR
   NOT DEFINED ASOBMASHOW_OUTPUT_HEADER)
    message(FATAL_ERROR "build identity refresh requires source, configuration, and output")
endif()

set(_asobmashow_commit "0000000000000000000000000000000000000000")
set(_asobmashow_source_clean 0)
find_program(_asobmashow_git NAMES git)
if(_asobmashow_git)
    execute_process(
        COMMAND "${_asobmashow_git}" -C "${ASOBMASHOW_SOURCE_DIR}"
                rev-parse --show-toplevel
        RESULT_VARIABLE _asobmashow_root_result
        OUTPUT_VARIABLE _asobmashow_git_root
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    file(REAL_PATH "${ASOBMASHOW_SOURCE_DIR}" _asobmashow_source_root)
    if(_asobmashow_root_result EQUAL 0)
        file(REAL_PATH "${_asobmashow_git_root}" _asobmashow_git_root)
    endif()
    execute_process(
        COMMAND "${_asobmashow_git}" -C "${ASOBMASHOW_SOURCE_DIR}"
                rev-parse --verify HEAD
        RESULT_VARIABLE _asobmashow_head_result
        OUTPUT_VARIABLE _asobmashow_git_head
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    string(LENGTH "${_asobmashow_git_head}" _asobmashow_head_length)
    if(_asobmashow_root_result EQUAL 0 AND
       "${_asobmashow_git_root}" STREQUAL "${_asobmashow_source_root}" AND
       _asobmashow_head_result EQUAL 0 AND
       _asobmashow_head_length EQUAL 40 AND
       _asobmashow_git_head MATCHES "^[0-9a-f]+$")
        set(_asobmashow_commit "${_asobmashow_git_head}")
        execute_process(
            COMMAND "${_asobmashow_git}" -C "${ASOBMASHOW_SOURCE_DIR}"
                    status --porcelain --untracked-files=normal
            RESULT_VARIABLE _asobmashow_status_result
            OUTPUT_VARIABLE _asobmashow_status
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(_asobmashow_status_result EQUAL 0 AND
           _asobmashow_status STREQUAL "")
            set(_asobmashow_source_clean 1)
        endif()
    endif()
endif()

string(CONCAT _asobmashow_header_content
    "#pragma once\n"
    "#define ASOBMASHOW_BUILD_COMMIT \"${_asobmashow_commit}\"\n"
    "#define ASOBMASHOW_BUILD_CONFIGURATION \"${ASOBMASHOW_BUILD_CONFIGURATION}\"\n"
    "#define ASOBMASHOW_SOURCE_CLEAN ${_asobmashow_source_clean}\n")
get_filename_component(_asobmashow_header_directory
    "${ASOBMASHOW_OUTPUT_HEADER}" DIRECTORY)
file(MAKE_DIRECTORY "${_asobmashow_header_directory}")
set(_asobmashow_existing_content "")
if(EXISTS "${ASOBMASHOW_OUTPUT_HEADER}")
    file(READ "${ASOBMASHOW_OUTPUT_HEADER}" _asobmashow_existing_content)
endif()
if(NOT _asobmashow_existing_content STREQUAL _asobmashow_header_content)
    file(WRITE "${ASOBMASHOW_OUTPUT_HEADER}" "${_asobmashow_header_content}")
endif()
