include("${CMAKE_CURRENT_LIST_DIR}/BgfxMetalVideoSurfaces.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/BgfxMetalPresentation.cmake")

# Apply to the generated Metal source on both desktop and the iOS bgfx project.
function(asobmashow_patch_bgfx_metal_readback content_variable)
    set(source "${${content_variable}}")
    set(allocation "\t\t\t\tm_ptr = s_renderMtl->m_device->newTexture(desc);")
    set(allocation_fixed [=[
                // Export readbacks only need a linear GPU blit destination.
                // Buffer-backed textures avoid getBytes' texture conversion.
                // destroy() leaves externally owned pointers intact; this slot
                // may now be reused for an ordinary or readback texture.
                m_ptr = NULL;
                MTL::Device* device = s_renderMtl->m_device;
                bool unifiedMemory = BX_ENABLED(BX_PLATFORM_IOS) || BX_ENABLED(BX_PLATFORM_VISIONOS);
#if BX_PLATFORM_OSX
                if (__builtin_available(macOS 10.15, *))
                {
                    unifiedMemory = device->hasUnifiedMemory();
                }
#endif
#if TARGET_OS_SIMULATOR
                // Simulator permits linear textures only on private buffers;
                // use the ordinary texture/getBytes path for CPU readback.
                unifiedMemory = false;
#endif
                if (__builtin_available(macOS 10.13, iOS 11.0, *))
                {
                    if (unifiedMemory
                    &&  0 != (_flags & BGFX_TEXTURE_READ_BACK)
                    &&  0 != (_flags & BGFX_TEXTURE_BLIT_DST)
                    &&  !renderTarget && !computeWrite && !writeOnly
                    &&  desc->textureType() == MTL::TextureType2D
                    &&  desc->mipmapLevelCount() == 1 && desc->arrayLength() == 1
                    &&  (format == MTL::PixelFormatRGBA8Unorm || format == MTL::PixelFormatBGRA8Unorm) )
                    {
                        const NS::UInteger alignment = device->minimumLinearTextureAlignmentForPixelFormat(format);
                        const NS::UInteger pitch = ((desc->width() * 4 + alignment - 1) / alignment) * alignment;
                        MTL::Buffer* buffer = device->newBuffer(pitch * desc->height(),
                            MTL::ResourceStorageModeShared | MTL::ResourceCPUCacheModeDefaultCache);
                        const MTL::StorageMode originalStorage = desc->storageMode();
                        desc->setStorageMode(MTL::StorageModeShared);
                        m_ptr = buffer != NULL ? buffer->newTexture(desc, 0, pitch) : NULL;
                        // The texture retains its backing buffer until destruction.
                        MTL_RELEASE_I(buffer);
                        desc->setStorageMode(originalStorage);
                    }
                }
                if (NULL == m_ptr)
                {
                    m_ptr = s_renderMtl->m_device->newTexture(desc);
                }
]=])
    set(synchronize "\t\t\tbce->synchronizeTexture(texture.m_ptr, _layer, _mip);")
    set(synchronize_fixed [=[
            if (texture.m_ptr->storageMode() == MTL::StorageModeManaged)
            {
                bce->synchronizeTexture(texture.m_ptr, _layer, _mip);
            }
]=])
    set(blit_sync "const bool readBack = !!(dst.m_flags & BGFX_TEXTURE_READ_BACK);")
    set(blit_sync_fixed "const bool readBack = !!(dst.m_flags & BGFX_TEXTURE_READ_BACK) && dst.m_ptr->storageMode() == MTL::StorageModeManaged;")
    set(read "\t\t\ttexture.m_ptr->getBytes(_data, bytesPerRow, 0, region, _mip, _layer);")
    set(read_fixed [=[
            MTL::Texture* nativeTexture = texture.m_ptr;
            MTL::Buffer* buffer = nativeTexture->buffer();
            if (NULL != buffer && buffer->storageMode() == MTL::StorageModeShared
            &&  0 == _mip && 0 == _layer && nativeTexture->textureType() == MTL::TextureType2D
            &&  (nativeTexture->pixelFormat() == MTL::PixelFormatRGBA8Unorm
              || nativeTexture->pixelFormat() == MTL::PixelFormatBGRA8Unorm) )
            {
                const uint8_t* src = (const uint8_t*)buffer->contents() + nativeTexture->bufferOffset();
                const NS::UInteger pitch = nativeTexture->bufferBytesPerRow();
                for (uint32_t row = 0; row < srcHeight; ++row)
                {
                    bx::memCopy((uint8_t*)_data + row * srcWidth * 4, src + row * pitch, srcWidth * 4);
                }
            }
            else
            {
                texture.m_ptr->getBytes(_data, bytesPerRow, 0, region, _mip, _layer);
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
    set(original "${BGFX_DIR}/src/renderer_mtl.cpp")
    get_target_property(sources bgfx SOURCES)
    if(NOT original IN_LIST sources)
        message(FATAL_ERROR "bgfx Metal source layout changed; review readback optimization")
    endif()
    file(READ "${original}" content)
    asobmashow_patch_bgfx_metal_readback(content)
    asobmashow_patch_bgfx_metal_video_surfaces(content)
    asobmashow_patch_bgfx_metal_presentation(content)
    set(patched "${CMAKE_CURRENT_BINARY_DIR}/generated/bgfx/renderer_mtl.mm")
    file(WRITE "${patched}.in" "${content}")
    configure_file("${patched}.in" "${patched}" COPYONLY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${original}")
    list(REMOVE_ITEM sources "${original}")
    list(APPEND sources "${patched}")
    set_property(TARGET bgfx PROPERTY SOURCES "${sources}")
    # The generated .mm needs the same language level as upstream metal-cpp.
    set_target_properties(bgfx PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
    target_include_directories(bgfx PRIVATE "${BGFX_DIR}/src")
endfunction()
