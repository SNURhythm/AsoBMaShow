# Loaded by project(bgfx), then applied after that project creates its target.
# iOS 16 can report every MTLBinding as unused outside Xcode, so bgfx skips
# shader uniforms and renders a black screen. Keep the legacy reflection path
# through iOS 16; use MTLBinding on iOS 17 and later.
# https://github.com/bkaradzic/bgfx/issues/3392#issuecomment-2582292135
include("${CMAKE_CURRENT_LIST_DIR}/BgfxMetalReadback.cmake")

function(asobmashow_ios_bgfx_metal_compatibility)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
        return()
    endif()
    if(BGFX_AMALGAMATED)
        message(FATAL_ERROR "iOS Metal compatibility requires non-amalgamated bgfx")
    endif()

    set(original "${BGFX_DIR}/src/renderer_mtl.mm")
    get_target_property(sources bgfx SOURCES)
    if(NOT original IN_LIST sources)
        message(FATAL_ERROR "bgfx Metal source layout changed; review iOS 16 compatibility")
    endif()
    file(READ "${original}" content)
    set(old_gate "m_usesMTLBindings, macOS 13.0, iOS 16.0,")
    set(new_gate "m_usesMTLBindings, macOS 13.0, iOS 17.0,")
    string(FIND "${content}" "${old_gate}" gate_offset)
    if(gate_offset EQUAL -1)
        message(FATAL_ERROR "bgfx Metal reflection gate changed; review iOS 16 compatibility")
    endif()
    string(REPLACE "${old_gate}" "${new_gate}" content "${content}")
    asobmashow_patch_bgfx_metal_readback(content)

    # Compile a generated copy, leaving the pinned upstream submodule intact.
    # COPYONLY preserves the output timestamp when its contents are unchanged.
    set(patched "${CMAKE_CURRENT_BINARY_DIR}/ios-compat/renderer_mtl.mm")
    file(WRITE "${patched}.in" "${content}")
    configure_file("${patched}.in" "${patched}" COPYONLY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${original}")
    list(REMOVE_ITEM sources "${original}")
    list(APPEND sources "${patched}")
    set_property(TARGET bgfx PROPERTY SOURCES "${sources}")
    target_include_directories(bgfx PRIVATE "${BGFX_DIR}/src")
endfunction()

cmake_language(DEFER CALL asobmashow_ios_bgfx_metal_compatibility)
