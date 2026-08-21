#include "SkinObjectResolutionPrecedence.h"

#include "../LuaGameplaySkinFeature.h"

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS

#include <array>

namespace skin {
namespace {

using Kind = SkinObjectResolutionKind;

constexpr std::array kReturningGenericOrder{
    Kind::Image,
    Kind::ImageSet,
    Kind::Value,
    Kind::FloatValue,
    Kind::Text,
    Kind::Slider,
    Kind::Graph,
    Kind::GaugeGraph,
};

constexpr std::array kPostJudgeGraphGenericOrder{
    Kind::BpmGraph,
    Kind::HitErrorVisualizer,
    Kind::TimingVisualizer,
    Kind::TimingDistributionGraph,
    Kind::Gauge,
};

constexpr std::array kGameplaySpecialOrder{
    Kind::Note,
    Kind::HiddenCover,
    Kind::LiftCover,
    Kind::Practice,
    Kind::Bga,
    Kind::Judge,
    Kind::PmChara,
};

const SkinObjectResolutionCandidate *
firstMatch(std::span<const SkinObjectResolutionCandidate> candidates,
           Kind kind) noexcept {
  const SkinObjectResolutionCandidate *first = nullptr;
  for (const auto &candidate : candidates) {
    if (!candidate.matches || candidate.kind != kind) {
      continue;
    }
    if (first == nullptr || candidate.authoredIndex < first->authoredIndex) {
      first = &candidate;
    }
  }
  return first;
}

SkinObjectResolutionResult
resultFor(const SkinObjectResolutionCandidate &candidate) {
  const bool unsupported = [&] {
    switch (candidate.kind) {
    case Kind::Image:
    case Kind::ImageSet:
    case Kind::Value:
    case Kind::FloatValue:
    case Kind::Text:
    case Kind::Slider:
    case Kind::Graph:
    case Kind::GaugeGraph:
    case Kind::JudgeGraph:
    case Kind::BpmGraph:
    case Kind::HitErrorVisualizer:
    case Kind::TimingVisualizer:
    case Kind::TimingDistributionGraph:
    case Kind::Gauge:
    case Kind::Note:
    case Kind::HiddenCover:
    case Kind::LiftCover:
    case Kind::Bga:
    case Kind::Judge:
    case Kind::PmChara:
      return false;
    case Kind::Practice:
      return true;
    }
    return true;
  }();
  return {.status = unsupported ? SkinObjectResolutionStatus::Unsupported
                                : SkinObjectResolutionStatus::Found,
          .kind = candidate.kind,
          .authoredIndex = candidate.authoredIndex};
}

} // namespace

SkinObjectResolutionResult resolveSkinObjectPrecedence(
    std::span<const SkinObjectResolutionCandidate> candidates) {
  if (candidates.size() > SkinObjectResolutionPolicy::maxCandidates) {
    return {.status = SkinObjectResolutionStatus::CandidateLimitExceeded,
            .kind = std::nullopt,
            .authoredIndex = std::nullopt};
  }

  for (const Kind kind : kReturningGenericOrder) {
    if (const auto *candidate = firstMatch(candidates, kind)) {
      return resultFor(*candidate);
    }
  }

  // JsonSkinObjectLoader assigns JudgeGraph and breaks only its local loop.
  // Any later generic match returns immediately and replaces it; otherwise
  // the provisional JudgeGraph remains the parent's result.
  const auto *judgeGraph = firstMatch(candidates, Kind::JudgeGraph);
  for (const Kind kind : kPostJudgeGraphGenericOrder) {
    if (const auto *candidate = firstMatch(candidates, kind)) {
      return resultFor(*candidate);
    }
  }
  if (judgeGraph != nullptr) {
    return resultFor(*judgeGraph);
  }

  for (const Kind kind : kGameplaySpecialOrder) {
    if (const auto *candidate = firstMatch(candidates, kind)) {
      return resultFor(*candidate);
    }
  }

  return {};
}

} // namespace skin

#endif
