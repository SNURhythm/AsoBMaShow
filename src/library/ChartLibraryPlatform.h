#pragma once

#include "../repositories/ChartRepository.h"

#include <filesystem>
#include <memory>
#include <vector>

namespace chart_library_tasks {
class ChartLibraryTaskService;
}

namespace chart_library_platform {

void clearFolderAccess();
void refreshFolderAccess(const std::vector<ChartEntry> &entries);
std::filesystem::path resolveFolderEntryPath(const ChartEntry &entry);

class FolderActionService final {
public:
  FolderActionService(
      ChartRepository &, chart_library_tasks::ChartLibraryTaskService &);
  ~FolderActionService();

  FolderActionService(const FolderActionService &) = delete;
  FolderActionService &operator=(const FolderActionService &) = delete;

  void requestAddFolder();
  void requestImportArchive();
  void poll();
  [[nodiscard]] bool active() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace chart_library_platform
