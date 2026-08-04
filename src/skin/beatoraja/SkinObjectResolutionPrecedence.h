#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace skin {

// The order mirrors JsonSkinObjectLoader.loadSkinObject and
// JsonPlaySkinObjectLoader.loadSkinObject at Beatoraja c2ed5db1. Candidates
// are source-neutral: a caller supplies only whether an authored definition
// matches the destination and its index within that definition's array.
enum class SkinObjectResolutionKind : std::uint8_t {
  Image,
  ImageSet,
  Value,
  FloatValue,
  Text,
  Slider,
  Graph,
  GaugeGraph,
  JudgeGraph,
  BpmGraph,
  HitErrorVisualizer,
  TimingVisualizer,
  TimingDistributionGraph,
  Gauge,
  Note,
  HiddenCover,
  LiftCover,
  Practice,
  Bga,
  Judge,
  PmChara,
};

struct SkinObjectResolutionCandidate {
  SkinObjectResolutionKind kind = SkinObjectResolutionKind::Image;
  std::size_t authoredIndex = 0;
  bool matches = false;
};

struct SkinObjectResolutionPolicy {
  static constexpr std::size_t maxCandidates = 4'096;
};

enum class SkinObjectResolutionStatus : std::uint8_t {
  Found,
  NotFound,
  Unsupported,
  CandidateLimitExceeded,
};

struct SkinObjectResolutionResult {
  SkinObjectResolutionStatus status = SkinObjectResolutionStatus::NotFound;
  std::optional<SkinObjectResolutionKind> kind;
  std::optional<std::size_t> authoredIndex;
};

// Duplicate definitions are intentional inputs: the selected candidate is
// the first authored matching entry for its pinned loop, never an ambiguity.
[[nodiscard]] SkinObjectResolutionResult resolveSkinObjectPrecedence(
    std::span<const SkinObjectResolutionCandidate> candidates);

} // namespace skin
