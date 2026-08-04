#pragma once

#include "../package/SkinPackageTypes.h"

#include <compare>
#include <cstdint>
#include <optional>
#include <string>

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

} // namespace skin
