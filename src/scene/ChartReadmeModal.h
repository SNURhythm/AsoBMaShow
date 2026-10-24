#pragma once
#include <SDL3/SDL_events.h>
#include <filesystem>
#include <memory>
#include <string>
class View;
// Parent owns views. Destroy this controller before destroying the parent.
class ChartReadmeModal {
public:
  static std::unique_ptr<ChartReadmeModal> Create(View *parent);
  ~ChartReadmeModal();
  void show(const std::filesystem::path &chart, const std::string &title);
  void hide();
  bool isVisible() const;
  bool handleEvents(SDL_Event &event);
  void update();
  void resize(int width, int height);
private:
  struct Impl;
  explicit ChartReadmeModal(View *parent);
  std::unique_ptr<Impl> impl_;
};
