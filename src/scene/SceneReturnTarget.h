#pragma once

#include <string>
#include <utility>

class Scene;
class SceneManager;

struct SceneReturnTarget {
  enum class Kind { Registered, Retained };

  Kind kind = Kind::Registered;
  std::string registeredName = "MainMenu";
  Scene *retained = nullptr;

  static SceneReturnTarget Registered(std::string name) {
    return {.kind = Kind::Registered,
            .registeredName = std::move(name),
            .retained = nullptr};
  }

  static SceneReturnTarget Retained(Scene *scene) {
    return {.kind = Kind::Retained, .registeredName = {}, .retained = scene};
  }
};

enum class SceneReturnResolution { Unavailable, Registered, Retained };

[[nodiscard]] inline SceneReturnResolution
resolveSceneReturnTarget(const SceneReturnTarget &target,
                         bool registeredAvailable, bool retainedAvailable) {
  if (target.kind == SceneReturnTarget::Kind::Registered) {
    return registeredAvailable ? SceneReturnResolution::Registered
                               : SceneReturnResolution::Unavailable;
  }
  return target.retained != nullptr && retainedAvailable
             ? SceneReturnResolution::Retained
             : SceneReturnResolution::Unavailable;
}

[[nodiscard]] bool returnToScene(SceneManager &manager,
                                 const SceneReturnTarget &target);
