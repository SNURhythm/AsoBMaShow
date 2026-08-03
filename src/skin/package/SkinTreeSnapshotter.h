#pragma once

#include "../SkinStoragePaths.h"
#include "SkinAliasDetector.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace skin {

class SkinRevisionLease;
struct SkinRevisionPin;

class SkinRevisionWeakPin {
public:
  bool hasLiveLease() const noexcept;

private:
  explicit SkinRevisionWeakPin(std::weak_ptr<const SkinRevisionPin>);
  std::weak_ptr<const SkinRevisionPin> pin_;
  friend class SkinRevisionLease;
};

enum class SkinSnapshotIoOperation : std::uint8_t {
  CopiedFileFsync,
  PreparedParentFsync,
  PublicationRename,
  PublishedParentFsync,
};

// Narrow fault seam for deterministic durability-boundary tests. Production
// leaves this unset and performs every operation normally.
class SkinSnapshotFailureInjector {
public:
  virtual ~SkinSnapshotFailureInjector() = default;
  virtual bool shouldFail(SkinSnapshotIoOperation,
                          const std::filesystem::path &) const noexcept = 0;
};

class SkinRevisionReadView {
public:
  const SkinRevision &revision() const noexcept;
  const std::filesystem::path &root() const noexcept;

private:
  SkinRevisionReadView(const SkinRevision *, const std::filesystem::path *);
  const SkinRevision *revision_ = nullptr;
  const std::filesystem::path *root_ = nullptr;
  friend class SkinRevisionLease;
  friend class PreparedSkinRevision;
};

class SkinRevisionLease {
public:
  SkinRevisionLease(SkinRevisionLease &&) noexcept = default;
  SkinRevisionLease &operator=(SkinRevisionLease &&) noexcept = default;
  SkinRevisionLease(const SkinRevisionLease &) = delete;
  SkinRevisionLease &operator=(const SkinRevisionLease &) = delete;

  const SkinRevision &revision() const noexcept;
  const std::filesystem::path &root() const noexcept;
  SkinRevisionReadView readView() const noexcept;
  SkinRevisionLease clone() const;
  SkinRevisionWeakPin weakPin() const noexcept;

private:
  explicit SkinRevisionLease(std::shared_ptr<const SkinRevisionPin>);
  std::shared_ptr<const SkinRevisionPin> pin_;
  friend class PreparedSkinRevision;
};

class PreparedSkinRevision {
public:
  PreparedSkinRevision(PreparedSkinRevision &&) noexcept;
  PreparedSkinRevision &operator=(PreparedSkinRevision &&) noexcept;
  PreparedSkinRevision(const PreparedSkinRevision &) = delete;
  PreparedSkinRevision &operator=(const PreparedSkinRevision &) = delete;
  ~PreparedSkinRevision();

  const SkinRevision &revision() const noexcept;
  const std::filesystem::path &stagingRoot() const noexcept;
  SkinRevisionReadView readView() const noexcept;
  std::optional<SkinRevisionLease> publish(std::string &error) &&;

private:
  PreparedSkinRevision(SkinRevision, std::filesystem::path,
                       std::filesystem::path,
                       std::shared_ptr<const SkinSnapshotFailureInjector>);
  struct State;
  std::unique_ptr<State> state_;
  friend class SkinTreeSnapshotter;
};

struct SnapshotTreeResult {
  std::optional<PreparedSkinRevision> prepared;
  bool cancelled = false;
  std::vector<SkinDiagnostic> diagnostics;
};

class SkinTreeSnapshotter {
public:
  SkinTreeSnapshotter(
      SkinStorageRoots, const SkinAliasDetector &,
      std::shared_ptr<const SkinSnapshotFailureInjector> failures = {});
  SnapshotTreeResult snapshot(const std::filesystem::path &sourceRoot,
                              const SkinPackageId &, std::stop_token,
                              SkinProgressCallback);

private:
  SkinStorageRoots roots_;
  const SkinAliasDetector &aliases_;
  std::shared_ptr<const SkinSnapshotFailureInjector> failures_;
};

} // namespace skin
