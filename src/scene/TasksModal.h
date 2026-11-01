#pragma once
#include <SDL3/SDL.h>
#include <memory>
#include <string>
#include <cstdint>
#include <array>
#include <filesystem>
#include <functional>

class View;
class ScrollView;
class TextView;
class Button;
class BlockingOverlayView;
namespace downloads { class DownloadCoordinator; }

class TasksModal final {
public:
  enum class Tab { Library, Downloads };
  TasksModal(View *parent, std::function<void()> refreshLibrary,
                 std::function<bool(const std::filesystem::path &)> locate);
  void show(Tab tab = Tab::Library);
  BlockingOverlayView *root() const { return root_; }
  ScrollView *libraryScroll() const { return libraryScroll_; }
  TextView *libraryText() const { return libraryText_; }
  void setLibraryCount(int count);
  int notificationCount() const { return libraryCount_ + downloadCount_; }
  void hide();
  bool isVisible() const;
  void update();
  void resize(int width, int height);
  bool handleEvents(SDL_Event &event);
private:
  void refresh();
  void selectTab(Tab tab);
  void refreshBadges();
  BlockingOverlayView *root_ = nullptr;
  View *panel_ = nullptr, *downloadsContent_ = nullptr;
  ScrollView *libraryScroll_ = nullptr;
  TextView *libraryText_ = nullptr;
  Button *libraryTab_ = nullptr, *downloadsTab_ = nullptr, *refreshLibrary_ = nullptr;
  TextView *libraryTabText_ = nullptr, *downloadsTabText_ = nullptr;
  TextView *libraryBadge_ = nullptr, *downloadsBadge_ = nullptr;
  std::array<int, 6> lastLayout_{-1, -1, -1, -1, -1, -1};
  int libraryCount_ = 0, downloadCount_ = 0;
  Tab selectedTab_ = Tab::Library;
  ScrollView *scroll_ = nullptr;
  TextView *error_ = nullptr;
  std::shared_ptr<downloads::DownloadCoordinator> owner_;
  std::function<bool(const std::filesystem::path &)> locate_;
  bool locateFailed_ = false;
  std::uint64_t nextRefresh_ = 0;
  std::string snapshot_;
};
