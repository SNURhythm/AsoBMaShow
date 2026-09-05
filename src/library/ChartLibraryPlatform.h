#pragma once

#include "../repositories/ChartRepository.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace chart_library_tasks {
class ChartLibraryTaskService;
}

namespace chart_library_platform {

void clearFolderAccess();
void refreshFolderAccess(const std::vector<ChartEntry> &entries);
std::filesystem::path resolveFolderEntryPath(const ChartEntry &entry);

// Result of a platform folder pick for an app-owned settings folder. `path`
// is a resolved absolute filesystem path (on Android it may be a SAF tree
// root), and `bookmark` carries the access token needed to re-access the
// folder after relaunch: an iOS security-scoped bookmark or an Android SAF
// tree URI.
struct SoundSetFolderPick {
  std::string path;
  std::string bookmark;
  bool succeed = false;
};

// Off-main-thread folder picker for the music-select sound-set folder. iOS
// requires the native picker to run off the main thread, so request() spawns a
// fresh std::jthread (guarded by an active flag) and the result is consumed by
// a later poll on the main thread — the same pattern as FolderActionService's
// picker thread. Desktop has no gate-less picker here and is a no-op.
class SoundSetFolderPicker final {
public:
  SoundSetFolderPicker();
  ~SoundSetFolderPicker();
  SoundSetFolderPicker(const SoundSetFolderPicker &) = delete;
  SoundSetFolderPicker &operator=(const SoundSetFolderPicker &) = delete;
  SoundSetFolderPicker(SoundSetFolderPicker &&) = delete;
  SoundSetFolderPicker &operator=(SoundSetFolderPicker &&) = delete;

  // Launches the platform folder picker off the main thread. Ignored while a
  // pick is active or a result awaits consume.
  void request();

  // True while the native picker is displayed or a result awaits consume().
  [[nodiscard]] bool active() const noexcept;

  // Consumes a finished pick; returns std::nullopt while active or when no
  // result is pending.
  [[nodiscard]] std::optional<SoundSetFolderPick> consume() noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

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
