#pragma once
#include <SDL3/SDL.h>
#include <array>
#include <functional>

class View;
class ScrollView;
class TextView;
class Button;
class BlockingOverlayView;

class TasksModal final {
public:
  TasksModal(View *parent, std::function<void()> refreshLibrary);
  void show();
  BlockingOverlayView *root() const { return root_; }
  ScrollView *libraryScroll() const { return libraryScroll_; }
  TextView *libraryText() const { return libraryText_; }
  void setLibraryCount(int count);
  int notificationCount() const { return libraryCount_; }
  void hide();
  bool isVisible() const;
  void resize(int width, int height);
  bool handleEvents(SDL_Event &event);
private:
  BlockingOverlayView *root_ = nullptr;
  View *panel_ = nullptr;
  ScrollView *libraryScroll_ = nullptr;
  TextView *libraryText_ = nullptr;
  Button *refreshLibrary_ = nullptr;
  std::array<int, 6> lastLayout_{-1, -1, -1, -1, -1, -1};
  int libraryCount_ = 0;
};
