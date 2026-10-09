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
    # Android usually exposes FIFO and MAILBOX without IMMEDIATE. Mailbox lets
    # the producer run freely, replacing queued frames at the display boundary.
    set(immediate "\t\t{ VK_PRESENT_MODE_IMMEDIATE_KHR,    false, \"VK_PRESENT_MODE_IMMEDIATE_KHR\"    },")
    set(android_mailbox "#if BX_PLATFORM_ANDROID\n\t\t{ VK_PRESENT_MODE_MAILBOX_KHR,      false, \"VK_PRESENT_MODE_MAILBOX_KHR\"      },\n#endif\n${immediate}")
    string(FIND "${source}" "${immediate}" match)
    if(match EQUAL -1)
        message(FATAL_ERROR "bgfx Vulkan source changed; review Android VSync-off presentation")
    endif()
    string(REPLACE "${immediate}" "${android_mailbox}" source "${source}")
    # The tested Adreno's cached/coherent mapping is slower to read on the CPU than
    # cached non-coherent memory. Texture readbacks can use either: the GPU is
    # already finished, and explicit invalidation below makes its writes visible.
    # That driver excludes it when TRANSFER_SRC is also requested. Let the
    # texture path ask for a destination-only buffer; retain the default usage
    # and allocation policy for upload, buffer-readback and screenshot buffers.
    set(host_buffer_signature "const void* _data = NULL)\n\t\t{\n\t\t\tBGFX_PROFILER_SCOPE(\"RendererContextVK::createHostBuffer\", kColorResource);")
    set(host_buffer_signature_fixed "const void* _data = NULL, VkBufferUsageFlags _usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)\n\t\t{\n\t\t\tBGFX_PROFILER_SCOPE(\"RendererContextVK::createHostBuffer\", kColorResource);")
    set(host_buffer_usage "bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;")
    set(host_buffer_usage_fixed "bci.usage = _usage;")
    set(texture_readback [=[
		void readTexture(TextureHandle _handle, void* _data, uint8_t _mip) override
		{
			TextureVK& texture = m_textures[_handle.idx];

			uint32_t height = bx::uint32_max(1, texture.m_height >> _mip);
			uint32_t pitch  = texture.m_readback.pitch(_mip);
			uint32_t size = height * pitch;

			DeviceMemoryAllocationVK stagingMemory;
			VkBuffer stagingBuffer;
			VK_CHECK(createReadbackBuffer(size, &stagingBuffer, &stagingMemory) );
]=])
    set(texture_allocation "VK_CHECK(createReadbackBuffer(size, &stagingBuffer, &stagingMemory) );")
    set(cached_texture_allocation [=[
#if BX_PLATFORM_ANDROID
            // Do not require coherency: permit the faster cached memory type.
            // createHostBuffer retains its uncached fallback when necessary.
            VK_CHECK(createHostBuffer(size,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT
                , &stagingBuffer, &stagingMemory, true, NULL, VK_BUFFER_USAGE_TRANSFER_DST_BIT) );
#else
            VK_CHECK(createReadbackBuffer(size, &stagingBuffer, &stagingMemory) );
#endif
]=])
    string(REPLACE "${texture_allocation}" "${cached_texture_allocation}"
        texture_readback_fixed "${texture_readback}")
    set(readback_mapping [=[
		VK_CHECK(vkMapMemory(s_renderVK->m_device, _memory, 0, VK_WHOLE_SIZE, 0, (void**)&src) );
		src += _offset;
]=])
    set(readback_mapping_fixed [=[
		VK_CHECK(vkMapMemory(s_renderVK->m_device, _memory, 0, VK_WHOLE_SIZE, 0, (void**)&src) );
#if BX_PLATFORM_ANDROID
        // The allocation is mapped in full, so the range satisfies atom-size
        // alignment even when the image begins at a nonzero allocation offset.
        // Invalidation is a no-op for the coherent fallback and capture buffers.
        VkMappedMemoryRange range = {};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = _memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        VK_CHECK(vkInvalidateMappedMemoryRanges(s_renderVK->m_device, 1, &range) );
#endif
		src += _offset;
]=])
    foreach(pattern IN ITEMS host_buffer_signature host_buffer_usage texture_readback readback_mapping)
        string(FIND "${source}" "${${pattern}}" match)
        if(match EQUAL -1)
            message(FATAL_ERROR "bgfx Vulkan source changed; review Android cached texture readback")
        endif()
        string(REPLACE "${${pattern}}" "${${pattern}_fixed}" source "${source}")
    endforeach()
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
