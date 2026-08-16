#pragma once

#include "../SkinStoragePaths.h"
#include "../SkinSafetyPolicy.h"
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
  CopiedFileCreate,
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

  // The Files-visible package is the source of truth on platforms that opt
  // into live sources. This is intentionally metadata-only: startup must not
  // walk or copy every package merely to restore the catalog.
  static std::optional<SkinRevisionLease> fromLiveSource(SkinRevision,
                                                         std::filesystem::path);

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
  void relocateLiveSourceTo(std::filesystem::path destination) noexcept;

private:
  PreparedSkinRevision(SkinRevision, std::filesystem::path,
                       std::filesystem::path,
                       std::shared_ptr<const SkinSnapshotFailureInjector>);
  struct State;
  std::unique_ptr<State> state_;
  friend class SkinTreeSnapshotter;
  friend class PreparedPackage;
};

struct SnapshotTreeResult {
  std::optional<PreparedSkinRevision> prepared;
  bool cancelled = false;
  std::vector<SkinDiagnostic> diagnostics;
};

enum class SkinSnapshotSourceRootPin : std::uint8_t {
  None,
  RetainedByCaller,
};

class SkinTreeSnapshotter {
public:
  SkinTreeSnapshotter(
      SkinStorageRoots, const SkinAliasDetector &,
      std::shared_ptr<const SkinSnapshotFailureInjector> failures = {},
      SkinSafetyPolicy safetyPolicy = SkinSafetyPolicy{});
  SnapshotTreeResult snapshot(const std::filesystem::path &sourceRoot,
                              const SkinPackageId &, std::stop_token,
                              SkinProgressCallback,
                              SkinSnapshotSourceRootPin =
                                  SkinSnapshotSourceRootPin::None);

private:
  SkinStorageRoots roots_;
  const SkinAliasDetector &aliases_;
  std::shared_ptr<const SkinSnapshotFailureInjector> failures_;
  SkinSafetyPolicy safetyPolicy_;
};

} // namespace skin
