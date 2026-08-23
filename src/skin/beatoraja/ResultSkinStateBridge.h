#pragma once

#include "Skin2DRenderer.h"

#include "../../scene/ResultScene.h"

#include <cstdint>
#include <string>
#include <vector>

namespace skin {

// Result scenes have no playfield projection. This bridge deliberately
// supplies Beatoraja's result properties from the immutable result snapshot
// and reports every gameplay-only property as unsupported.
class ResultSkinStateBridge final : public ISkinFrameState {
public:
  ResultSkinStateBridge(ResultSkinData data, std::uint64_t frameSerial,
                        std::int64_t elapsedMillis,
                        const BeatorajaSkinConfiguration *configuration = nullptr);

  std::uint64_t frameSerial() const noexcept override;
  SkinPropertyLookup<bool>
  booleanProperty(const SkinBuiltinPropertySelector &) override;
  SkinPropertyLookup<std::int64_t>
  integerProperty(const SkinBuiltinPropertySelector &,
                  SkinIntegerPropertyDomain) override;
  SkinPropertyLookup<double>
  floatProperty(const SkinBuiltinPropertySelector &,
                SkinFloatPropertyDomain) override;
  SkinPropertyLookup<std::string_view>
  stringProperty(const SkinBuiltinPropertySelector &) override;
  SkinPropertyLookup<SkinRuntimeOffset> offsetProperty(int) override;
  std::int64_t timerProperty(const SkinBuiltinPropertySelector &) override;
  std::span<const SkinProjectedNoteView> projectedNotes() const noexcept override;
  std::span<const SkinProjectedLongNoteView>
  projectedLongNotes() const noexcept override;
  std::span<const SkinProjectedLineView> projectedLines() const noexcept override;
  SkinGameplayGraphStateView gameplayGraphState() const noexcept override;
  SkinGaugeStateView gaugeState() const noexcept override;
  SkinJudgeStateView judgeState(int player) const noexcept override;
  SkinNoteExpansionStateView noteExpansionState() const noexcept override;

private:
  [[nodiscard]] std::optional<int> integerSelector(
      const SkinBuiltinPropertySelector &) const noexcept;
  [[nodiscard]] std::optional<int> count(Judgement) const noexcept;
  [[nodiscard]] std::optional<int> score() const noexcept;
  [[nodiscard]] std::optional<int> maxScore() const noexcept;
  [[nodiscard]] std::optional<int> maxCombo() const noexcept;
  [[nodiscard]] std::optional<float> finalGauge() const noexcept;
  [[nodiscard]] std::optional<int>
  timing(Judgement judgement, bool early) const noexcept;

  ResultSkinData data_;
  std::uint64_t frameSerial_ = 0;
  std::int64_t elapsedMillis_ = 0;
  std::string stringValue_;
  std::vector<float> gaugeHistory_;
  std::uint64_t gaugeRevision_ = 0;
  const BeatorajaSkinConfiguration *configuration_ = nullptr;
};

} // namespace skin
