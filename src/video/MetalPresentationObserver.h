#pragma once

namespace video {
enum class MetalPresentationEvent { DrawableWait, Presented };
// Times are in the Core Animation host clock, in seconds. Observers must be
// thread-safe and remain callable until bgfx shutdown, including after removal.
using MetalPresentationObserver = void (*)(MetalPresentationEvent, double, double);
}

extern "C" void asobmashowMetalSetPresentationObserver(video::MetalPresentationObserver);
