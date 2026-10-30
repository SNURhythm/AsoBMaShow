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
            // Renderer-owned dimensions stay stable throughout encoding, even
            // when UIKit changes the live layer before its event is consumed.
            const FrameBufferMtl* renderTarget = &s_renderMtl->m_mainFrameBuffer;
            for (uint32_t ii = 1; renderTarget->m_swapChain != this && ii < s_renderMtl->m_numWindows; ++ii)
            {
                renderTarget = &s_renderMtl->m_frameBuffers[s_renderMtl->m_windows[ii].idx];
            }
            BX_ASSERT(renderTarget->m_swapChain == this, "Missing Metal swap-chain render target");
            const uint32_t drawableWidth = renderTarget->m_width;
            const uint32_t drawableHeight = renderTarget->m_height;
            const MTL::PixelFormat drawableFormat = (s_renderMtl->m_resolution.reset & BGFX_RESET_SRGB_BACKBUFFER)
                ? s_textureFormat[m_colorFormat].fmtSrgb : s_textureFormat[m_colorFormat].fmt;
            void* providedDrawable = NULL;
            const bool managedDrawable = asobmashowMetalAcquireDrawable(m_metalLayer, &providedDrawable);
            m_drawable = occluded ? NULL : managedDrawable
                ? static_cast<CA::MetalDrawable*>(providedDrawable) : m_metalLayer->nextDrawable();
            if (m_drawable != NULL)
            {
                const MTL::Texture* texture = m_drawable->texture();
                if (texture->width() != drawableWidth || texture->height() != drawableHeight
                ||  texture->pixelFormat() != drawableFormat)
                {
                    // Not retained yet: provider/autorelease ownership releases
                    // it. Never present a drawable incompatible with attachments.
                    m_drawable = NULL;
                }
            }
]=])
    set(fallback_anchor [=[				desc->setPixelFormat(m_metalLayer->pixelFormat() );
				desc->setWidth(m_metalLayer->drawableSize().width);
				desc->setHeight(m_metalLayer->drawableSize().height);]=])
    set(fallback_fixed [=[
                // A rejected/missing lease must still encode safely against
                // the current depth/MSAA attachments; this texture is not shown.
                desc->setPixelFormat(drawableFormat);
                desc->setWidth(drawableWidth);
                desc->setHeight(drawableHeight);]=])
    set(extent_anchor "\t\tm_pixelFormatHash = m_swapChain->resize(_width, _height, _format, _depthFormat);")
    set(extent_fixed [=[
        m_width = _width;
        m_height = _height;
		m_pixelFormatHash = m_swapChain->resize(_width, _height, _format, _depthFormat);]=])
    set(present_anchor "presentCommandBuffer->presentDrawable( (MTL::Drawable*)frameBuffer.m_swapChain->m_drawable);")
    set(present_fixed [=[if (asobmashowMetalShouldPresent(frameBuffer.m_swapChain->m_metalLayer,
                            frameBuffer.m_swapChain->m_drawable))
                        {
                            asobmashowMetalWillPresent(frameBuffer.m_swapChain->m_metalLayer,
                                frameBuffer.m_swapChain->m_drawable, presentCommandBuffer);
                            presentCommandBuffer->presentDrawable( (MTL::Drawable*)frameBuffer.m_swapChain->m_drawable);
                        }]=])
    foreach(hook include acquire fallback extent present)
        string(FIND "${source}" "${${hook}_anchor}" offset)
        if(offset EQUAL -1)
            message(FATAL_ERROR "bgfx Metal presentation ${hook} changed; review display-link integration")
        endif()
        string(REPLACE "${${hook}_anchor}" "${${hook}_fixed}" source "${source}")
    endforeach()
    set(${content_variable} "${source}" PARENT_SCOPE)
endfunction()
