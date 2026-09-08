#include "SceneReturnTarget.h"

#include "SceneManager.h"

bool returnToScene(SceneManager &manager, const SceneReturnTarget &target) {
  const auto resolution = resolveSceneReturnTarget(
      target, manager.hasRegisteredScene(target.registeredName),
      manager.hasBackgroundScene(target.retained));
  switch (resolution) {
  case SceneReturnResolution::Registered:
    manager.changeScene(target.registeredName, false);
    return true;
  case SceneReturnResolution::Retained:
    manager.changeScene(target.retained, false);
    return true;
  case SceneReturnResolution::Unavailable:
    return false;
  }
  return false;
}
