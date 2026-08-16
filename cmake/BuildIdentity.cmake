include_guard(GLOBAL)

set(_ASOBMASHOW_BUILD_IDENTITY_MODULE_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(asobmashow_configure_build_identity_refresh target source_directory)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "build identity target does not exist: ${target}")
    endif()

    get_filename_component(_asobmashow_source_directory
        "${source_directory}" REALPATH)
    set(_asobmashow_identity_directory
        "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>")
    set(_asobmashow_identity_header
        "${_asobmashow_identity_directory}/BuildIdentityConfig.h")
    set(_asobmashow_refresh_target "${target}_build_identity_refresh")

    # Custom targets intentionally run for every build. The script leaves the
    # generated header untouched when identity content is unchanged, so normal
    # incremental builds do not recompile BuildIdentity.cpp.
    add_custom_target(${_asobmashow_refresh_target}
        COMMAND "${CMAKE_COMMAND}"
            "-DASOBMASHOW_SOURCE_DIR=${_asobmashow_source_directory}"
            "-DASOBMASHOW_BUILD_CONFIGURATION=$<CONFIG>"
            "-DASOBMASHOW_OUTPUT_HEADER=${_asobmashow_identity_header}"
            -P "${_ASOBMASHOW_BUILD_IDENTITY_MODULE_DIR}/GenerateBuildIdentityConfig.cmake"
        VERBATIM
    )
    add_dependencies(${target} ${_asobmashow_refresh_target})
    target_include_directories(${target} PRIVATE
        "${_asobmashow_identity_directory}")
endfunction()
