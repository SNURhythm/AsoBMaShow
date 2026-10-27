#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "BgfxMetalPresentationGate.h"
#include <cstdlib>
#include <iostream>

using namespace asobmashow::metalPresentation;
using namespace std::chrono_literals;
static int destroyed = 0;
static void require(bool condition, const char* message)
{
    if (!condition) { std::cerr << message << '\n'; std::abort(); }
}

// Stand in only for the Metal drawable: run the real generated registration
// block and completion gate without needing a window or physical display.
@interface PresentationDrawable : NSObject {
@public
    MTLDrawablePresentedHandler callback;
}
- (void)addPresentedHandler:(MTLDrawablePresentedHandler)handler;
@end
@implementation PresentationDrawable
- (void)addPresentedHandler:(MTLDrawablePresentedHandler)handler {
    require(callback == nil, "registered more than one callback");
    callback = [handler copy];
}
- (void)dealloc { ++destroyed; [callback release]; [super dealloc]; }
@end

namespace MTL { struct Drawable; }
struct CommandBuffer {
    int presentations = 0;
    void presentDrawable(MTL::Drawable*) { ++presentations; }
};
static void flip(unsigned ii, PresentationDrawable* drawable, CommandBuffer* presentCommandBuffer)
{
    struct SwapChain { void* m_drawable; } swapChain{drawable};
    struct FrameBuffer { SwapChain* m_swapChain; } frameBuffer{&swapChain};
#include "metal_presentation_hook.inc"
}

int main()
{
    @autoreleasepool {
        CommandBuffer commands;
        auto* drawable = [PresentationDrawable new];
        const auto owners = [drawable retainCount];
        flip(0, drawable, &commands);
        require(commands.presentations == 1, "changed ordinary presentation count");
#if BX_PLATFORM_IOS
        require(drawable->callback != nil, "primary presentation handler missing");
        require([drawable retainCount] == owners, "handler retained drawable");
        MTLDrawablePresentedHandler callback = [drawable->callback copy];
        [drawable release];
        require(destroyed == 1, "copied callback kept drawable alive");
        callback(nil);
        require(wait(nullptr, nullptr, 0ms) == WaitResult::Completed, "native callback did not complete gate");
        begin();
        callback(nil);
        require(wait(nullptr, nullptr, 0ms) == WaitResult::TimedOut, "late native callback released a fresh token");
        [callback release];
#else
        require(drawable->callback == nil, "pacing activated outside iOS");
        [drawable release];
#endif
        drawable = [PresentationDrawable new];
        flip(1, drawable, &commands);
        require(drawable->callback == nil, "secondary window registered pacing callback");
        require(commands.presentations == 2, "secondary presentation changed");
        require(wait(nullptr, nullptr, 0ms) == WaitResult::Completed, "secondary window created a pending wait");
        [drawable release];
        require(destroyed == 2, "drawable lifetime changed");
    }
    std::cout << "native presentation callback contracts passed\n";
}
