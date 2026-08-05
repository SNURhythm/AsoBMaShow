#pragma once

#include "../SkinStoragePaths.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace skin {

struct SkinAcceptanceActivationKey;

struct SkinOverlayDigestTicket {
  std::uint64_t value = 0;

  explicit operator bool() const noexcept { return value != 0; }
  auto operator<=>(const SkinOverlayDigestTicket &) const = default;
};

enum class SkinOverlayDigestPollState : std::uint8_t {
  Unknown,
  Pending,
  Ready,
};

struct SkinOverlayDigestPollResult {
  SkinOverlayDigestPollState state = SkinOverlayDigestPollState::Unknown;
  std::string lowercaseSha256;
  std::optional<SkinDiagnostic> failure;
};

// Process-global, monotonic and never reused. Exhaustion fails closed with a
// zero ticket rather than wrapping. Provider implementations use this helper
// when accepting bounded worker work.
[[nodiscard]] SkinOverlayDigestTicket nextSkinOverlayDigestTicket() noexcept;

struct SkinOverlayDigestLimits {
  static constexpr std::uint64_t defaultMaximumBytes =
      16ULL * 1024ULL * 1024ULL;
  static constexpr std::uint64_t defaultMaximumFiles = 1'024;

  std::uint64_t maximumFiles = defaultMaximumFiles;
  std::uint64_t maximumFileBytes = defaultMaximumBytes;
  std::uint64_t maximumTotalBytes = defaultMaximumBytes;
};

class IAsyncSkinOverlayDigestProvider {
public:
  virtual ~IAsyncSkinOverlayDigestProvider() = default;

  // Queues a bounded no-follow digest of the private overlay selected by the
  // typed activation. Implementations derive their own capability/path; the
  // caller never passes a host path.
  virtual SkinOverlayDigestTicket
  beginDigest(const SkinAcceptanceActivationKey &) = 0;

  // Polling is memory-only. It must not hash, traverse, or open files.
  virtual SkinOverlayDigestPollResult
      pollDigest(SkinOverlayDigestTicket) const noexcept = 0;
  virtual void cancelDigest(SkinOverlayDigestTicket) noexcept = 0;

  // Provider-owned shutdown cancels, drains, and joins its worker and is
  // idempotent. The application owns the provider longer than the recorder.
  virtual void shutdown() noexcept = 0;
};

// A single-worker, bounded, no-follow implementation used by the application.
// It owns the typed storage roots by value and retains only in-memory polling
// state. Destruction performs the same cancel-and-join operation as shutdown().
class SkinOverlayDigestProvider final : public IAsyncSkinOverlayDigestProvider {
public:
  explicit SkinOverlayDigestProvider(
      SkinStorageRoots,
      SkinOverlayDigestLimits limits = SkinOverlayDigestLimits{});
  ~SkinOverlayDigestProvider() override;

  SkinOverlayDigestProvider(const SkinOverlayDigestProvider &) = delete;
  SkinOverlayDigestProvider &
  operator=(const SkinOverlayDigestProvider &) = delete;
  SkinOverlayDigestProvider(SkinOverlayDigestProvider &&) = delete;
  SkinOverlayDigestProvider &operator=(SkinOverlayDigestProvider &&) = delete;

  SkinOverlayDigestTicket
  beginDigest(const SkinAcceptanceActivationKey &) override;
  SkinOverlayDigestPollResult
      pollDigest(SkinOverlayDigestTicket) const noexcept override;
  void cancelDigest(SkinOverlayDigestTicket) noexcept override;
  void shutdown() noexcept override;

#if defined(ASOBMASHOW_SKIN_OVERLAY_DIGEST_PROVIDER_TESTING)
  using StabilityHookForTesting = void (*)(void *);
  struct InventoryNodeForTesting {
    std::string virtualPath;
    bool directory = false;
  };

  void setStabilityHookForTesting(StabilityHookForTesting, void *) noexcept;
  void setShutdownHookForTesting(StabilityHookForTesting, void *) noexcept;
  [[nodiscard]] std::size_t queuedJobCountForTesting() const noexcept;
  void failNextQueueCommitForTesting() noexcept;
  void failNextComputationAndFailureResultForTesting() noexcept;

  // Exercises the same collision namespace used by the descriptor-relative
  // inventory walker without depending on a host filesystem's case rules.
  [[nodiscard]] static bool inventoryRejectsNodesForTesting(
      std::span<const InventoryNodeForTesting>) noexcept;
#endif

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace skin
