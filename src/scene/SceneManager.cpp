#include "SceneManager.h"
#include "Scene.h"
SceneManager::SceneManager(ApplicationContext &context) : context(context) {
  context.sceneManager = this;
}

bool SceneManager::isRegisteredScene(const Scene *scene) const {
  if (scene == nullptr) {
    return false;
  }
  for (const auto &[name, registeredScene] : registeredScenes) {
    (void)name;
    if (registeredScene.get() == scene) {
      return true;
    }
  }
  return false;
}

void SceneManager::cleanupSceneInstance(Scene *scene) {
  if (scene == nullptr) {
    return;
  }
  scene->cleanup();
  if (!isRegisteredScene(scene)) {
    delete scene;
  }
}

void SceneManager::updateBackgroundTaskPauseState() {
  const bool shouldPause =
      currentScene != nullptr &&
      currentScene->pausesBackgroundTasksForPerformance();
  const bool changed =
      context.backgroundTasksPausedForForegroundScene.exchange(shouldPause) !=
      shouldPause;
  if (context.chartLibraryTasks) {
    context.chartLibraryTasks->setGameplayPaused(shouldPause);
  }
  if (changed && context.notifyBackgroundTaskPauseStateChanged) {
    context.notifyBackgroundTaskPauseStateChanged();
  }
}

void SceneManager::registerScene(const std::string& name, std::unique_ptr<Scene> scene) {
  registeredScenes[name] = std::move(scene);
}

bool SceneManager::hasRegisteredScene(const std::string &name) const {
  return registeredScenes.contains(name);
}

bool SceneManager::hasBackgroundScene(const Scene *scene) const {
  return scene != nullptr && backgroundScenes.contains(const_cast<Scene *>(scene));
}

void SceneManager::changeScene(std::unique_ptr<Scene> newScene,
                               bool keepBackground) {
  if (newScene == nullptr) {
    return;
  }

  Scene *newScenePtr = newScene.get();
  if (currentScene && !keepBackground) {
    Scene *sceneToRelease = currentScene;
    currentScene = nullptr;
    cleanupSceneInstance(sceneToRelease);
  }
  if (keepBackground && currentScene && currentScene != newScenePtr) {
    currentScene->onPause();
    backgroundScenes.insert(currentScene);
  }

  currentScene = newScenePtr;
  updateBackgroundTaskPauseState();
  try {
    currentScene->prepareForUse();
    currentScene->init();
  } catch (...) {
    if (currentScene == newScenePtr) {
      currentScene = nullptr;
      updateBackgroundTaskPauseState();
    }
    throw;
  }
  newScene.release();
}

void SceneManager::changeScene(Scene *newScene, bool keepBackground) {
  // Check if the new scene is already in backgroundScenes (O(1) lookup)
  auto it = backgroundScenes.find(newScene);
  
  // Handle current scene (common logic)
  if (currentScene && !keepBackground) {
    Scene *sceneToRelease = currentScene;
    cleanupSceneInstance(sceneToRelease);
  }
  if (keepBackground && currentScene && currentScene != newScene) {
    currentScene->onPause();
    backgroundScenes.insert(currentScene);
  }
  
  if (it != backgroundScenes.end()) {
    // Scene is in background, bring it to foreground
    currentScene = newScene;
    backgroundScenes.erase(it);
    updateBackgroundTaskPauseState();
    pendingRegisteredSceneChange_.reset();
    resumingScene_ = true;
    try {
      currentScene->onResume();
    } catch (...) {
      resumingScene_ = false;
      pendingRegisteredSceneChange_.reset();
      throw;
    }
    resumingScene_ = false;
    if (pendingRegisteredSceneChange_) {
      auto pending = std::exchange(pendingRegisteredSceneChange_, std::nullopt);
      changeScene(pending->first, pending->second);
    }
    // Don't call init() again since the scene is already initialized
  } else {
    // Normal scene change for new or registered scenes
    currentScene = newScene;
    updateBackgroundTaskPauseState();
    currentScene->prepareForUse();
    currentScene->init();
  }
}

void SceneManager::changeScene(const std::string& sceneName, bool keepBackground) {
  if (resumingScene_) {
    pendingRegisteredSceneChange_ = {sceneName, keepBackground};
    return;
  }
  auto it = registeredScenes.find(sceneName);
  if (it != registeredScenes.end()) {
    // Use the existing changeScene method with the scene pointer
    Scene* scenePtr = it->second.get();
    changeScene(scenePtr, keepBackground);
  }
  // Note: Could add error handling here if scene name not found
}

EventHandleResult SceneManager::handleEvents(SDL_Event &event) {
  EventHandleResult result;
  View::dispatchTemporaryEventListeners(event);
  if (currentScene) {
    result = currentScene->handleEvents(event);
  }
  View::dispatchDeferredEventCallbacks();
  return result;
}

void SceneManager::update(float dt) {
  if (currentScene) {
    currentScene->update(dt);
  }
}

void SceneManager::handleDeferred() {
  if (currentScene) {
    currentScene->handleDeferred();
  }
}

void SceneManager::render() {
  context.gameplayBgaCompositeState = {};
  if (currentScene) {
    currentScene->render();
  }
}

void SceneManager::cleanup() {
  if (currentScene != nullptr) {
    cleanupSceneInstance(currentScene);
    currentScene = nullptr;
    updateBackgroundTaskPauseState();
  }

  for (auto *scene : backgroundScenes) {
    if (!isRegisteredScene(scene)) {
      cleanupSceneInstance(scene);
    }
  }
  backgroundScenes.clear();

  for (auto &[name, scene] : registeredScenes) {
    (void)name;
    scene->cleanup();
  }
  registeredScenes.clear();
  updateBackgroundTaskPauseState();
}
SceneManager::~SceneManager() { cleanup(); }
