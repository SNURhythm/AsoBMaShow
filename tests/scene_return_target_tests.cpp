#include "scene/SceneReturnTarget.h"

#include <iostream>
#include <string>

namespace {
int failures = 0;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void testRegisteredTargetHasNoFallback() {
  const auto target = SceneReturnTarget::Registered("Intro");
  expect(target.kind == SceneReturnTarget::Kind::Registered &&
             target.registeredName == "Intro" && target.retained == nullptr,
         "registered target retains only its exact scene name");
  expect(resolveSceneReturnTarget(target, true, false) ==
             SceneReturnResolution::Registered,
         "available registered target resolves registered");
  expect(resolveSceneReturnTarget(target, false, true) ==
             SceneReturnResolution::Unavailable,
         "missing registered target does not fall back to a retained scene");
}

void testRetainedTargetHasNoFallback() {
  auto *identity = reinterpret_cast<Scene *>(0x1234);
  const auto target = SceneReturnTarget::Retained(identity);
  expect(target.kind == SceneReturnTarget::Kind::Retained &&
             target.registeredName.empty() && target.retained == identity,
         "retained target preserves the exact paused scene identity");
  expect(resolveSceneReturnTarget(target, false, true) ==
             SceneReturnResolution::Retained,
         "available retained target resolves retained");
  expect(resolveSceneReturnTarget(target, true, false) ==
             SceneReturnResolution::Unavailable,
         "missing retained target does not fall back to a registered scene");
  expect(resolveSceneReturnTarget(SceneReturnTarget::Retained(nullptr), true,
                                  true) ==
             SceneReturnResolution::Unavailable,
         "null retained identity is unavailable");
}
} // namespace

int main() {
  testRegisteredTargetHasNoFallback();
  testRetainedTargetHasNoFallback();
  if (failures != 0) {
    std::cerr << failures << " scene return target test(s) failed\n";
    return 1;
  }
  std::cout << "scene return target tests passed\n";
  return 0;
}
