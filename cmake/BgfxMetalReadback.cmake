# Apply to the generated Metal source on both desktop and the iOS bgfx project.
function(asobmashow_patch_bgfx_metal_readback content_variable)
    set(source "${${content_variable}}")
    set(allocation "\t\t\t\tm_ptr = s_renderMtl->m_device.newTextureWithDescriptor(desc);")
    set(allocation_fixed [=[
                // Export readbacks only need a linear GPU blit destination.
                // Buffer-backed textures avoid getBytes' texture conversion.
                // destroy() leaves externally owned pointers intact; this slot
                // may now be reused for an ordinary or readback texture.
                m_ptr = NULL;
                id<MTLDevice> device = s_renderMtl->m_device;
                bool unifiedMemory = BX_ENABLED(BX_PLATFORM_IOS) || BX_ENABLED(BX_PLATFORM_VISIONOS);
#if BX_PLATFORM_OSX
                if (@available(macOS 10.15, *))
                {
                    unifiedMemory = device.hasUnifiedMemory;
                }
#endif
                if (@available(macOS 10.13, iOS 11.0, *))
                {
                    if (unifiedMemory
                    &&  0 != (_flags & BGFX_TEXTURE_READ_BACK)
                    &&  0 != (_flags & BGFX_TEXTURE_BLIT_DST)
                    &&  !renderTarget && !computeWrite && !writeOnly
                    &&  desc.textureType == MTLTextureType2D
                    &&  desc.mipmapLevelCount == 1 && desc.arrayLength == 1
                    &&  (format == MTLPixelFormatRGBA8Unorm || format == MTLPixelFormatBGRA8Unorm) )
                    {
                        const NSUInteger alignment = [device minimumLinearTextureAlignmentForPixelFormat:format];
                        const NSUInteger pitch = ((desc.width * 4 + alignment - 1) / alignment) * alignment;
                        id<MTLBuffer> buffer = [device newBufferWithLength:pitch * desc.height
                            options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeDefaultCache];
                        const MTLStorageMode originalStorage = desc.storageMode;
                        desc.storageMode = MTLStorageModeShared;
                        m_ptr = [buffer newTextureWithDescriptor:desc offset:0 bytesPerRow:pitch];
                        // The texture retains its backing buffer until destruction.
                        MTL_RELEASE_I(buffer);
                        desc.storageMode = originalStorage;
                    }
                }
                if (NULL == m_ptr)
                {
                    m_ptr = s_renderMtl->m_device.newTextureWithDescriptor(desc);
                }
]=])
    set(synchronize "\t\t\tbce.synchronizeTexture(texture.m_ptr, 0, _mip);")
    set(synchronize_fixed [=[
            if (texture.m_ptr.m_obj.storageMode == MTLStorageModeManaged)
            {
                bce.synchronizeTexture(texture.m_ptr, 0, _mip);
            }
]=])
    set(blit_sync "const bool readBack = !!(dst.m_flags & BGFX_TEXTURE_READ_BACK);")
    set(blit_sync_fixed "const bool readBack = !!(dst.m_flags & BGFX_TEXTURE_READ_BACK) && dst.m_ptr.m_obj.storageMode == MTLStorageModeManaged;")
    set(read "\t\t\ttexture.m_ptr.getBytes(_data, srcWidth*bpp/8, 0, region, _mip, 0);")
    set(read_fixed [=[
            id<MTLTexture> nativeTexture = texture.m_ptr;
            id<MTLBuffer> buffer = nativeTexture.buffer;
            if (nil != buffer && buffer.storageMode == MTLStorageModeShared
            &&  0 == _mip && nativeTexture.textureType == MTLTextureType2D
            &&  (nativeTexture.pixelFormat == MTLPixelFormatRGBA8Unorm
              || nativeTexture.pixelFormat == MTLPixelFormatBGRA8Unorm) )
            {
                const uint8_t* src = (const uint8_t*)buffer.contents + nativeTexture.bufferOffset;
                const NSUInteger pitch = nativeTexture.bufferBytesPerRow;
                for (uint32_t row = 0; row < srcHeight; ++row)
                {
                    bx::memCopy((uint8_t*)_data + row * srcWidth * 4, src + row * pitch, srcWidth * 4);
                }
            }
            else
            {
                texture.m_ptr.getBytes(_data, srcWidth*bpp/8, 0, region, _mip, 0);
            }
]=])
    foreach(pattern IN ITEMS allocation synchronize blit_sync read)
        string(FIND "${source}" "${${pattern}}" match)
        if(match EQUAL -1)
            message(FATAL_ERROR "bgfx Metal source changed; review buffer-backed readback (${pattern})")
        endif()
        string(REPLACE "${${pattern}}" "${${pattern}_fixed}" source "${source}")
    endforeach()
    set(${content_variable} "${source}" PARENT_SCOPE)
endfunction()

function(asobmashow_fix_bgfx_metal)
    if(NOT APPLE)
        return()
    endif()
    if(BGFX_AMALGAMATED)
        message(FATAL_ERROR "Metal readback optimization requires non-amalgamated bgfx")
    endif()
    set(original "${BGFX_DIR}/src/renderer_mtl.mm")
    get_target_property(sources bgfx SOURCES)
    if(NOT original IN_LIST sources)
        message(FATAL_ERROR "bgfx Metal source layout changed; review readback optimization")
    endif()
    file(READ "${original}" content)
    asobmashow_patch_bgfx_metal_readback(content)
    set(patched "${CMAKE_CURRENT_BINARY_DIR}/generated/bgfx/renderer_mtl.mm")
    file(WRITE "${patched}.in" "${content}")
    configure_file("${patched}.in" "${patched}" COPYONLY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${original}")
    list(REMOVE_ITEM sources "${original}")
    list(APPEND sources "${patched}")
    set_property(TARGET bgfx PROPERTY SOURCES "${sources}")
    target_include_directories(bgfx PRIVATE "${BGFX_DIR}/src")
endfunction()
