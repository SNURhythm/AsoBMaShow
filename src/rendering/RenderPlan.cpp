#include "RenderPlan.h"

namespace rendering {
void applyViewOrder(bgfx::ViewId blurViewH, bgfx::ViewId blurViewV,
                    bgfx::ViewId finalView) {
  // Finish offscreen work before the output clear acquires a drawable.
  const std::array<bgfx::ViewId, 9> order = {
      bga_view, bga_layer_view, blurViewH, blurViewV,
      clear_view, finalView, main_view,    ui_view,
      readback_view,
  };
  bgfx::setViewOrder(0, static_cast<uint16_t>(order.size()), order.data());
}
} // namespace rendering
