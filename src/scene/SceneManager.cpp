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

void SceneManager::registerScene(const std::string& name, std::unique_ptr<Scene> scene) {
  registeredScenes[name] = std::move(scene);
}

void SceneManager::changeScene(Scene *newScene, bool keepBackground) {
  // Check if the new scene is already in backgroundScenes (O(1) lookup)
  auto it = backgroundScenes.find(newScene);
  
  // Handle current scene (common logic)
  if (currentScene && !keepBackground) {
    Scene *sceneToRelease = currentScene;
    cleanupSceneInstance(sceneToRelease);
  }
  if (keepBackground && currentScene) {
    backgroundScenes.insert(currentScene);
  }
  
  if (it != backgroundScenes.end()) {
    // Scene is in background, bring it to foreground
    currentScene = newScene;
    backgroundScenes.erase(it);
    currentScene->onResume();
    // Don't call init() again since the scene is already initialized
  } else {
    // Normal scene change for new or registered scenes
    currentScene = newScene;
    currentScene->prepareForUse();
    currentScene->init();
  }
}

void SceneManager::changeScene(const std::string& sceneName, bool keepBackground) {
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
  if (currentScene) {
    currentScene->render();
  }
}

void SceneManager::cleanup() {
  if (currentScene != nullptr) {
    cleanupSceneInstance(currentScene);
    currentScene = nullptr;
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
}
SceneManager::~SceneManager() { cleanup(); }
