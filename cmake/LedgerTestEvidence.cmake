# Native setup fixtures produce proof for the current CTest invocation. Audits
# require those fixtures even when selected alone with ctest -R.
set(ASOBMASHOW_LEDGER_EVIDENCE_SCRIPT
    "${CMAKE_CURRENT_LIST_DIR}/../tests/support/ledger_test_evidence.py")
set(ASOBMASHOW_LEDGER_EVIDENCE_DIRECTORY
    "${CMAKE_BINARY_DIR}/ledger-evidence/$<CONFIG>")

function(asobmashow_load_ledger_runners ledger_path output_variable)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${ledger_path}" "${ASOBMASHOW_LEDGER_EVIDENCE_SCRIPT}")
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${ASOBMASHOW_LEDGER_EVIDENCE_SCRIPT}"
            --runners "${ledger_path}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE runners
        ERROR_VARIABLE error
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT result EQUAL 0 OR NOT runners)
        message(FATAL_ERROR "Cannot read ledger runners from ${ledger_path}: ${error}")
    endif()
    set(${output_variable} "${runners}" PARENT_SCOPE)
endfunction()

function(asobmashow_add_ledger_test target_name test_name)
    get_property(existing TARGET ${target_name} PROPERTY ASOBMASHOW_LEDGER_EVIDENCE_TEST)
    if(existing)
        message(FATAL_ERROR "Multiple default ledger tests for ${target_name}: ${existing}, ${test_name}")
    endif()
    add_test(NAME ${test_name}
        COMMAND "${Python3_EXECUTABLE}" "${ASOBMASHOW_LEDGER_EVIDENCE_SCRIPT}"
            --executable $<TARGET_FILE:${target_name}>
            --runner ${target_name}
            --evidence-dir "${ASOBMASHOW_LEDGER_EVIDENCE_DIRECTORY}")
    set_property(TEST ${test_name} APPEND PROPERTY
        FIXTURES_SETUP "ledger_${target_name}")
    set_property(TARGET ${target_name} PROPERTY
        ASOBMASHOW_LEDGER_EVIDENCE_TEST ${test_name})
endfunction()

function(asobmashow_require_ledger_evidence audit_test)
    if(NOT TEST ${audit_test})
        return()
    endif()
    set(unavailable_runners)
    foreach(runner IN LISTS ARGN)
        if(NOT TARGET ${runner})
            list(APPEND unavailable_runners ${runner})
            continue()
        endif()
        get_property(owner_test TARGET ${runner} PROPERTY ASOBMASHOW_LEDGER_EVIDENCE_TEST)
        if(NOT owner_test)
            message(FATAL_ERROR "Ledger audit ${audit_test} requires a default test for ${runner}")
        endif()
        if(NOT TEST ${owner_test})
            message(FATAL_ERROR "Ledger audit ${audit_test} requires a default test for ${runner}")
        endif()
    endforeach()
    if(unavailable_runners)
        message(STATUS "Disabling ledger audit ${audit_test}; unavailable runners: ${unavailable_runners}")
        set_property(TEST ${audit_test} PROPERTY DISABLED TRUE)
        return()
    endif()
    foreach(runner IN LISTS ARGN)
        set_property(TEST ${audit_test} APPEND PROPERTY
            FIXTURES_REQUIRED "ledger_${runner}")
    endforeach()
    set_property(TEST ${audit_test} APPEND PROPERTY ENVIRONMENT
        "ASOBMASHOW_LEDGER_EVIDENCE_DIR=${ASOBMASHOW_LEDGER_EVIDENCE_DIRECTORY}")
endfunction()
