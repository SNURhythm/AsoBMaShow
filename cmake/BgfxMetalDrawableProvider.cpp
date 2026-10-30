// Included once by the generated Metal backend. Provider functions execute on
// the renderer owner; registration may happen before or after bgfx init.
#include "../src/video/MetalDrawableProvider.h"
#include <atomic>

namespace {
std::atomic<const video::MetalDrawableProvider*> metalDrawableProvider{nullptr};
bool asobmashowMetalAcquireDrawable(void* layer, void** drawable)
{
    const auto* provider = metalDrawableProvider.load(std::memory_order_acquire);
    return provider && provider->acquire(layer, drawable);
}
bool asobmashowMetalShouldPresent(void* layer, void* drawable)
{
    const auto* provider = metalDrawableProvider.load(std::memory_order_acquire);
    return !provider || provider->shouldPresent(layer, drawable);
}
void asobmashowMetalWillPresent(void* layer, void* drawable, void* commandBuffer)
{
    const auto* provider = metalDrawableProvider.load(std::memory_order_acquire);
    if (provider && provider->willPresent)
        provider->willPresent(layer, drawable, commandBuffer);
}
}
extern "C" void asobmashowMetalSetDrawableProvider(const video::MetalDrawableProvider* provider)
{
    metalDrawableProvider.store(provider, std::memory_order_release);
}
