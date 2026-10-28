# Keep the fixed drawable pool and the display-link drawable provider.
function(asobmashow_patch_bgfx_metal_presentation content_variable)
    set(source "${${content_variable}}")
    # Use the same three-drawable pool on macOS as on iOS. Two drawables
    # can throttle presentation even when the GPU finishes within a refresh.
    set(drawable_count [=[m_metalLayer->setMaximumDrawableCount(bx::clamp<uint32_t>(
				  maxFrameLatency != 0 ? maxFrameLatency : BGFX_CONFIG_MAX_FRAME_LATENCY
				, 2
				, 3
				) );]=])
    string(FIND "${source}" "${drawable_count}" count_offset)
    if(count_offset EQUAL -1)
        message(FATAL_ERROR "bgfx drawable count changed; review fixed Metal drawable pool")
    endif()
    string(REPLACE "${drawable_count}" "m_metalLayer->setMaximumDrawableCount(3);" source "${source}")
    set(include_anchor "#include \"renderer_mtl.h\"")
    set(include_fixed "#include \"${CMAKE_CURRENT_FUNCTION_LIST_DIR}/BgfxMetalDrawableProvider.cpp\"\n${include_anchor}")
    set(acquire_anchor "\t\t\tm_drawable = occluded ? NULL : m_metalLayer->nextDrawable();")
    set(acquire_fixed [=[
            void* providedDrawable = NULL;
            const bool managedDrawable = asobmashowMetalAcquireDrawable(m_metalLayer, &providedDrawable);
            m_drawable = occluded ? NULL : managedDrawable
                ? static_cast<CA::MetalDrawable*>(providedDrawable) : m_metalLayer->nextDrawable();
]=])
    set(present_anchor "presentCommandBuffer->presentDrawable( (MTL::Drawable*)frameBuffer.m_swapChain->m_drawable);")
    set(present_fixed [=[if (asobmashowMetalShouldPresent(frameBuffer.m_swapChain->m_metalLayer,
                            frameBuffer.m_swapChain->m_drawable))
                        {
                            presentCommandBuffer->presentDrawable( (MTL::Drawable*)frameBuffer.m_swapChain->m_drawable);
                        }]=])
    foreach(hook include acquire present)
        string(FIND "${source}" "${${hook}_anchor}" offset)
        if(offset EQUAL -1)
            message(FATAL_ERROR "bgfx Metal presentation ${hook} changed; review display-link integration")
        endif()
        string(REPLACE "${${hook}_anchor}" "${${hook}_fixed}" source "${source}")
    endforeach()
    set(${content_variable} "${source}" PARENT_SCOPE)
endfunction()
