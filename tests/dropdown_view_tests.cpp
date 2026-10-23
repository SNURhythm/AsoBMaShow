#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#include "../src/view/DropdownView.h"
#include "../src/view/OverlayPortal.h"
#undef private
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <cmath>
#include <cstdlib>

void testOverlayPlacementUsesWindowEdges() {
  const OverlayAnchor centered{.x = 100, .y = 100, .width = 200, .height = 42};
  const OverlayPlacement below =
      placeAnchoredOverlay(centered, 200, 160, 46, 800, 600, 10, 4);
  if (!below.opensBelow || below.x != 100 || below.y != 146 ||
      below.width != 200 || below.height != 160) {
    std::abort();
  }

  const OverlayAnchor nearBottom{
      .x = 100, .y = 520, .width = 200, .height = 42};
  const OverlayPlacement above =
      placeAnchoredOverlay(nearBottom, 200, 160, 46, 800, 600, 10, 4);
  if (above.opensBelow || above.x != 100 || above.y != 356 ||
      above.width != 200 || above.height != 160) {
    std::abort();
  }

  const OverlayAnchor nearRight{.x = 750, .y = 100, .width = 40, .height = 42};
  const OverlayPlacement shifted =
      placeAnchoredOverlay(nearRight, 200, 100, 46, 800, 600, 10, 4);
  if (shifted.x != 590 || shifted.width != 200) {
    std::abort();
  }

  const OverlayAnchor constrained{.x = 20, .y = 40, .width = 60, .height = 20};
  const OverlayPlacement clamped =
      placeAnchoredOverlay(constrained, 200, 120, 46, 100, 100, 10, 4);
  if (!clamped.opensBelow || clamped.x != 10 || clamped.y != 64 ||
      clamped.width != 80 || clamped.height != 26) {
    std::abort();
  }
}

void testOverlayPlacementRespectsSafeArea() {
  const rendering::UiSafeAreaInsets insets{.top = 40, .right = 30, .bottom = 20, .left = 50};
  for (const int y : {-200, 100, 350, 900}) {
    const auto placement = placeAnchoredOverlay(
        {.x = 780, .y = y, .width = 100, .height = 40},
        900, 2000, 46, 800, 600, 10, 4, insets);
    if (placement.x != 60 || placement.width != 700 ||
        placement.y < 50 || placement.y + placement.height > 570) std::abort();
  }
  const auto above = placeAnchoredOverlay(
      {.x = 100, .y = 350, .width = 200, .height = 40},
      200, 280, 46, 800, 600, 10, 4, insets);
  if (above.opensBelow || above.y != 66 || above.height != 280) std::abort();
  const auto tiny = placeAnchoredOverlay({}, 200, 500, 46, 8, 8, 10, 4, insets);
  if (tiny.width != 0 || tiny.height != 0 || tiny.x > 8 || tiny.y > 8) std::abort();
}

void testNormalizedOverlayAnchorUsesTriggerBounds() {
  const OverlayAnchor trigger{
      .x = 192, .y = 540, .width = 96, .height = 54};
  const NormalizedOverlayAnchor normalized =
      normalizeOverlayAnchor(trigger, 1920, 1080);
  constexpr float tolerance = 0.0001f;
  if (std::abs(normalized.x - 0.1f) > tolerance ||
      std::abs(normalized.y - 0.5f) > tolerance ||
      std::abs(normalized.width - 0.05f) > tolerance ||
      std::abs(normalized.height - 0.05f) > tolerance) {
    std::abort();
  }
}

int main() {
  testOverlayPlacementUsesWindowEdges();
  testOverlayPlacementRespectsSafeArea();
  testNormalizedOverlayAnchorUsesTriggerBounds();
  // Anchors can move outside the viewport when their parent scrolls.
  for (const int y : {-200, 900}) {
    const auto placement = placeAnchoredOverlay(
        {.x = -50, .y = y, .width = 100, .height = 40},
        200, 2000, 46, 800, 600, 10, 4);
    if (placement.y < 10 || placement.y + placement.height > 590) std::abort();
  }

  DropdownView::State state{.enabled = true};
  const DropdownView::Option unavailable{
      .id = "missing", .label = "Missing (Unavailable)", .available = false};
  if (DropdownView::optionSelectable(state, unavailable)) {
    return 1;
  }

  const DropdownView::Option available{
      .id = "working", .label = "Working", .available = true};
  if (!DropdownView::optionSelectable(state, available)) {
    return 2;
  }
  state.enabled = false;
  if (DropdownView::optionSelectable(state, available)) {
    return 3;
  }
  return 0;
}
