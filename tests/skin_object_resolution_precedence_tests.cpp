#include "skin/beatoraja/SkinObjectResolutionPrecedence.h"

#include <array>
#include <cstddef>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

using namespace skin;

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

SkinObjectResolutionCandidate candidate(SkinObjectResolutionKind kind,
                                        std::size_t authoredIndex) {
  return {.kind = kind, .authoredIndex = authoredIndex, .matches = true};
}

void expectFound(const SkinObjectResolutionResult &result,
                 SkinObjectResolutionKind kind, std::size_t authoredIndex,
                 std::string_view message) {
  expect(result.status == SkinObjectResolutionStatus::Found &&
             result.kind == kind && result.authoredIndex == authoredIndex,
         message);
}

SkinObjectResolutionStatus expectedStatus(SkinObjectResolutionKind kind) {
  switch (kind) {
  case SkinObjectResolutionKind::Image:
  case SkinObjectResolutionKind::ImageSet:
  case SkinObjectResolutionKind::Value:
  case SkinObjectResolutionKind::FloatValue:
  case SkinObjectResolutionKind::Text:
  case SkinObjectResolutionKind::Slider:
  case SkinObjectResolutionKind::Graph:
  case SkinObjectResolutionKind::JudgeGraph:
  case SkinObjectResolutionKind::HitErrorVisualizer:
  case SkinObjectResolutionKind::TimingVisualizer:
  case SkinObjectResolutionKind::Gauge:
  case SkinObjectResolutionKind::Note:
  case SkinObjectResolutionKind::HiddenCover:
  case SkinObjectResolutionKind::LiftCover:
  case SkinObjectResolutionKind::Bga:
  case SkinObjectResolutionKind::Judge:
    return SkinObjectResolutionStatus::Found;
  default:
    return SkinObjectResolutionStatus::Unsupported;
  }
}

void expectWinner(const SkinObjectResolutionResult &result,
                  SkinObjectResolutionKind kind, std::size_t authoredIndex,
                  std::string_view message) {
  expect(result.status == expectedStatus(kind) && result.kind == kind &&
             result.authoredIndex == authoredIndex,
         message);
}

SkinObjectResolutionKind expectedGenericWinner(
    SkinObjectResolutionKind earlier, SkinObjectResolutionKind later) {
  // JsonSkinObjectLoader only breaks (rather than returns) for JudgeGraph.
  // A later generic branch therefore replaces its provisional object.
  return earlier == SkinObjectResolutionKind::JudgeGraph ? later : earlier;
}

void testEveryGenericPairUsesPinnedPriority() {
  constexpr std::array genericOrder{
      SkinObjectResolutionKind::Image,
      SkinObjectResolutionKind::ImageSet,
      SkinObjectResolutionKind::Value,
      SkinObjectResolutionKind::FloatValue,
      SkinObjectResolutionKind::Text,
      SkinObjectResolutionKind::Slider,
      SkinObjectResolutionKind::Graph,
      SkinObjectResolutionKind::GaugeGraph,
      SkinObjectResolutionKind::JudgeGraph,
      SkinObjectResolutionKind::BpmGraph,
      SkinObjectResolutionKind::HitErrorVisualizer,
      SkinObjectResolutionKind::TimingVisualizer,
      SkinObjectResolutionKind::TimingDistributionGraph,
      SkinObjectResolutionKind::Gauge,
  };

  for (std::size_t earlier = 0; earlier < genericOrder.size(); ++earlier) {
    for (std::size_t later = earlier + 1; later < genericOrder.size();
         ++later) {
      const std::array candidates{
          candidate(genericOrder[later], 17),
          candidate(genericOrder[earlier], 3),
      };
      const auto result = resolveSkinObjectPrecedence(candidates);
      const auto expected = expectedGenericWinner(genericOrder[earlier],
                                                  genericOrder[later]);
      const auto expectedIndex = expected == genericOrder[earlier] ? 3 : 17;
      expectWinner(result, expected, expectedIndex,
                   "each generic pair preserves the pinned winner and status");
    }
  }
}

void testFirstAuthoredMatchWinsWithinPinnedLoops() {
  constexpr std::array loopedGenerics{
      SkinObjectResolutionKind::Image,
      SkinObjectResolutionKind::ImageSet,
      SkinObjectResolutionKind::Value,
      SkinObjectResolutionKind::FloatValue,
      SkinObjectResolutionKind::Text,
      SkinObjectResolutionKind::Slider,
      SkinObjectResolutionKind::Graph,
      SkinObjectResolutionKind::GaugeGraph,
      SkinObjectResolutionKind::JudgeGraph,
      SkinObjectResolutionKind::BpmGraph,
      SkinObjectResolutionKind::HitErrorVisualizer,
      SkinObjectResolutionKind::TimingVisualizer,
      SkinObjectResolutionKind::TimingDistributionGraph,
  };
  for (const auto kind : loopedGenerics) {
    const std::array candidates{candidate(kind, 4), candidate(kind, 9)};
    const auto result = resolveSkinObjectPrecedence(candidates);
    expectWinner(result, kind, 4,
                 "a generic loop selects its first authored matching definition");
  }

  constexpr std::array loopedSpecials{
      SkinObjectResolutionKind::HiddenCover,
      SkinObjectResolutionKind::LiftCover,
      SkinObjectResolutionKind::Judge,
  };
  for (const auto kind : loopedSpecials) {
    const std::array candidates{candidate(kind, 2), candidate(kind, 6)};
    const auto result = resolveSkinObjectPrecedence(candidates);
    expectFound(result, kind, 2,
                "a gameplay special loop selects its first authored match");
  }
}

