#pragma once

#include "AppSettings.h"
#include "AppSettingsStore.h"
#include "PlayerProfileManager.h"
#include "skin/package/SkinPackageTypes.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

class ProfileSettingsPersistenceCoordinator;

namespace skin {

class ISkinProfileSnapshotProvider;

struct VersionedSkinProfileSettings {
  SkinProfileId profileId;
  std::uint64_t generation = 0;
  SkinProfileSettings settings;
  bool operator==(const VersionedSkinProfileSettings &) const = default;
};

struct SkinProfileCommitResult {
  enum class Status : std::uint8_t {
    Pending,
    Persisted,
    RetryableFailure,
    GenerationChanged
  } status = Status::Pending;
  std::uint64_t ticket = 0;
  bool generationChanged = false;
  std::optional<VersionedSkinProfileSettings> snapshot;
  std::optional<SkinDiagnostic> failure;
};

struct ProfileInventorySnapshot {
  std::uint64_t inventoryGeneration = 0;
  std::vector<VersionedSkinProfileSettings> profiles;
};

struct AllSkinProfileSnapshotsResult {
  bool complete = false;
  bool cancelled = false;
  std::optional<ProfileInventorySnapshot> inventory;
  std::vector<SkinDiagnostic> diagnostics;
};

class ProfileInventoryCommitFence {
public:
  ProfileInventoryCommitFence(ProfileInventoryCommitFence &&other) noexcept
      : release_(std::move(other.release_)) {
    other.release_ = {};
  }
  ProfileInventoryCommitFence &
  operator=(ProfileInventoryCommitFence &&other) noexcept {
    if (this != &other) {
      releaseNoThrow();
      release_ = std::move(other.release_);
      other.release_ = {};
    }
    return *this;
  }
  ProfileInventoryCommitFence(const ProfileInventoryCommitFence &) = delete;
  ProfileInventoryCommitFence &
  operator=(const ProfileInventoryCommitFence &) = delete;
  ~ProfileInventoryCommitFence() { releaseNoThrow(); }

private:
  friend class ::ProfileSettingsPersistenceCoordinator;
  friend class ISkinProfileSnapshotProvider;
  explicit ProfileInventoryCommitFence(std::function<void()> release)
      : release_(std::move(release)) {}
  void releaseNoThrow() noexcept {
    if (!release_) {
      return;
    }
    auto release = std::move(release_);
    try {
      release();
    } catch (...) {
    }
  }
  std::function<void()> release_;
};

class ProfileInventoryMutationBarrier {
public:
  ProfileInventoryMutationBarrier(ProfileInventoryMutationBarrier &&) noexcept;
  ProfileInventoryMutationBarrier &
  operator=(ProfileInventoryMutationBarrier &&) noexcept;
  ProfileInventoryMutationBarrier(const ProfileInventoryMutationBarrier &) =
      delete;
  ProfileInventoryMutationBarrier &
  operator=(const ProfileInventoryMutationBarrier &) = delete;
  ~ProfileInventoryMutationBarrier();

private:
  friend class ::ProfileSettingsPersistenceCoordinator;
  explicit ProfileInventoryMutationBarrier(std::function<void()> release);
  void release() noexcept;
  std::function<void()> release_;
};

class ISkinProfileSnapshotProvider {
public:
  virtual ~ISkinProfileSnapshotProvider() = default;
  virtual std::uint64_t beginSnapshotAllProfiles() = 0;
  virtual std::optional<AllSkinProfileSnapshotsResult>
  pollSnapshotAllProfiles(std::uint64_t ticket) = 0;
  virtual void cancelSnapshotAllProfiles(std::uint64_t ticket) noexcept = 0;
  virtual std::optional<ProfileInventoryCommitFence>
  tryAcquireInventoryCommitFence(const ProfileInventorySnapshot &) = 0;
  virtual ProfileInventoryMutationBarrier beginInventoryMutation() = 0;
  virtual void
  finishInventoryMutation(ProfileInventoryMutationBarrier &&) noexcept = 0;

protected:
  static ProfileInventoryCommitFence
  makeInventoryCommitFence(std::function<void()> release) {
    return ProfileInventoryCommitFence(std::move(release));
  }
};

class ISkinProfileSettingsOwner {
public:
  virtual ~ISkinProfileSettingsOwner() = default;
  virtual VersionedSkinProfileSettings
  snapshot(const SkinProfileId &) const = 0;
  virtual SkinProfileCommitResult
  beginCommit(const SkinProfileId &, std::uint64_t expectedGeneration,
              SkinProfileSettings candidate) = 0;
  virtual SkinProfileCommitResult pollCommit(std::uint64_t ticket) = 0;
  virtual void acknowledgeCommit(std::uint64_t ticket) noexcept = 0;
};

} // namespace skin

struct ProfileSettingsPersistenceDependencies {
  std::function<bool(const std::filesystem::path &, const AppSettings &,
                     std::string &)>
      saveAtomic = AppSettingsStore::Save;
  std::function<AppSettingsLoadResult(const std::filesystem::path &)>
      loadSettings = AppSettingsStore::Load;
  std::function<void()> afterFullSaveAdmitted;
  std::function<void()> afterSnapshotInputsCaptured;
};

class ProfileSettingsPersistenceCoordinator final
    : public skin::ISkinProfileSettingsOwner,
      public skin::ISkinProfileSnapshotProvider {
public:
  ProfileSettingsPersistenceCoordinator(
      PlayerProfileManager &, AppSettings &activeSettings,
      ProfileSettingsPersistenceDependencies = {});
  ~ProfileSettingsPersistenceCoordinator();

  skin::VersionedSkinProfileSettings
  snapshot(const skin::SkinProfileId &) const override;
  skin::SkinProfileCommitResult
  beginCommit(const skin::SkinProfileId &, std::uint64_t expectedGeneration,
              skin::SkinProfileSettings candidate) override;
  skin::SkinProfileCommitResult pollCommit(std::uint64_t ticket) override;
  void acknowledgeCommit(std::uint64_t ticket) noexcept override;
  std::uint64_t beginSnapshotAllProfiles() override;
  std::optional<skin::AllSkinProfileSnapshotsResult>
  pollSnapshotAllProfiles(std::uint64_t ticket) override;
  void cancelSnapshotAllProfiles(std::uint64_t ticket) noexcept override;
  std::optional<skin::ProfileInventoryCommitFence>
  tryAcquireInventoryCommitFence(
      const skin::ProfileInventorySnapshot &) override;
  skin::ProfileInventoryMutationBarrier beginInventoryMutation() override;
  void finishInventoryMutation(
      skin::ProfileInventoryMutationBarrier &&) noexcept override;
  bool saveActiveSettingsAndWait(const skin::SkinProfileId &, AppSettings &,
                                 std::string &error);
  bool flushProfileAndWait(const skin::SkinProfileId &, std::string &error);
  void bindCommittedActiveProfile(skin::SkinProfileId, AppSettings &);
  void shutdown() noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
