// Included by the generated Metal backend after its native type declarations.
#include "../src/video/MetalPresentationTiming.h"
#import <QuartzCore/QuartzCore.h>

namespace {
void asobmashowMetalRecordRenderTiming(MTL::CommandBuffer* buffer)
{
    const auto token = asobmashowMetalTimingToken();
    if (!token) return;
    using namespace video::presentationTiming;
    capture().record(token, Event::RenderSubmit, CACurrentMediaTime());
    buffer->addCompletedHandler(MTL::HandlerFunction([token](MTL::CommandBuffer* completed) {
        capture().record(token, Event::GpuStart, completed->GPUStartTime());
        capture().record(token, Event::GpuEnd, completed->GPUEndTime());
    }));
}
void asobmashowMetalRecordPresentTiming(MTL::CommandBuffer* buffer)
{
    const auto token = asobmashowMetalTimingToken();
    if (!token) return;
    using namespace video::presentationTiming;
    capture().record(token, Event::PresentRequest, CACurrentMediaTime());
    buffer->addScheduledHandler(MTL::HandlerFunction([token](MTL::CommandBuffer*) {
        capture().record(token, Event::PresentScheduled, CACurrentMediaTime());
    }));
}
}
