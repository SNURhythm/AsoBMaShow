#include "DownloadService.h"
#include "DownloadPlatform.h"
#include "DownloadImport.h"
#include <exception>
#include <atomic>

namespace downloads {
std::shared_ptr<DownloadCoordinator> service() {
  static std::once_flag once;
  static std::shared_ptr<DownloadCoordinator> owner;
  static std::exception_ptr failure;
  std::call_once(once, [] {
    try {
      const auto root = platform::privateRoot();
      if (root.empty()) throw std::runtime_error("Private download storage is unavailable.");
      CoordinatorHooks hooks;
      hooks.startTransfer = platform::startTransfer;
      hooks.cancelTransfer = platform::cancelTransfer;
      hooks.finishActivity = platform::finishActivity;
      hooks.beginImport = platform::beginImport;
      hooks.reportProgress = platform::reportProgress;
      hooks.cleanup = cleanupArchiveJob;
      hooks.importArchive = [root](const auto &job, const auto &archive, auto &cancelled, auto progress, auto prepare) {
        return importArchiveJob(job, archive, root / job.id, cancelled, std::move(progress), std::move(prepare));
      };
      hooks.resolveArtifact = [root](const auto &job, auto decision, auto &cancelled, auto prepare) {
        return resolveArchiveJob(job, root / job.id, decision, cancelled, std::move(prepare));
      };
      owner = std::make_shared<DownloadCoordinator>(root, std::move(hooks));
    } catch (...) { failure = std::current_exception(); }
  });
  if (failure) std::rethrow_exception(failure);
  return owner;
}
namespace {
std::mutex foregroundMutex;
bool gameForeground = false;
std::mutex presentationMutex;
std::optional<std::string> requestedPresentation;
}
void setGameForeground(bool foreground) {
  std::lock_guard lock(foregroundMutex);
  gameForeground = foreground;
  service()->setForeground(gameForeground);
}
void requestDownloadsPresentation(const std::string &id) {
  std::lock_guard lock(presentationMutex);
  requestedPresentation = id;
}
std::optional<std::string> takeDownloadsPresentation() {
  std::lock_guard lock(presentationMutex);
  return std::exchange(requestedPresentation, {});
}
} // namespace downloads
