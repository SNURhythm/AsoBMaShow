#pragma once
#include <atomic>
#include <thread>
#include <iostream>
#include <vector>
#include <string>
#include "AppSettings.h"
#include "game/GameState.h"
#include "scene/SceneManager.h"
#include "audio/Jukebox.h"
class ApplicationContext {

public:
  std::atomic<bool> quitFlag;
  SceneManager *sceneManager = nullptr;
  Uint64 currentFrame = 0;
  AppSettings settings;
  Jukebox jukebox;

  // string: annotation, thread: thread
  std::vector<std::pair<std::string, std::thread>> threads;

  ApplicationContext() : quitFlag(false), settings(AppSettings::load()),
                         jukebox(&gameStopwatch) {
    settings.sanitize();
    jukebox.setVisualsEnabled(settings.bgaEnabled);
  }
  ~ApplicationContext() {
    quitFlag = true;
    std::cout << "Waiting for threads to join..." << std::endl;
    for (auto &thread : threads) {
      if (thread.second.joinable()) {
        std::cout << "Joining thread: " << thread.first << std::endl;
        thread.second.join();
      }
    }
    std::cout << "Main function is quitting..." << std::endl;
  }
};
