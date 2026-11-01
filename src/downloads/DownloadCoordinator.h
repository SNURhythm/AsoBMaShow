#pragma once

#include "DownloadJobs.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <set>
#include <thread>

namespace downloads {
using BeforePublication = std::function<bool(const BmsSearchPendingArtifact &)>;
struct CoordinatorHooks {
  std::function<std::string(const Job &, const std::filesystem::path &)> startTransfer;
  std::function<void(const Job &)> cancelTransfer;
  std::function<void(const Job &, bool)> finishActivity;
  std::function<void(const Job &)> reportProgress;
  std::function<BmsSearchResult(const Job &, const std::filesystem::path &,
      std::atomic_bool &, BmsSearchDownloadProgressCallback, BeforePublication)> importArchive;
  std::function<BmsSearchResult(const Job &, BmsSearchPendingArtifactDecision,
      std::atomic_bool &, BeforePublication)> resolveArtifact;
};

// Native delegates and UI hold an application-owned service. UI detachment has
// no lifecycle effect. All callbacks use durable identity plus transfer attempt.
class DownloadCoordinator final {
public:
  DownloadCoordinator(std::filesystem::path root, CoordinatorHooks hooks);
  ~DownloadCoordinator();
  Job submit(Request request);
  std::vector<Job> jobs() const;
  std::optional<Job> get(const std::string &id) const;
  std::string error() const;
  std::filesystem::path archivePath(const std::string &id) const;
  std::filesystem::path jobPath(const std::string &id) const;
  void setForeground(bool foreground);
  void enableImports(bool enabled);
  void setContinuedProcessing(const std::string &id, bool permitted);
  bool cancel(const std::string &id);
  bool retry(const std::string &id);
  bool decide(const std::string &id, BmsSearchPendingArtifactDecision decision);
  bool forget(const std::string &id);
  bool isCurrentTransfer(const std::string &id, std::uint64_t attempt) const;
  void transferProgress(const std::string &id, std::uint64_t attempt,
                        std::uint64_t bytes, std::uint64_t total);
  bool acceptFile(const std::string &id, std::uint64_t attempt,
                  const std::filesystem::path &completedFile);
  void transferFailed(const std::string &id, std::uint64_t attempt,
                      const std::string &message);
  std::vector<Job> readyForLibrary() const;
  bool acknowledgeLibrary(const std::string &id);
private:
  void pumpTransfers();
  void importLoop();
  std::optional<Job> importableLocked() const;
  bool preparePublication(const std::string &id, const BmsSearchPendingArtifact &artifact);
  void finishActivity(const Job &job, bool success) noexcept;
  JobStore store_;
  CoordinatorHooks hooks_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  bool foreground_ = false, importsEnabled_ = false, stopping_ = false;
  std::set<std::string> continued_;
  std::string activeImport_;
  std::string persistenceError_;
  std::atomic_bool interrupted_{false};
  std::jthread worker_;
};
} // namespace downloads
