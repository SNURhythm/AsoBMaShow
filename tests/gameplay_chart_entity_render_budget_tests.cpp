#include "scene/play/GameplayChartEntityRenderBudget.h"

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
} // namespace

int main() {
  using namespace gameplay_chart_entity_render_budget;

  require(kMaxRectanglesPerFrame == 60'000U,
          "gameplay chart rendering keeps the approved frame cap");
  require(kSingleRectangleEntityCost == 1U,
          "a simple chart entity costs one rectangle");
  require(kLongNoteReservationCost == 3U,
          "a long note reserves body, head, and tail");
  require(kReplayGhostOutlineCost == 4U,
          "a replay ghost reserves its complete outline");
  require(kReplayMissMarkerCost == 14U,
          "a replay miss marker reserves its complete X");

  Budget budget;
  require(budget.remaining() == kMaxRectanglesPerFrame,
          "a new frame starts with the complete budget");
  require(!budget.exhausted(), "a new frame is not exhausted");
  require(budget.tryConsume(kReplayGhostOutlineCost),
          "a complete atomic shape fits within the budget");
  require(budget.remaining() ==
              kMaxRectanglesPerFrame - kReplayGhostOutlineCost,
          "an accepted request consumes its complete cost");

  Budget exactBoundary;
  require(exactBoundary.tryConsume(kMaxRectanglesPerFrame),
          "an exact-boundary request is accepted");
  require(exactBoundary.remaining() == 0U,
          "an exact-boundary request consumes all capacity");
  require(exactBoundary.exhausted(),
          "zero remaining capacity reports exhausted");
  require(!exactBoundary.tryConsume(kSingleRectangleEntityCost),
          "no request is accepted after exact exhaustion");

  Budget rejected;
  require(rejected.tryConsume(kMaxRectanglesPerFrame - 2U),
          "setup leaves less room than a long-note reservation");
  require(!rejected.tryConsume(kLongNoteReservationCost),
          "an over-budget atomic request is rejected");
  require(rejected.remaining() == 2U,
          "a rejected request does not partially consume capacity");
  require(rejected.exhausted(),
          "the first rejected request latches exhaustion");
  require(!rejected.tryConsume(kSingleRectangleEntityCost),
          "later smaller shapes stay rejected after the latch");
  require(rejected.remaining() == 2U,
          "latched rejections leave the remainder unchanged");

  rejected.reset();
  require(rejected.remaining() == kMaxRectanglesPerFrame,
          "the next frame restores the complete budget");
  require(!rejected.exhausted(),
          "reset clears the exhaustion latch");
  require(rejected.tryConsume(kLongNoteReservationCost),
          "atomic requests work again after reset");

  return 0;
}
