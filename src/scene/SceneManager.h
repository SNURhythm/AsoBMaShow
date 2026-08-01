#pragma once

#include <SDL2/SDL.h>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <string>
class Scene;
class EventHandleResult;
class ApplicationContext;
class SceneManager {
private:
  ApplicationContext &context;
  bool isRegisteredScene(const Scene *scene) const;
  void cleanupSceneInstance(Scene *scene);
  void updateBackgroundTaskPauseState();

public:
  Scene* currentScene = nullptr;
  std::unordered_set<Scene*> backgroundScenes;
  std::unordered_map<std::string, std::unique_ptr<Scene>> registeredScenes;
  
  SceneManager() = delete;
  ~SceneManager();
  explicit SceneManager(ApplicationContext &context);
  SceneManager(const SceneManager &) = delete;
  SceneManager &operator=(const SceneManager &) = delete;
  SceneManager(SceneManager &&) = delete;
  SceneManager &operator=(SceneManager &&) = delete;
  
  // Scene registration
  void registerScene(const std::string& name, std::unique_ptr<Scene> scene);
  
  // Scene changing
  void changeScene(std::unique_ptr<Scene> newScene,
                   bool keepBackground = false);
  void changeScene(Scene *newScene, bool keepBackground = false);
  void changeScene(const std::string& sceneName, bool keepBackground = false);
  
  EventHandleResult handleEvents(SDL_Event &event);
  void cleanup();
  void update(float dt);
  void handleDeferred();
  void render();
};
