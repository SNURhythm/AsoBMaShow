#pragma once

#include <functional>
#include <string>
#include <utility>

class Scene;
class SceneManager;

struct SceneReturnTarget {
  enum class Kind { Registered, Retained };

  Kind kind = Kind::Registered;
  std::string registeredName = "MainMenu";
  Scene *retained = nullptr;
  std::function<void()> settingsWillOpen;

  static SceneReturnTarget Registered(std::string name) {
    return {.kind = Kind::Registered,
            .registeredName = std::move(name),
            .retained = nullptr,
            .settingsWillOpen = {}};
  }

  static SceneReturnTarget Retained(
      Scene *scene, std::function<void()> settingsWillOpen = {}) {
    return {.kind = Kind::Retained,
            .registeredName = {},
            .retained = scene,
            .settingsWillOpen = std::move(settingsWillOpen)};
  }

  void notifySettingsWillOpen() const {
    if (kind == Kind::Retained && settingsWillOpen) settingsWillOpen();
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
