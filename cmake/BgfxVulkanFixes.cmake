# Keep the vendored checkout intact; compile a corrected copy of this one source.
# BGFX_TEXTURE_RT is the encoded single-sample selector, not an independent bit.
# OR-ing it into the MSAA selector turns 2x into 4x (and 8x into 16x), while
# the Vulkan pipeline still uses the requested count. Adreno renders corruptly.
function(asobmashow_fix_bgfx_vulkan)
    if(BGFX_AMALGAMATED)
        message(FATAL_ERROR "The bgfx Vulkan fixes require BGFX_AMALGAMATED=OFF")
    endif()

    set(original "${BGFX_DIR}/src/renderer_vk.cpp")
    set(corrected "${CMAKE_CURRENT_BINARY_DIR}/generated/bgfx/renderer_vk.cpp")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${original}")
    file(READ "${original}" source)
    set(broken "(uint64_t(samplerIndex + 1) << BGFX_TEXTURE_RT_MSAA_SHIFT) | BGFX_TEXTURE_RT | BGFX_TEXTURE_RT_WRITE_ONLY")
    set(fixed "(uint64_t(samplerIndex + 1) << BGFX_TEXTURE_RT_MSAA_SHIFT) | BGFX_TEXTURE_RT_WRITE_ONLY")
    string(FIND "${source}" "${broken}" match)
    if(match EQUAL -1)
        message(FATAL_ERROR "bgfx Vulkan source changed; review the local MSAA fix")
    endif()
    string(REPLACE "${broken}" "${fixed}" source "${source}")
    # Android's compositor rotates our identity-transform swapchain. In landscape
    # it can report SUBOPTIMAL after every successful present. Recreating the
    # identical swapchain only stalls the GPU and repeats the same result. Keep
    # acquired images usable; SDL resize/reset and OUT_OF_DATE still rebuild it.
    set(suboptimal_error "\t\t\tcase VK_ERROR_OUT_OF_DATE_KHR:\n\t\t\tcase VK_SUBOPTIMAL_KHR:")
    set(suboptimal_error_fixed "\t\t\tcase VK_ERROR_OUT_OF_DATE_KHR:\n#if !BX_PLATFORM_ANDROID\n\t\t\tcase VK_SUBOPTIMAL_KHR:\n#endif")
    set(swapchain_success "\t\t\tcase VK_SUCCESS:\n\t\t\t\tbreak;\n\n\t\t\tcase VK_ERROR_SURFACE_LOST_KHR:")
    set(swapchain_success_fixed "\t\t\tcase VK_SUCCESS:\n#if BX_PLATFORM_ANDROID\n\t\t\tcase VK_SUBOPTIMAL_KHR:\n#endif\n\t\t\t\tbreak;\n\n\t\t\tcase VK_ERROR_SURFACE_LOST_KHR:")
    set(present_result "\t\t\tswitch (result)\n\t\t\t{\n\t\t\tcase VK_ERROR_SURFACE_LOST_KHR:")
    set(present_result_fixed "\t\t\tswitch (result)\n\t\t\t{\n#if BX_PLATFORM_ANDROID\n\t\t\tcase VK_SUBOPTIMAL_KHR:\n\t\t\t\tbreak;\n#endif\n\t\t\tcase VK_ERROR_SURFACE_LOST_KHR:")
    foreach(pattern IN ITEMS suboptimal_error swapchain_success present_result)
        string(FIND "${source}" "${${pattern}}" match)
        if(match EQUAL -1)
            message(FATAL_ERROR "bgfx Vulkan source changed; review Android swapchain result handling")
        endif()
    endforeach()
    string(REPLACE "${suboptimal_error}" "${suboptimal_error_fixed}" source "${source}")
    string(REPLACE "${swapchain_success}" "${swapchain_success_fixed}" source "${source}")
    string(REPLACE "${present_result}" "${present_result_fixed}" source "${source}")
    # configure_file preserves the timestamp when the resulting source is unchanged.
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/bgfx")
    file(WRITE "${corrected}.in" "${source}")
    configure_file("${corrected}.in" "${corrected}" COPYONLY)

    get_target_property(sources bgfx SOURCES)
    list(FIND sources "${original}" source_index)
    if(source_index EQUAL -1)
        message(FATAL_ERROR "bgfx target no longer compiles renderer_vk.cpp separately")
    endif()
    list(REMOVE_ITEM sources "${original}")
    set_property(TARGET bgfx PROPERTY SOURCES "${sources}")
    target_sources(bgfx PRIVATE "${corrected}")
    set_source_files_properties("${corrected}" TARGET_DIRECTORY bgfx
        PROPERTIES INCLUDE_DIRECTORIES "${BGFX_DIR}/src")
    set(ASOBMASHOW_BGFX_VULKAN_SOURCE "${corrected}" PARENT_SCOPE)
endfunction()
