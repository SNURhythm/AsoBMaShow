#pragma once

namespace video {
// Optional borrowed-drawable source. Returning true from acquire means this
// layer is managed even when *drawable is null; never call nextDrawable then.
// Attached links remain managed between frames and after a lease is consumed.
// The source keeps the drawable alive until bgfx has retained it. Callbacks run
// on the Metal renderer thread and must remain valid until bgfx shutdown.
struct MetalDrawableProvider {
  bool (*acquire)(void *layer, void **drawable);
  bool (*shouldPresent)(void *layer, void *drawable);
  // Optional diagnostics immediately before presentDrawable. Implementations
  // may observe scheduling/display asynchronously, never change submission.
  void (*willPresent)(void *layer, void *drawable, void *commandBuffer) = nullptr;
};
}

extern "C" void asobmashowMetalSetDrawableProvider(const video::MetalDrawableProvider *);
