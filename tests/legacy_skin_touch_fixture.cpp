#include "scene/play/RealtimeTouchInputRouter.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <utility>
#include <vector>

using SDL_FingerID = long long;
using Uint32 = unsigned;
Uint32 SDL_GetTicks() { return 0; }
template <class... Args> void SDL_Log(Args...) {}
struct Vector3 { float x, y, z; };
namespace bx { using Vec3 = Vector3; }
namespace bms_parser {
struct Note { bool IsLongNote() const { return false; } };
struct LongNote : Note { bool IsHolding = false; };
}
enum class ReplayTouchAction { Down, Move, Up, Cancel };
struct FlickState {
  float startX, startY;
  Uint32 startTime;
  bool active;
  int lastFlickDirection;
  bms_parser::LongNote *activeLongNote;
};
bool hasActiveLongNote(FlickState &state) {
  return state.activeLongNote && state.activeLongNote->IsHolding;
}
namespace rendering {
int render_width = 1000, render_height = 500;
int window_width = 800, window_height = 400;
float ui_scale_x = 1.0F, ui_scale_y = 1.0F;
int ui_offset_x = 100, ui_offset_y = 50;
struct Camera {
  float getNearClip() const { return 0; }
  float getFarClip() const { return 1; }
  bx::Vec3 deproject(float x, float y, float z) const {
    return {x / render_width * 8, y / render_height, z};
  }
} game_camera;
}

// Only the platform/camera and logical-input sink are substitutes. Finger
// ownership, drag, scratch, hit testing and layout invalidation are extracted
// unchanged from production below.
class RhythmInputHandler {
public:
  int totalLaneCount = 8, scratchLaneCount = 1;
  float playAreaWidth = 8, playAreaLeftX = 0;
  bool dragModeEnabled = false;
  std::vector<int> laneOrder{0, 1, 2, 3, 4, 5, 6, 7};
  std::optional<gameplay::RealtimeTouchLayout> touchLaneLayout;
  std::map<SDL_FingerID, int> fingerToLane;
  std::map<SDL_FingerID, bool> fingerLanePressed;
  std::map<SDL_FingerID, FlickState> flickStates;
  std::map<SDL_FingerID, Uint32> cancelGraceExpiry;
  std::function<std::optional<bool>(int)> longNoteHeldCallback;
  std::vector<int> presses, releases;
  bms_parser::Note *applyTouchLane(int lane, bool pressed, std::optional<int>) {
    (pressed ? presses : releases).push_back(lane);
    return nullptr;
  }
  bool notifyTouchEvent(SDL_FingerID, ReplayTouchAction, Vector3) { return false; }
  Vector3 normalizedTouchToRenderLocation(Vector3) const;
  bool isLaneOccupied(int, SDL_FingerID) const;
  void beginFingerLane(SDL_FingerID, int, Vector3);
  void releaseFingerLane(SDL_FingerID);
  void handleScratchMove(SDL_FingerID, Vector3);
  void onFingerDown(SDL_FingerID, Vector3);
  void onFingerUp(SDL_FingerID, Vector3);
  void onFingerMove(SDL_FingerID, Vector3);
  int clampLane(int) const;
  bool isScratchLane(int) const;
  int touchToLaneIndex(Vector3) const;
  std::optional<int> touchToLaneIfInside(Vector3) const;
  int touchToLane(Vector3);
  void setTouchLaneLayout(std::optional<gameplay::RealtimeTouchLayout>);
  std::optional<int> authoredTouchLane(Vector3, bool) const;
};

// PRODUCTION_METHODS

int failures = 0;
void expect(bool pass, const char *message) {
  if (!pass) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}

gameplay::RealtimeTouchLayout skinLayout() {
  gameplay::RealtimeTouchLayout layout;
  layout.revision = 17;
  layout.laneRegions = {
      {.bottomLeft = {.20F, .8F}, .bottomRight = {.30F, .8F},
       .topLeft = {.20F, .2F}, .topRight = {.30F, .2F}, .lane = 3},
      {.bottomLeft = {.35F, .8F}, .bottomRight = {.60F, .8F},
       .topLeft = {.35F, .2F}, .topRight = {.60F, .2F}, .lane = 1},
      {.bottomLeft = {.70F, .8F}, .bottomRight = {.80F, .8F},
       .topLeft = {.70F, .2F}, .topRight = {.80F, .2F}, .lane = 7,
       .scratch = true}};
  return layout;
}

