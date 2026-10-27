#include "BgfxMetalPresentationGate.h"
#include <Block.h>
#include <cstdint>
#include <cstdlib>
#include <iostream>

using namespace asobmashow::metalPresentation;
static void require(bool value, const char* message)
{
    if (!value) { std::cerr << message << '\n'; std::abort(); }
}
namespace MTL {
struct Drawable
{
    void (^handler)(Drawable*) = nullptr;
    int presentations = 0;
    ~Drawable() { if (handler) Block_release(handler); }
    void addPresentedHandler(void (^callback)(Drawable*))
    {
        require(!handler, "duplicate handler registration");
        handler = Block_copy(callback);
    }
    void present() { ++presentations; if (handler) handler(this); }
};
}
struct FakeMetalLayer
{
    MTL::Drawable* next = nullptr;
    int acquisitions = 0;
    MTL::Drawable* nextDrawable() { ++acquisitions; return next; }
};
struct SwapChain;
struct Renderer { struct { SwapChain* m_swapChain = nullptr; } m_mainFrameBuffer; } renderer;
static Renderer* s_renderMtl = &renderer;
static bool windowOccluded = false;
static bool isWindowOccluded(void*) { return windowOccluded; }
struct SwapChain
{
    void* m_nwh = nullptr;
    FakeMetalLayer* m_metalLayer;
    MTL::Drawable* m_drawable = nullptr;
    void acquireDrawable()
    {
#include "metal_presentation_hook.inc"
    }
};
static bool yes(void*) { return true; }
int main()
{
    FakeMetalLayer layer;
    MTL::Drawable first, second, third;
    auto* primary = new SwapChain{nullptr, &layer};
    renderer.m_mainFrameBuffer.m_swapChain = primary;
    {
        Scope submission(yes, nullptr);
        layer.next = &first; primary->acquireDrawable();
        layer.next = nullptr; primary->acquireDrawable(); // nil must return permit
        layer.next = &second; primary->acquireDrawable();
    }
    require(layer.acquisitions == 3, "native nextDrawable path changed");
#if BX_PLATFORM_IOS
    require(first.handler && second.handler, "primary iOS callbacks missing or nil leaked slot");
#else
    require(!first.handler && !second.handler, "non-iOS registered callback");
#endif
    // Lifetime: callbacks outlive both the submission scope and native swapchain.
    delete primary;
    renderer.m_mainFrameBuffer.m_swapChain = nullptr;
    second.present(); first.present(); first.present();
    require(first.presentations == 2, "ordinary presentation path changed");
    {
        Scope submission(yes, nullptr);
        SwapChain secondary{nullptr, &layer};
        layer.next = &third; secondary.acquireDrawable();
        require(!third.handler, "secondary swapchain was paced");
        renderer.m_mainFrameBuffer.m_swapChain = &secondary;
        { Scope discard(nullptr, nullptr); secondary.acquireDrawable(); }
        require(!third.handler, "discard submission was paced");
    }
    // No scope (export worker) uses ordinary acquisition without callback.
    SwapChain unscoped{nullptr, &layer};
    renderer.m_mainFrameBuffer.m_swapChain = &unscoped;
    unscoped.acquireDrawable();
    require(!third.handler, "unscoped acquisition was paced");
    cancel();
#if BX_PLATFORM_IOS
    {
        int predicateCalls = 0;
        Scope submission([](void* p) { ++*static_cast<int*>(p); return true; }, &predicateCalls);
        windowOccluded = true;
        const auto acquisitions = layer.acquisitions;
        unscoped.acquireDrawable();
        require(layer.acquisitions == acquisitions, "occluded window acquired native drawable");
        require(predicateCalls == 0, "occluded window entered acquisition gate");
    }
#endif
    std::cout << "generated native acquisition contracts passed (iOS=" << BX_PLATFORM_IOS << ")\n";
}
