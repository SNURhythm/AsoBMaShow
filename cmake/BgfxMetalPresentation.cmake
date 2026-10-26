# Observe the actual drawable timestamp without changing the presentation
# policy. No callback, clock read, or block allocation when telemetry is off.
function(asobmashow_patch_bgfx_metal_presentation content_variable)
    set(source "${${content_variable}}")
    # Keep the app-owned bit outside every flag in the pinned bgfx API.
    file(READ "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../bgfx/bgfx/include/bgfx/defines.h" defines)
    string(REGEX MATCHALL "#define BGFX_RESET_[^\n]+UINT32_C\\(0x[0-9a-fA-F]+\\)" reset_definitions "${defines}")
    if(NOT reset_definitions)
        message(FATAL_ERROR "bgfx reset definitions changed; review Metal flag collision check")
    endif()
    foreach(definition IN LISTS reset_definitions)
        string(REGEX MATCH "0x[0-9a-fA-F]+" value "${definition}")
        math(EXPR collision "${value} & 0x40000000")
        if(collision)
            message(FATAL_ERROR "bgfx reset flag collision; review Metal triple buffering")
        endif()
    endforeach()
    set(drawable_count [=[maxFrameLatency != 0 ? maxFrameLatency : BGFX_CONFIG_MAX_FRAME_LATENCY
				, 2
				, 3]=])
    string(FIND "${source}" "${drawable_count}" count_offset)
    if(count_offset EQUAL -1)
        message(FATAL_ERROR "bgfx drawable count changed; review triple buffering")
    endif()
    string(REPLACE "${drawable_count}" "video::metalDrawableCount(resetFlags), 2, 3" source "${source}")
    set(anchor "#define UNIFORM_BUFFER_SIZE (8*1024*1024)")
    set(helper [=[
#include "video/MetalPresentationObserver.h"
#include "video/MetalDrawableCount.h"
#include <atomic>
#import <QuartzCore/QuartzCore.h>

namespace {
std::atomic<video::MetalPresentationObserver> asoMetalPresentationObserver{nullptr};
void asoObserveMetalPresentation(id<MTLDrawable> drawable)
{
    const auto observer = asoMetalPresentationObserver.load(std::memory_order_acquire);
    if (observer == nullptr) return;
    if (@available(macOS 10.15, iOS 10.3, *))
    {
        const double requested = CACurrentMediaTime();
        [drawable addPresentedHandler:^(id<MTLDrawable> presented) {
            observer(video::MetalPresentationEvent::Presented, requested, presented.presentedTime);
        }];
    }
}
}
extern "C" void asobmashowMetalSetPresentationObserver(video::MetalPresentationObserver observer)
{
    asoMetalPresentationObserver.store(observer, std::memory_order_release);
}
]=])
    set(present "m_commandBuffer.presentDrawable(frameBuffer.m_swapChain->m_drawable);")
    set(present_fixed [=[
                            if (ii == 0) asoObserveMetalPresentation(frameBuffer.m_swapChain->m_drawable);
                            m_commandBuffer.presentDrawable(frameBuffer.m_swapChain->m_drawable);
]=])
    set(acquire "m_drawable = m_metalLayer.nextDrawable;")
    set(acquire_fixed [=[
            const auto observer = asoMetalPresentationObserver.load(std::memory_order_acquire);
            const double started = observer != nullptr ? CACurrentMediaTime() : 0.0;
            m_drawable = m_metalLayer.nextDrawable;
            if (observer != nullptr)
                observer(video::MetalPresentationEvent::DrawableWait, started, CACurrentMediaTime());
]=])
    foreach(pattern IN ITEMS anchor present acquire)
        string(FIND "${source}" "${${pattern}}" offset)
        if(offset EQUAL -1)
            message(FATAL_ERROR "bgfx Metal presentation changed; review observer (${pattern})")
        endif()
    endforeach()
    string(REPLACE "${anchor}" "${helper}\n${anchor}" source "${source}")
    string(REPLACE "${present}" "${present_fixed}" source "${source}")
    string(REPLACE "${acquire}" "${acquire_fixed}" source "${source}")
    set(${content_variable} "${source}" PARENT_SCOPE)
endfunction()
