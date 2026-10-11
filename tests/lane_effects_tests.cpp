#include "../src/scene/play/LaneEffects.h"
#include "graph_allocation_guard.h"
#include "../src/scene/play/KeybombEffect.h"
#include <cmath>
#undef NDEBUG
#include <cassert>
#include <thread>

struct Geometry {
  int count = 0;
  int gradientTriangles = 0;
  float extent = 0;
  double motion = 0;
  unsigned firstColor = 0;
  unsigned maxAlpha = 0;
  void point(float x, float y, unsigned color) {
    if (firstColor == 0) firstColor = color;
    motion += x * x + y * y;
    assert(std::isfinite(x) && std::isfinite(y));
    extent = std::max(extent, std::max(std::abs(x - 10), std::abs(y - 20)));
    maxAlpha = std::max(maxAlpha, color >> 24);
  }
  void addCircle(float x, float y, float r, unsigned c) {
    ++count; point(x-r,y-r,c); point(x+r,y+r,c);
  }
  void addLine(float x, float y, float a, float b, float, unsigned c) {
    ++count; point(x,y,c); point(a,b,c);
  }
  void addTriangleColors(float x, float y, float a, float b, float d, float e,
                         unsigned center, unsigned edge1, unsigned edge2) {
    ++gradientTriangles;
    assert((edge1 >> 24) == 0 && (edge2 >> 24) == 0);
    const float r1 = std::hypot(a - x, b - y), r2 = std::hypot(d - x, e - y);
    assert(std::abs(r1 - r2) < 0.0001F);
    addTriangle(x, y, a, b, d, e, center);
  }
  void addTriangle(float x, float y, float a, float b, float d, float e, unsigned c) {
    ++count; point(x,y,c); point(a,b,c); point(d,e,c);
  }
};

void testGeometry() {
  const LaneKeybombEvent event{PGreat, 100000};
  Geometry normal, large, dim, hidden, future, expired;
  keybomb::draw(normal, event, 150000, 10, 20, 2, {});
  keybomb::draw(large, event, 150000, 10, 20, 2, {true, 200, 100});
  keybomb::draw(dim, event, 150000, 10, 20, 2, {true, 100, 25});
  keybomb::draw(hidden, event, 150000, 10, 20, 2, {false, 100, 100});
  keybomb::draw(future, event, 99999, 10, 20, 2, {});
  keybomb::draw(expired, event, 1000000, 10, 20, 2, {});
  assert(normal.gradientTriangles > 0); // Smooth disks even below 0.5 world units.
  assert(normal.count > 0 && std::abs(large.extent - normal.extent * 2) < 0.001F);
  assert(dim.maxAlpha < normal.maxAlpha && dim.count == normal.count);
  assert(hidden.count == 0 && future.count == 0 && expired.count == 0);
  Geometry zero;
  keybomb::draw(zero, event, 150000, 10, 20, 2, {true, 100, 0});
  assert(zero.count == 0);
  for (const auto judgement : {PGreat, Great, Good, Bad, Kpoor}) {
    Geometry burst;
    keybomb::draw(burst, {judgement, 0}, 80000, 10, 20, 2, {});
    assert(burst.count > 0 && burst.count < 200);
  }
}

void testPoorHasNoKeybomb() {
  for (const auto style : {keybomb::Style{}, keybomb::Style{true, 300, 100, 0xFFFFFF}}) {
    Geometry burst, hold;
    keybomb::draw(burst, {Poor, 0}, 80'000, 10, 20, 2, style);
    keybomb::drawHold(hold, Poor, 2'000'000, 10, 20, 2, style);
    assert(burst.count == 0 && hold.count == 0);
  }
}

void testDefaultHitsStayClearlyVisible() {
  for (const auto judgement : {PGreat, Great, Good}) {
    Geometry burst;
    keybomb::draw(burst, {judgement, 0}, 80'000, 10, 20, 2, {});
    assert(burst.maxAlpha >= 200); // Keep a strong core after the initial flash.
  }
}

void testHoldGeometry() {
  Geometry a, b, paused, large, dim, hidden, zero;
  const keybomb::Style pink{true, 100, 100, 0xE83FA0};
  keybomb::drawHold(a, PGreat, 2'000'000, 10, 20, 2, pink);
  keybomb::drawHold(b, PGreat, 2'080'000, 10, 20, 2, pink);
  keybomb::drawHold(paused, PGreat, 2'000'000, 10, 20, 2, pink);
  keybomb::drawHold(large, PGreat, 2'000'000, 10, 20, 2, {true, 200, 100});
  keybomb::drawHold(dim, PGreat, 2'000'000, 10, 20, 2, {true, 100, 25});
  keybomb::drawHold(hidden, PGreat, 2'000'000, 10, 20, 2, {false, 100, 100});
  keybomb::drawHold(zero, PGreat, 2'000'000, 10, 20, 2, {true, 100, 0});
  assert(a.count > 0 && a.count < 100 && a.count == b.count);
  assert(a.motion != b.motion && a.motion == paused.motion);
  assert((a.firstColor & 0xFFFFFFU) == 0xA03FE8);
  assert(std::abs(large.extent - a.extent * 2) < 0.001F);
  assert(dim.maxAlpha < a.maxAlpha && hidden.count == 0 && zero.count == 0);
  Geometry burst;
  keybomb::draw(burst, {PGreat, 0}, 80000, 10, 20, 2, pink);
  assert((burst.firstColor & 0xFFFFFFU) == 0xA03FE8);
}

int main() {
  testPoorHasNoKeybomb();
  testDefaultHitsStayClearlyVisible();
  testGeometry();
  testHoldGeometry();
  AtomicLaneState lane;
  {
    graph_test::AllocationGuard guard;
    graph_test::remainingAllocationBytes = 0;
    lane.publishKeybomb(PGreat, 0);
    assert(lane.keybomb().judgement == PGreat && lane.keybomb().timeMicros == 0);
    lane.publishKeybomb(None, 100);
    assert(lane.keybomb().judgement == PGreat && lane.keybomb().timeMicros == 0);
    lane.publishKeybomb(Poor, -100);
    assert(lane.keybomb().judgement == Poor && lane.keybomb().timeMicros == -100);
  }
  lane.publishKeybomb(PGreat, 100);
  std::thread writer([&] {
    for (int i = 0; i < 100000; ++i) {
      lane.publishKeybomb(Great, 200);
      lane.publishKeybomb(PGreat, 100);
    }
  });
  for (int i = 0; i < 100000; ++i) {
    const auto event = lane.keybomb();
    assert((event.judgement == PGreat && event.timeMicros == 100) ||
           (event.judgement == Great && event.timeMicros == 200));
  }
  writer.join();
  lane.clearKeybomb();
  assert(lane.keybomb().judgement == None);
}
