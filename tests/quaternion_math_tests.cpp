#include "math/Quaternion.h"
#include "math/Vector3.h"

#include <array>
#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
void require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

void requireNear(float actual, float expected, std::string_view message) {
  constexpr float tolerance = 1.0e-5f;
  require(std::isfinite(actual) && std::fabs(actual - expected) <= tolerance,
          message);
}

void requireSameAngles(const Vector3 &actual, const Vector3 &expected) {
  requireNear(actual.x, expected.x, "roll matches the canonical conversion");
  requireNear(actual.y, expected.y, "pitch matches the canonical conversion");
  requireNear(actual.z, expected.z, "yaw matches the canonical conversion");
}

void testIdentityHasZeroEulerAngles() {
  const Vector3 angles = Quaternion{}.getEulerAngles();
  requireNear(angles.x, 0.0f, "identity roll is zero");
  requireNear(angles.y, 0.0f, "identity pitch is zero");
  requireNear(angles.z, 0.0f, "identity yaw is zero");
}

void testLegacyAccessorMatchesSafeConversion() {
  constexpr std::array<std::array<float, 3>, 4> rotations{{
      {20.0f, -30.0f, 45.0f},
      {-65.0f, 10.0f, 125.0f},
      {90.0f, 0.0f, -90.0f},
      {5.0f, 89.0f, 15.0f},
  }};
  for (const auto &rotation : rotations) {
    const Quaternion quaternion =
        Quaternion::fromEuler(rotation[0], rotation[1], rotation[2]);
    requireSameAngles(quaternion.getEulerAngles(), quaternion.toEuler());
  }
}

void testCompoundCrossProductUsesOriginalComponents() {
  Vector3 value{1.0f, 2.0f, 3.0f};
  Vector3 other{4.0f, 5.0f, 6.0f};
  const Vector3 expected = value.cross(other);

  value *= other;

  requireNear(value.x, expected.x, "compound cross-product x is stable");
  requireNear(value.y, expected.y, "compound cross-product y is stable");
  requireNear(value.z, expected.z, "compound cross-product z is stable");
}
} // namespace

int main() {
  testIdentityHasZeroEulerAngles();
  testLegacyAccessorMatchesSafeConversion();
  testCompoundCrossProductUsesOriginalComponents();
  return 0;
}
