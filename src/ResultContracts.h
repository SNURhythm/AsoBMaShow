#pragma once

#include "CanonicalDigest.h"
#include "DurablePayloadLimits.h"
#include "scene/play/GameplayGaugeTypes.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>

namespace result_contract {

inline constexpr std::array<int, 7> kSupportedKeyModes{5, 7, 9, 10, 14, 24, 48};

[[nodiscard]] inline constexpr bool isSupportedKeyMode(int keyMode) noexcept {
  for (const int supported : kSupportedKeyModes) {
    if (keyMode == supported) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] inline constexpr bool isKnownGaugeType(GaugeType value) noexcept {
  const int index = static_cast<int>(value);
  return index >= 0 && index < static_cast<int>(kGaugeTypeCount);
}

[[nodiscard]] inline constexpr bool
isKnownGaugeProfile(GaugeProfile value) noexcept {
  const int index = static_cast<int>(value);
  return index >= static_cast<int>(GaugeProfile::Standard) &&
         index <= static_cast<int>(GaugeProfile::Standard24Keys);
}

[[nodiscard]] inline constexpr bool
isKnownGaugeAutoShift(GaugeAutoShiftMode value) noexcept {
  const int index = static_cast<int>(value);
  return index >= static_cast<int>(GaugeAutoShiftMode::None) &&
         index <= static_cast<int>(GaugeAutoShiftMode::BestClear);
}

[[nodiscard]] inline constexpr bool isKnownLongNoteMode(int value) noexcept {
  return value >= 0 && value <= 3;
}

[[nodiscard]] inline constexpr bool isKnownClearRank(int value) noexcept {
  constexpr std::array ranks{
      kClearTypeFailedRank,
      kClearTypeAssistedEasyClearRank,
      kClearTypeLightAssistedEasyClearRank,
      kClearTypeEasyClearRank,
      kClearTypeNormalClearRank,
      kClearTypeHardClearRank,
      kClearTypeExHardClearRank,
      kClearTypeFullComboRank,
  };
  for (const int rank : ranks) {
    if (value == rank) {
      return true;
    }
  }
  return false;
}

struct ChartIdentity {
  std::string md5;
  std::string sha256;
  int keyMode = 0;

  bool operator==(const ChartIdentity &) const = default;
};

enum class ChartIdentityMatch : std::uint8_t {
  Match,
  Sha256Mismatch,
  Md5Mismatch,
  KeyModeMismatch,
};

[[nodiscard]] inline bool canonicalChartHashes(std::string_view md5,
                                               std::string_view sha256,
                                               bool requireMd5) noexcept {
  return canonical_digest::isCanonicalLowerHex(sha256, 64) &&
         ((!requireMd5 && md5.empty()) ||
          canonical_digest::isCanonicalLowerHex(md5, 32));
}

[[nodiscard]] inline bool canonicalChartIdentity(const ChartIdentity &identity,
                                                 bool requireMd5) noexcept {
  return canonicalChartHashes(identity.md5, identity.sha256, requireMd5) &&
         isSupportedKeyMode(identity.keyMode);
}

[[nodiscard]] inline ChartIdentityMatch
compareChartIdentity(const ChartIdentity &recorded,
                     const ChartIdentity &selected) noexcept {
  if (recorded.sha256 != selected.sha256) {
    return ChartIdentityMatch::Sha256Mismatch;
  }
  if (!recorded.md5.empty() && recorded.md5 != selected.md5) {
    return ChartIdentityMatch::Md5Mismatch;
  }
  if (recorded.keyMode != selected.keyMode) {
    return ChartIdentityMatch::KeyModeMismatch;
  }
  return ChartIdentityMatch::Match;
}

struct ResultOutcomeFacts {
  int score = 0;
  int maxScore = 0;
  int maxCombo = 0;
  int comboBreak = 0;
  int pGreat = 0;
  int great = 0;
  int good = 0;
  int bad = 0;
  int poor = 0;
  int kPoor = 0;
  int fast = 0;
  int slow = 0;
  float finalGauge = 0.0F;
  int clearType = kClearTypeFailedRank;
  std::span<const float> gaugeHistory;
};

[[nodiscard]] inline bool
validResultOutcome(const ResultOutcomeFacts &facts,
                   std::int64_t maximumAllowedCombo) noexcept {
  const std::array counts{
      facts.score,  facts.maxScore, facts.maxCombo, facts.comboBreak,
      facts.pGreat, facts.great,    facts.good,     facts.bad,
      facts.poor,   facts.kPoor,    facts.fast,     facts.slow,
  };
  for (const int count : counts) {
    if (count < 0) {
      return false;
    }
  }
  if (maximumAllowedCombo < 0 || facts.maxScore <= 0 ||
      facts.maxScore % 2 != 0 || facts.score > facts.maxScore ||
      static_cast<std::int64_t>(facts.maxCombo) > maximumAllowedCombo ||
      static_cast<std::int64_t>(facts.pGreat) * 2LL + facts.great !=
          facts.score ||
      !std::isfinite(facts.finalGauge) || facts.finalGauge < 0.0F ||
      !isKnownClearRank(facts.clearType) ||
      !durable_payload::withinLimit(
          facts.gaugeHistory.size(),
          durable_payload::kMaximumResultGaugeSamples)) {
    return false;
  }
  for (const float sample : facts.gaugeHistory) {
    if (!std::isfinite(sample)) {
      return false;
    }
  }
  return true;
}

} // namespace result_contract
