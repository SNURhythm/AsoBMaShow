# Observe the actual drawable timestamp without changing the presentation
# policy. No callback, clock read, or block allocation when telemetry is off.
function(asobmashow_patch_bgfx_metal_presentation content_variable)
    set(source "${${content_variable}}")
    # Use the same three-drawable pool on macOS as on iOS. Two drawables
    # can throttle presentation even when the GPU finishes within a refresh.
    set(drawable_count [=[m_metalLayer.maximumDrawableCount = bx::clamp<uint32_t>(
				  maxFrameLatency != 0 ? maxFrameLatency : BGFX_CONFIG_MAX_FRAME_LATENCY
				, 2
				, 3
				);]=])
    string(FIND "${source}" "${drawable_count}" count_offset)
    if(count_offset EQUAL -1)
        message(FATAL_ERROR "bgfx drawable count changed; review fixed Metal drawable pool")
    endif()
    string(REPLACE "${drawable_count}" "m_metalLayer.maximumDrawableCount = 3;" source "${source}")
    set(anchor "#define UNIFORM_BUFFER_SIZE (8*1024*1024)")
    set(helper [=[
#include "video/MetalPresentationObserver.h"
#include <atomic>
#import <QuartzCore/QuartzCore.h>

namespace {
std::atomic<video::MetalPresentationObserver> asoMetalPresentationObserver{nullptr};
void asoObserveMetalPresentation(id<MTLDrawable> drawable)
{
#if !TARGET_OS_SIMULATOR
    // The simulator's MTLDrawable protocol omits presentation timestamps.
    const auto observer = asoMetalPresentationObserver.load(std::memory_order_acquire);
    if (observer == nullptr) return;
    if (@available(macOS 10.15, iOS 10.3, *))
    {
        const double requested = CACurrentMediaTime();
        [drawable addPresentedHandler:^(id<MTLDrawable> presented) {
            observer(video::MetalPresentationEvent::Presented, requested, presented.presentedTime);
        }];
    }
#else
    (void)drawable;
#endif
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