void testEveryGenericPreemptsEveryGameplaySpecial() {
  constexpr std::array generics{
      SkinObjectResolutionKind::Image,
      SkinObjectResolutionKind::ImageSet,
      SkinObjectResolutionKind::Value,
      SkinObjectResolutionKind::FloatValue,
      SkinObjectResolutionKind::Text,
      SkinObjectResolutionKind::Slider,
      SkinObjectResolutionKind::Graph,
      SkinObjectResolutionKind::GaugeGraph,
      SkinObjectResolutionKind::JudgeGraph,
      SkinObjectResolutionKind::BpmGraph,
      SkinObjectResolutionKind::HitErrorVisualizer,
      SkinObjectResolutionKind::TimingVisualizer,
      SkinObjectResolutionKind::TimingDistributionGraph,
      SkinObjectResolutionKind::Gauge,
  };
  constexpr std::array specials{
      SkinObjectResolutionKind::Note,
      SkinObjectResolutionKind::HiddenCover,
      SkinObjectResolutionKind::LiftCover,
      SkinObjectResolutionKind::Practice,
      SkinObjectResolutionKind::Bga,
      SkinObjectResolutionKind::Judge,
      SkinObjectResolutionKind::PmChara,
  };

  for (const auto generic : generics) {
    for (const auto special : specials) {
      const std::array candidates{candidate(special, 8), candidate(generic, 5)};
      const auto result = resolveSkinObjectPrecedence(candidates);
      expectWinner(result, generic, 5,
                   "a generic definition preempts every gameplay-only definition");
    }
  }
}

void testGameplaySpecialChainUsesPinnedOrder() {
  const std::array candidates{
      candidate(SkinObjectResolutionKind::Note, 1),
      candidate(SkinObjectResolutionKind::HiddenCover, 2),
      candidate(SkinObjectResolutionKind::LiftCover, 3),
      candidate(SkinObjectResolutionKind::Bga, 4),
      candidate(SkinObjectResolutionKind::Judge, 5),
  };
  expectFound(resolveSkinObjectPrecedence(candidates),
              SkinObjectResolutionKind::Note, 1,
              "Note wins the Note-to-Judge gameplay precedence chain");

  for (std::size_t first = 1; first < candidates.size(); ++first) {
    std::vector<SkinObjectResolutionCandidate> suffix(candidates.begin() +
                                                           first,
                                                       candidates.end());
    const auto expected = candidates[first];
    expectFound(resolveSkinObjectPrecedence(suffix), expected.kind,
                expected.authoredIndex,
                "removing each earlier gameplay special exposes the next one");
  }
}

void testUnsupportedAndNotFoundStayDistinct() {
  const std::array unsupported{candidate(SkinObjectResolutionKind::Practice, 7)};
  const auto unsupportedResult = resolveSkinObjectPrecedence(unsupported);
  expect(unsupportedResult.status == SkinObjectResolutionStatus::Unsupported &&
             unsupportedResult.kind == SkinObjectResolutionKind::Practice &&
             unsupportedResult.authoredIndex == 7,
         "an unsupported pinned definition is distinct from no match");

  const std::array<SkinObjectResolutionCandidate, 1> unmatched{{
      {.kind = SkinObjectResolutionKind::Image, .authoredIndex = 1, .matches = false},
  }};
  const auto notFound = resolveSkinObjectPrecedence(unmatched);
  expect(notFound.status == SkinObjectResolutionStatus::NotFound &&
             !notFound.kind.has_value() && !notFound.authoredIndex.has_value(),
         "no matching candidate reports NotFound without a winner");
}

void testUnsupportedGenericStillPreemptsLaterSupportedSpecial() {
  const std::array candidates{
      candidate(SkinObjectResolutionKind::Note, 4),
      candidate(SkinObjectResolutionKind::GaugeGraph, 2),
  };
  const auto result = resolveSkinObjectPrecedence(candidates);
  expect(result.status == SkinObjectResolutionStatus::Unsupported &&
             result.kind == SkinObjectResolutionKind::GaugeGraph &&
             result.authoredIndex == 2,
         "an unsupported generic wins before a later supported gameplay special");
}

void testCandidateLimitFailsBeforeResolution() {
  std::vector<SkinObjectResolutionCandidate> candidates(
      SkinObjectResolutionPolicy::maxCandidates + 1,
      {.kind = SkinObjectResolutionKind::Image,
       .authoredIndex = 0,
       .matches = true});
  const auto result = resolveSkinObjectPrecedence(candidates);
  expect(result.status == SkinObjectResolutionStatus::CandidateLimitExceeded &&
             !result.kind.has_value() && !result.authoredIndex.has_value(),
         "one candidate above the bounded lookup limit fails before selecting");
}

} // namespace

int main() {
  testEveryGenericPairUsesPinnedPriority();
  testFirstAuthoredMatchWinsWithinPinnedLoops();
  testEveryGenericPreemptsEveryGameplaySpecial();
  testGameplaySpecialChainUsesPinnedOrder();
  testUnsupportedAndNotFoundStayDistinct();
  testUnsupportedGenericStillPreemptsLaterSupportedSpecial();
  testCandidateLimitFailsBeforeResolution();
  return failures == 0 ? 0 : 1;
}
