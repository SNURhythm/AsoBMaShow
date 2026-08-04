#pragma once

#include "SkinProfileSettings.h"
#include "package/SkinActivationCommitStore.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

namespace skin {

// The only package-to-gameplay handoff. The owning activation and its revision
// lease move into one chart-lifetime request; gameplay never prepares or
// commits package state itself.
struct GameplaySkinActivationRequest {
  std::uint64_t sessionSerial = 0;
  SkinProfileId profileId;
  ValidatedSkinActivation activation;
  ViewportSettings viewport;
};

using AcquireGameplaySkinForNextChart =
    std::function<std::optional<GameplaySkinActivationRequest>()>;

// One noexcept boundary around the destructive handoff from a consumed
// next-attempt request to an installed chart-lifetime session. In particular,
// allocation by a texture/session factory must not escape gameplay reset and
// replace the already-warmed built-in presentation.
template <typename Acquire, typename Construct, typename Install,
          typename OnException>
[[nodiscard]] bool runGameplaySkinAttemptInstallFailClosed(
    Acquire &&acquire, Construct &&construct, Install &&install,
    OnException &&onException) noexcept {
  try {
    auto request = std::invoke(std::forward<Acquire>(acquire));
    if (!request) {
      return false;
    }
    auto session = std::invoke(std::forward<Construct>(construct),
                               std::move(*request));
    if (!session) {
      return false;
    }
    std::invoke(std::forward<Install>(install), std::move(session));
    return true;
  } catch (...) {
    try {
      std::invoke(std::forward<OnException>(onException));
    } catch (...) {
    }
    return false;
  }
}

} // namespace skin
