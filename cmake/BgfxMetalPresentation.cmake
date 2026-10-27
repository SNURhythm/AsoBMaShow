# Keep a fixed drawable pool and pace iOS using actual presentation completion.
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
    set(anchor "#include <bx/pixelformat.h>")
    string(FIND "${source}" "${anchor}" include_offset)
    if(include_offset EQUAL -1)
        message(FATAL_ERROR "bgfx Metal includes changed; review presentation completion gate")
    endif()
    set(gate_include "\n#if BX_PLATFORM_IOS\n#import <Metal/Metal.h>\n#include \"${CMAKE_CURRENT_FUNCTION_LIST_DIR}/BgfxMetalPresentationGate.cpp\"\n#endif\n")
    string(REPLACE "${anchor}" "${anchor}${gate_include}" source "${source}")
    set(present "presentCommandBuffer->presentDrawable( (MTL::Drawable*)frameBuffer.m_swapChain->m_drawable);")
    string(FIND "${source}" "${present}" present_offset)
    if(present_offset EQUAL -1)
        message(FATAL_ERROR "bgfx Metal present changed; review presentation completion gate")
    endif()
    set(completion [=[
#if BX_PLATFORM_IOS
                        if (ii == 0)
                        {
                            const auto token = asobmashow::metalPresentation::begin();
                            [(id<MTLDrawable>)frameBuffer.m_swapChain->m_drawable addPresentedHandler:^(id<MTLDrawable>) {
                                asobmashow::metalPresentation::complete(token);
                            }];
                        }
#endif
                        ]=])
    string(REPLACE "${present}" "${completion}${present}" source "${source}")
    set(${content_variable} "${source}" PARENT_SCOPE)
endfunction()
