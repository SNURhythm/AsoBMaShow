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
#include <functional>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

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

static_assert(std::is_base_of_v<View, DropdownView>);
static_assert(std::is_constructible_v<DropdownView, DropdownView::Callbacks,
                                      OverlayPortal *>);
static_assert(std::is_same_v<decltype(DropdownView::State{}.options),
                             std::vector<DropdownView::Option>>);
static_assert(
    std::is_same_v<decltype(DropdownView::State{}.selectedId), std::string>);
static_assert(std::is_same_v<decltype(DropdownView::State{}.enabled), bool>);
static_assert(std::is_same_v<decltype(DropdownView::Option{}.available), bool>);
static_assert(std::is_same_v<decltype(DropdownView::Callbacks{}.onOpenChanged),
                             std::function<void(bool)>>);
static_assert(std::is_same_v<
              decltype(DropdownView::Callbacks{}.onOptionSelectedResult),
              std::function<bool(const std::string &)>>);
static_assert(DropdownView::kDefaultWidth == 160.0f);
static_assert(std::is_same_v<decltype(DropdownView::refreshIndicator(
                                 std::declval<View *>(),
                                 std::declval<const std::optional<Color> &>())),
                             bool>);
static_assert(
    std::is_same_v<decltype(std::declval<DropdownView &>().pendingRefresh),
                   std::optional<DropdownView::State>>);
static_assert(std::is_same_v<decltype(std::declval<DropdownView &>()
                                          .dispatchingOptionCallback),
                             bool>);
static_assert(std::is_same_v<
              decltype(std::declval<DropdownView &>().deferredRefreshScheduled),
              bool>);
static_assert(std::is_same_v<
              decltype(std::declval<DropdownView &>().optionsMatch(
                  std::declval<const std::vector<DropdownView::Option> &>())),
              bool>);
static_assert(
    std::is_same_v<decltype(std::declval<DropdownView &>().overlayPortal),
                   OverlayPortal *>);
static_assert(
    std::is_same_v<decltype(std::declval<DropdownView &>().menuOwnedByPortal),
                   bool>);

int main() {
  testOverlayPlacementUsesWindowEdges();
  testNormalizedOverlayAnchorUsesTriggerBounds();

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
