#pragma once

#include "../audio/GameplayBgaFrame.h"

namespace rendering {
// Only the fullscreen offscreen output is cached. Final brightness and the
// presentation serial deliberately do not participate in its identity.
struct FullscreenBgaOutputKey {
  std::uint64_t contentEpoch = 0;
  std::uint64_t targetGeneration = 0;
  GameplayBgaComposition composition = GameplayBgaComposition::Blank;
  std::array<std::optional<PreparedGameplayBgaSurface>, 3> surfaces;
  std::array<int, 8> viewport{};
  int displayMode = 0;

  bool operator==(const FullscreenBgaOutputKey &) const = default;
};

inline std::optional<FullscreenBgaOutputKey> fullscreenBgaOutputKey(
    const PreparedGameplayBgaFrame &frame, std::uint64_t targetGeneration,
    std::array<int, 8> viewport, int displayMode) noexcept {
  if (frame.contentEpoch == 0 || targetGeneration == 0) return std::nullopt;
  FullscreenBgaOutputKey key{frame.contentEpoch, targetGeneration,
      frame.composition, {frame.base, frame.layer, frame.miss}, viewport, displayMode};
  for (auto &surface : key.surfaces) {
    if (!surface) continue;
    if (surface->contentIdentity == 0) return std::nullopt;
    // Lease lookup tokens are not content (prepareImageFrame uses a sequence).
    surface->surfaceToken = 0;
  }
  return key;
}

class FullscreenBgaCache {
public:
  bool canReuse(const std::optional<FullscreenBgaOutputKey> &key) const noexcept {
    return key && completed_ && *key == *completed_;
  }
  void invalidate() noexcept { completed_.reset(); }
  // Call after bgfx::frame/IOSPreparedFrame accepted the complete output, with
  // Stats::numDrawCallsPeak read AFTER that call and Caps::limits.maxDrawCalls.
  // Unlike the delayed render-thread numDraw, this CPU-side lifetime peak is
  // updated by frame()'s encoderApiWait from submitted + dropped render items.
  // Once it exceeds capacity we cannot prove later frames accepted every draw:
  // conservatively recompose until bgfx reinitialization resets the peak.
  // A failed draw, interrupted frame or partial overwrite evicts the old key.
  void complete(const std::optional<FullscreenBgaOutputKey> &key,
                bool outputSubmitted, std::uint32_t requestedDrawPeak,
                std::uint32_t drawCapacity) noexcept {
    completed_ = outputSubmitted && drawCapacity != 0 && requestedDrawPeak <= drawCapacity
        ? key : std::nullopt;
  }

private:
  std::optional<FullscreenBgaOutputKey> completed_;
};
} // namespace rendering
