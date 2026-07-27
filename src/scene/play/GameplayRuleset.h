#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

enum class GameplayRuleset : std::uint8_t { LR2, Beatoraja };

inline constexpr GameplayRuleset kDefaultGameplayRuleset =
    GameplayRuleset::LR2;

struct RulesetDescriptor {
  static constexpr int kCurrentVersion = 3;

  std::string id = "lr2";
  int version = 3;
  std::string scoringModel = "asobmashow-v1";
  std::string judgementModel = "lr2-v1";
  std::string gaugeModel = "lr2-gauge-v1";

  bool operator==(const RulesetDescriptor &) const = default;

  static RulesetDescriptor For(GameplayRuleset ruleset);
  static RulesetDescriptor Current();
  static RulesetDescriptor Legacy();
};

[[nodiscard]] inline std::optional<GameplayRuleset>
gameplayRulesetFromId(std::string_view id) noexcept {
  if (id == "lr2") {
    return GameplayRuleset::LR2;
  }
  if (id == "beatoraja") {
    return GameplayRuleset::Beatoraja;
  }
  return std::nullopt;
}

[[nodiscard]] inline std::string_view
gameplayRulesetId(GameplayRuleset ruleset) noexcept {
  switch (ruleset) {
  case GameplayRuleset::LR2:
    return "lr2";
  case GameplayRuleset::Beatoraja:
    return "beatoraja";
  }
  return "lr2";
}

[[nodiscard]] inline std::string_view
gameplayRulesetLabel(GameplayRuleset ruleset) noexcept {
  switch (ruleset) {
  case GameplayRuleset::LR2:
    return "LR2";
  case GameplayRuleset::Beatoraja:
    return "Beatoraja";
  }
  return "LR2";
}

[[nodiscard]] inline GameplayRuleset
gameplayRulesetSelectionOrDefault(std::string_view id) noexcept {
  return gameplayRulesetFromId(id).value_or(kDefaultGameplayRuleset);
}

inline RulesetDescriptor RulesetDescriptor::For(GameplayRuleset ruleset) {
  switch (ruleset) {
  case GameplayRuleset::LR2:
    return {};
  case GameplayRuleset::Beatoraja:
    return {
        .id = "beatoraja",
        .version = 2,
        .scoringModel = "asobmashow-v1",
        .judgementModel = "bms-rank-v1",
        .gaugeModel = "beatoraja-profile-gauge-v2",
    };
  }
  return {};
}

inline RulesetDescriptor RulesetDescriptor::Current() {
  return For(GameplayRuleset::LR2);
}

inline RulesetDescriptor RulesetDescriptor::Legacy() {
  return {
      .id = "legacy-unknown",
      .version = 0,
      .scoringModel = "legacy-unknown",
      .judgementModel = "legacy-unknown",
      .gaugeModel = "legacy-unknown",
  };
}

[[nodiscard]] inline bool
isLegacyRulesetIdentity(std::string_view id, int version) noexcept {
  const RulesetDescriptor legacy = RulesetDescriptor::Legacy();
  return id == legacy.id && version == legacy.version;
}

[[nodiscard]] inline std::optional<GameplayRuleset>
gameplayRulesetFromSupportedIdentity(std::string_view id,
                                     int version) noexcept {
  const auto ruleset = gameplayRulesetFromId(id);
  if (!ruleset.has_value() ||
      RulesetDescriptor::For(*ruleset).version != version) {
    return std::nullopt;
  }
  return ruleset;
}

[[nodiscard]] inline std::optional<GameplayRuleset>
gameplayRulesetFromReplayIdentity(std::string_view id,
                                  int version) noexcept {
  if (isLegacyRulesetIdentity(id, version)) {
    return GameplayRuleset::Beatoraja;
  }
  return gameplayRulesetFromSupportedIdentity(id, version);
}

[[nodiscard]] inline RulesetDescriptor
rulesetDescriptorFromReplayIdentity(std::string_view id, int version) {
  if (const auto ruleset =
          gameplayRulesetFromSupportedIdentity(id, version)) {
    return RulesetDescriptor::For(*ruleset);
  }
  RulesetDescriptor descriptor = RulesetDescriptor::Legacy();
  descriptor.id = std::string(id);
  descriptor.version = version;
  return descriptor;
}

[[nodiscard]] inline bool
isSupportedRulesetDescriptor(const RulesetDescriptor &descriptor) noexcept {
  return descriptor == RulesetDescriptor::For(GameplayRuleset::LR2) ||
         descriptor == RulesetDescriptor::For(GameplayRuleset::Beatoraja);
}
