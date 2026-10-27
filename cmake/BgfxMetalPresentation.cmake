# Keep the fixed drawable pool and pace primary iOS drawable acquisition.
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
    set(include_fixed "#if BX_PLATFORM_IOS\n#include \"${CMAKE_CURRENT_FUNCTION_LIST_DIR}/BgfxMetalPresentationGate.cpp\"\n#endif\n${include_anchor}")
    set(acquire_anchor "\t\t\tm_drawable = occluded ? NULL : m_metalLayer->nextDrawable();")
    set(acquire_fixed [=[
#if BX_PLATFORM_IOS
            const uint64_t presentationToken = !occluded && this == s_renderMtl->m_mainFrameBuffer.m_swapChain
                ? asobmashow::metalPresentation::acquire() : 0;
#endif
            m_drawable = occluded ? NULL : m_metalLayer->nextDrawable();
#if BX_PLATFORM_IOS
            if (presentationToken != 0)
            {
                if (m_drawable == NULL)
                {
                    asobmashow::metalPresentation::complete(presentationToken);
                }
                else
                {
                    m_drawable->addPresentedHandler(^(MTL::Drawable*) {
                        asobmashow::metalPresentation::complete(presentationToken);
                    });
                }
            }
#endif
]=])
    set(init_anchor "\tvoid SwapChainMtl::init(void* _nwh)\n\t{")
    set(destroy_anchor "\tSwapChainMtl::~SwapChainMtl()\n\t{")
    set(reset [=[
#if BX_PLATFORM_IOS
        if (this == s_renderMtl->m_mainFrameBuffer.m_swapChain)
        {
            asobmashow::metalPresentation::cancel();
        }
#endif
]=])
    set(init_fixed "${init_anchor}\n${reset}")
    set(destroy_fixed "${destroy_anchor}\n${reset}")
    foreach(hook include acquire init destroy)
        string(FIND "${source}" "${${hook}_anchor}" offset)
        if(offset EQUAL -1)
            message(FATAL_ERROR "bgfx Metal presentation ${hook} changed; review acquisition pacing")
        endif()
        string(REPLACE "${${hook}_anchor}" "${${hook}_fixed}" source "${source}")
    endforeach()
    set(${content_variable} "${source}" PARENT_SCOPE)
endfunction()
