#pragma once

#include "StorageFileTransaction.h"
#include "StorageDirectoryEvidence.h"
#include "StorageWorkScheduler.h"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace storage_management {
class StorageConversionPermit {
public:
  class Lease {
  public:
    Lease(Lease &&) noexcept;
    Lease &operator=(Lease &&) = delete;
    ~Lease();
  private:
    friend class StorageConversionPermit;
    explicit Lease(StorageConversionPermit *owner) : owner_(owner) {}
    StorageConversionPermit *owner_;
  };
  std::optional<Lease> Acquire(Checkpoint);
  void Notify();
private:
  std::mutex mutex_;
  std::condition_variable changed_;
  bool busy_ = false;
};
std::shared_ptr<StorageConversionPermit> SharedStorageConversionPermit();

struct OptimizerTransactions {
  std::function<FileResult(const AssetCandidate &, const EncodeResult &, Checkpoint)> replace;
  std::function<FileResult(const AssetCandidate &, Checkpoint)> erase;
  std::function<StorageReport(Checkpoint, const StorageRecoveryObserver &)> recover;
};
using StorageEncoder = std::function<EncodeResult(const AssetCandidate &, const StoragePolicy &,
    const std::filesystem::path &, Checkpoint, ProgressCallback)>;
struct OptimizerDependencies {
  StoragePlatformPaths paths;
  std::shared_ptr<StorageConversionPermit> permit;
  std::function<StorageCodecCapabilities()> probeCodecs;
  StorageEncoder encodeAudio, encodeVideo;
  std::function<OptimizerTransactions(const StoragePlatformPaths &, StorageCommitPrecondition)> transactions;
  std::function<std::optional<std::uint64_t>(const std::filesystem::path &)> availableSpace;
  std::function<std::chrono::steady_clock::time_point()> now;
  StorageEvidenceMetrics *evidenceMetrics = nullptr;
  StorageExecutionLimits executionLimits = DefaultStorageExecutionLimits();
};
OptimizerDependencies DefaultOptimizerDependencies(StoragePlatformPaths);

struct OptimizationRequest {
  InventoryRequest inventory;
  StoragePolicy policy;
  std::optional<StoragePreflight> preflight;
  std::vector<GroupStorageMeasurement> measurementBaselines;
};
class StorageOptimizer {
public:
  explicit StorageOptimizer(OptimizerDependencies dependencies) : dependencies_(std::move(dependencies)) {}
  StorageReport Run(OptimizationRequest, Checkpoint = {}, ProgressCallback = {});
private:
  OptimizerDependencies dependencies_;
};
} // namespace storage_management