int main() {
  // SDL already removed the UI offset: (.25 * 1000 - 100) / 800.
  const Vector3 firstLane{.1875F, .5F, 0};
  const Vector3 secondLane{.4375F, .5F, 0};
  const Vector3 gap{.28125F, .5F, 0};
  RhythmInputHandler handler;
  handler.setTouchLaneLayout(skinLayout());
  handler.onFingerDown(1, firstLane);
  handler.onFingerUp(1, firstLane);
  expect(handler.presses == std::vector<int>{3} &&
             handler.releases == std::vector<int>{3},
         "SDL tap follows authored lane with safe-area offset");
  handler.presses.clear(); handler.releases.clear();
  handler.onFingerDown(2, gap);
  handler.onFingerUp(2, gap);
  expect(handler.presses.empty(), "skin lane gap never clamps to a built-in lane");
  handler.onFingerDown(3, {.1875F, 1.2F, 0});
  handler.onFingerUp(3, firstLane);
  expect(handler.presses == std::vector<int>{3},
         "non-drag tap below authored lane retains vertical clamping");
  handler.presses.clear(); handler.releases.clear();
  handler.dragModeEnabled = true;
  handler.onFingerDown(4, firstLane);
  handler.onFingerMove(4, secondLane);
  handler.onFingerMove(4, gap);
  handler.onFingerUp(4, gap);
  expect(handler.presses == std::vector<int>({3, 1}) &&
             handler.releases == std::vector<int>({3, 1}),
         "drag crosses unequal authored lanes and releases in their gap");
  handler.presses.clear(); handler.releases.clear();
  handler.dragModeEnabled = false;
  const Vector3 scratch{.8125F, .5F, 0};
  handler.onFingerDown(5, scratch);
  expect(handler.presses.empty(), "authored scratch waits for flick");
  handler.onFingerMove(5, {.8125F, .4F, 0});
  handler.onFingerUp(5, scratch);
  expect(handler.presses == std::vector<int>{7} &&
             handler.releases == std::vector<int>{7},
         "authored scratch flick and lift retain scratch ownership");
  handler.presses.clear(); handler.releases.clear();
  handler.setTouchLaneLayout(gameplay::RealtimeTouchLayout{});
  handler.onFingerDown(6, firstLane);
  expect(handler.presses.empty(), "unpublished skin geometry fails closed");
  handler.onFingerUp(6, firstLane);
  handler.presses.clear(); handler.releases.clear();
  handler.setTouchLaneLayout(std::nullopt);
  handler.onFingerDown(7, {.1875F, .5F, 0});
  handler.onFingerUp(7, firstLane);
  expect(handler.presses == std::vector<int>{1} &&
             handler.releases == std::vector<int>{1},
         "built-in camera routing remains unchanged without a skin layout");
  handler.presses.clear(); handler.releases.clear();
  handler.setTouchLaneLayout(skinLayout());
  handler.onFingerDown(8, firstLane);
  handler.setTouchLaneLayout(skinLayout());
  expect(handler.releases.empty(), "ordinary frame publication preserves held notes");
  auto replacement = skinLayout();
  ++replacement.revision;
  handler.setTouchLaneLayout(replacement);
  expect(handler.releases == std::vector<int>{3},
         "layout revision change releases the original held lane");
  handler.onFingerUp(8, firstLane);
  expect(handler.releases == std::vector<int>{3},
         "lift after layout invalidation cannot release twice");
  handler.presses.clear(); handler.releases.clear();
  rendering::ui_scale_x = 2.0F;
  rendering::ui_scale_y = .5F;
  handler.onFingerDown(9, {.09375F, 1.0F, 0});
  handler.onFingerUp(9, {.09375F, 1.0F, 0});
  expect(handler.presses == std::vector<int>{3},
         "independent UI scales map to the same authored drawable lane");
  handler.presses.clear(); handler.releases.clear();
  handler.dragModeEnabled = true;
  handler.onFingerDown(10, {.09375F, 3.0F, 0});
  handler.onFingerUp(10, {.09375F, 3.0F, 0});
  expect(handler.presses.empty(), "drag mode does not vertically clamp outside skin lanes");
  return failures ? 1 : 0;
}
