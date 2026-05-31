#pragma once

#include <array>
#include <bgfx/bgfx.h>
#include <cstdint>

namespace rendering {
// Centralized view IDs.
inline constexpr bgfx::ViewId clear_view = 0;
inline constexpr bgfx::ViewId bga_view = 1;
inline constexpr bgfx::ViewId bga_layer_view = 2;
inline constexpr bgfx::ViewId blur_view_h = 3;
inline constexpr bgfx::ViewId blur_view_v = 4;
inline constexpr bgfx::ViewId final_view = 5;
inline constexpr bgfx::ViewId main_view = 128;
inline constexpr bgfx::ViewId ui_view = 254;

inline constexpr std::array<bgfx::ViewId, 4> kGameplayOutputViews = {
    clear_view, final_view, main_view, ui_view};
inline constexpr std::array<bgfx::ViewId, 2> kGameplayBgaInputViews = {
    bga_view, bga_layer_view};

void applyViewOrder(bgfx::ViewId blurViewH, bgfx::ViewId blurViewV,
                    bgfx::ViewId finalView);
} // namespace rendering
