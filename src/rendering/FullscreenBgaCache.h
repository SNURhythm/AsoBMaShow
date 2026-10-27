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
  // Capture current-frame draw acceptance before frame(), then publish only
  // after the presentation attempt succeeds. A later accepted frame can recover
  // even when bgfx's lifetime draw-demand peak records an earlier overload.
  void complete(const std::optional<FullscreenBgaOutputKey> &key,
                bool outputSubmitted, bool drawsAccepted) noexcept {
    completed_ = outputSubmitted && drawsAccepted ? key : std::nullopt;
  }

private:
  std::optional<FullscreenBgaOutputKey> completed_;
};
} // namespace rendering
