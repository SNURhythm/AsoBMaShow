#include "../src/rendering/PortraitPlayfieldFraming.h"
#include "../src/settings/PresentationGeometryPolicy.h"
#include <cassert>
#include <cmath>
#include <iostream>

int main() {
  using namespace rendering;
  constexpr double pi = 3.141592653589793;
  const auto policy = player_settings::presentationGeometryPolicy(player_settings::PresentationOrientation::Portrait);
  for (float aspect : {9.0F/16.0F, 3.0F/4.0F, 9.0F/21.0F})
  for (float length : {policy.length.minimum, policy.length.defaultValue, policy.length.maximum})
  for (float width : {policy.width.minimum, policy.width.defaultValue, policy.width.maximum})
  for (float angle : {0.0F, 28.0F})
  for (NormalizedSafeArea safe : {NormalizedSafeArea{}, NormalizedSafeArea{.top=.055F, .right=.02F, .bottom=.035F, .left=.01F}}) {
    auto frame = framePortraitPlayfield(length, width, angle, aspect, safe);
    assert(std::isfinite(frame.cameraDepth) && frame.cameraDepth > .1F);
    assert(std::isfinite(frame.lookAtY));
    const double a = angle * pi / 180, sa = std::sin(a), ca = std::cos(a);
    const double eyeY = frame.lookAtY - std::tan(a) * frame.cameraDepth;
    const double eyeZ = -frame.cameraDepth;
    const double tangent = std::tan(60 * pi / 180);
    // Independent camera basis projection, including the complete lower lane
    // area, judgement line, and top. All corners must remain in the safe rect.
    for (double y : {-1.0, 0.0, double(length)})
    for (double x : {-width / 2.0, width / 2.0}) {
      const double depth = (y - eyeY) * sa - eyeZ * ca;
      const double cameraY = (y - eyeY) * ca + eyeZ * sa;
      const double nx = x / (depth * tangent * aspect);
      const double ny = cameraY / (depth * tangent);
      assert(depth > .1 && depth < 100);
      assert(nx >= -1 + 2*safe.left - 1e-5 && nx <= 1 - 2*safe.right + 1e-5);
      assert(ny >= -1 + 2*safe.bottom - 1e-5 && ny <= 1 - 2*safe.top + 1e-5);
    }
    for (int lane = 0; lane < 8; ++lane) {
      const double x = width * ((lane + .5)/8 - .5), y = 0;
      const double depth = (y-eyeY)*sa-eyeZ*ca;
      const double nx = x/(depth*tangent*aspect);
      const double ny = ((y-eyeY)*ca+eyeZ*sa)/(depth*tangent);
      // Unproject a screen ray, intersect z=0, and recover the input lane.
      const double rayX = nx*tangent*aspect;
      const double rayY = sa + ny*tangent*ca;
      const double rayZ = ca - ny*tangent*sa;
      const double rayT = -eyeZ/rayZ;
      const double recoveredX = rayT*rayX;
      const double recoveredY = eyeY + rayT*rayY;
      assert(std::abs(recoveredY-y) < 1e-4);
      assert(int(std::floor((recoveredX/width+.5)*8)) == lane);
    }
  }
  std::cout << "portrait framing and lane touch tests passed\n";
}
