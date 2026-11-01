#pragma once
#include <SDL3/SDL.h>
#include <memory>
#include <string>
#include <cstdint>

class View;
class ScrollView;
class TextView;
namespace downloads { class DownloadCoordinator; }

class DownloadsModal final {
public:
  explicit DownloadsModal(View *parent);
  void show();
  void hide();
  bool isVisible() const;
  void update();
  void resize(int width, int height);
  bool handleEvents(SDL_Event &event);
private:
  void refresh();
  View *root_ = nullptr, *panel_ = nullptr;
  ScrollView *scroll_ = nullptr;
  TextView *error_ = nullptr;
  std::shared_ptr<downloads::DownloadCoordinator> owner_;
  std::uint64_t nextRefresh_ = 0;
  std::string snapshot_;
};
