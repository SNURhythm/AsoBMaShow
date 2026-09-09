#pragma once

#include "ArchiveUnzipOperation.h"

#include <SDL2/SDL.h>
#include <functional>
#include <memory>

class Button;
class TextView;
class View;

struct ArchiveUnzipModalCallbacks {
  std::function<void()> libraryChanged;
  std::function<void(const ArchiveUnzipResult &)> finished;
};

class ArchiveUnzipModal final {
public:
  static std::unique_ptr<ArchiveUnzipModal>
  Create(View *parent, ChartRepository &repository,
         ArchiveUnzipModalCallbacks callbacks);
  ~ArchiveUnzipModal();
  ArchiveUnzipModal(const ArchiveUnzipModal &) = delete;
  ArchiveUnzipModal &operator=(const ArchiveUnzipModal &) = delete;

  bool start(const ChartMetaRecord &record);
  bool inProgress() const;
  bool isVisible() const;
  View *root() const;
  void update();
  void resize(int width, int height);
  void cancelAndWait();
  void hide();
  bool handleEvents(SDL_Event &event);

private:
  explicit ArchiveUnzipModal(ChartRepository &repository,
                             ArchiveUnzipModalCallbacks callbacks);
  void build(View *parent);
  void cancelOrClose();
  void deleteArchive();
  void setDeleteVisible(bool visible);
  void updateProgress(double fraction, const std::string &message,
                      std::uint64_t current = 0, std::uint64_t total = 0);

  ArchiveUnzipOperation operation_;
  ArchiveUnzipModalCallbacks callbacks_;
  View *root_ = nullptr;
  View *track_ = nullptr;
  View *fill_ = nullptr;
  TextView *title_ = nullptr;
  TextView *message_ = nullptr;
  TextView *percent_ = nullptr;
  TextView *detail_ = nullptr;
  Button *deleteButton_ = nullptr;
  Button *cancelButton_ = nullptr;
  TextView *cancelText_ = nullptr;
  std::uint64_t estimatedSize_ = 0;
  bool cancelling_ = false;
  bool libraryChangedPending_ = false;
};
