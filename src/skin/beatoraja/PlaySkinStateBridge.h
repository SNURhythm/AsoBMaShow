#pragma once

#include "BeatorajaSkinConfiguration.h"
#include "BeatorajaSkinModel.h"
#include "Skin2DRenderer.h"
#include "../../scene/play/PlayfieldProjection.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace skin {

enum class SkinEventMutationKind : std::uint8_t {
  SessionPresentation,
  SetOption,
  SetFilePath,
  SetOffset,
  ReadOnly,
  Unsupported,
};

struct SkinEventMutationRule {
  int builtInEventId = 0;
  SkinEventMutationKind kind = SkinEventMutationKind::Unsupported;
  std::uint8_t minimumArguments = 0;
  std::uint8_t maximumArguments = 0;
  std::string configurationKey;
};

class SkinEventMutationTable final {
public:
  static constexpr std::uint32_t schemaVersion = 1;

  SkinEventMutationTable() = default;
  explicit SkinEventMutationTable(std::vector<SkinEventMutationRule> rules);

  [[nodiscard]] const SkinEventMutationRule *find(int) const noexcept;
  [[nodiscard]] std::span<const SkinEventMutationRule> rules() const noexcept {
    return rules_;
  }

private:
  std::vector<SkinEventMutationRule> rules_;
};

[[nodiscard]] SkinEventMutationTable makePinnedSkinEventMutationTableV1();

struct SetSkinOption {
  std::string key;
  int value = 0;
};

struct SetSkinFilePath {
  std::string key;
  std::string declaredValue;
};

struct SetSkinOffset {
  std::string key;
  ConfigOffset value;
};

struct SessionPresentationWrite {
  int eventId = 0;
  std::array<int, 2> arguments{};
  std::uint8_t argumentCount = 0;
};

// Pinned FloatPropertyFactory.RateType exposes these three built-in
// FloatWriter targets during BMSPlayer gameplay. They mutate Config.AudioConfig
// rather than the skin configuration.
enum class SkinAudioVolumeWriterTarget : std::uint8_t {
  Master,
  Keysound,
  Bgm,
};

struct SetSkinAudioVolume {
  SkinAudioVolumeWriterTarget target = SkinAudioVolumeWriterTarget::Master;
  float value = 1.0F;
};

// FloatPropertyFactory.practice_position mutates BMSPlayer's retained
// PracticeConfiguration viewport, independently of STATE_PRACTICE.
struct SetPracticeItemScroll {
  float position = 0.0F;
};

// BMSPlayer.executeEvent applies BUTTON_PRACTICE_ITEM1 through ITEM16 only
// while STATE_PRACTICE is active. The visible row and arg1 direction are
// value-owned until the selected skin frame has submitted.
struct SetPracticeMenuItem {
  std::size_t visibleIndex = 0;
  bool increment = true;
};

struct SetPracticeVisibleItems {
  int count = 10;
};

using PersistedSkinConfigurationWrite =
    std::variant<SetSkinOption, SetSkinFilePath, SetSkinOffset>;
using SkinFrameMutation =
    std::variant<SessionPresentationWrite, PersistedSkinConfigurationWrite,
                 SetSkinAudioVolume, SetPracticeItemScroll,
                 SetPracticeMenuItem, SetPracticeVisibleItems>;

enum class SkinHostCallStatus : std::uint8_t {
  Completed,
  Unsupported,
  BudgetExceeded,
  CriticalFailure,
};

struct SkinHostCallResult {
  SkinHostCallStatus status = SkinHostCallStatus::Completed;
  std::uint32_t callbacksInvoked = 0;
  std::vector<SkinDiagnostic> diagnostics;

  [[nodiscard]] bool ok() const noexcept {
    return status == SkinHostCallStatus::Completed ||
           status == SkinHostCallStatus::Unsupported;
  }
};

struct PlaySkinFrameCommit {
  std::uint64_t frameSerial = 0;
  std::vector<SkinFrameMutation> orderedMutations;
};

struct PlaySkinStateBridgeContext {
  const PlayfieldChartVisualModel &chartModel;
  // The configured-load bridge has real gameplay state but no decoded model
  // yet. Model-dependent callbacks are unavailable until full model decoding
  // has completed.
  const ValidatedBeatorajaSkinModel *model = nullptr;
  const BeatorajaSkinConfiguration &configuration;
  LuaSkinRuntime *runtime = nullptr;
  const SkinEventMutationTable &mutationTable;
  // PomyuCharaProcessor initializes all eight motion cycles to one
  // millisecond. A decoded PLAY pmchara definition replaces individual
  // entries with its authored #Frame/#Anime cycle.
  std::array<int, 8> pomyuMotionCyclesMillis = {1, 1, 1, 1,
                                                 1, 1, 1, 1};
};

class PlaySkinStateBridge final : public ISkinFrameState {
public:
  explicit PlaySkinStateBridge(PlaySkinStateBridgeContext);
  ~PlaySkinStateBridge() override;

  PlaySkinStateBridge(const PlaySkinStateBridge &) = delete;
  PlaySkinStateBridge &operator=(const PlaySkinStateBridge &) = delete;
  PlaySkinStateBridge(PlaySkinStateBridge &&) = delete;
  PlaySkinStateBridge &operator=(PlaySkinStateBridge &&) = delete;

  void beginFrame(const PlayfieldVisualState &,
                  const PlayfieldProjectionResult &);
  SkinHostCallResult updateCustomObjects();
  SkinHostCallResult executeEvent(int, std::span<const int> arguments);
  SkinHostCallResult invokeEventBinding(SkinEventBindingId,
                                        std::span<const int> arguments);
  SkinHostCallResult invokeWriter(SkinFloatWriterId, double normalizedValue);
  SkinHostCallResult invokeWriter(SkinStringWriterId, std::string_view value);
  [[nodiscard]] PlaySkinFrameCommit takeFrameCommitForContinuation();
  [[nodiscard]] PlaySkinFrameCommit commitFrame();
  void discardFrame() noexcept;

  [[nodiscard]] std::uint64_t frameSerial() const noexcept override;
  SkinPropertyLookup<bool>
  booleanProperty(const SkinBuiltinPropertySelector &) override;
  SkinPropertyLookup<std::int64_t>
  integerProperty(const SkinBuiltinPropertySelector &,
                  SkinIntegerPropertyDomain =
                      SkinIntegerPropertyDomain::IntegerValue) override;
  SkinPropertyLookup<double>
  floatProperty(const SkinBuiltinPropertySelector &,
                SkinFloatPropertyDomain = SkinFloatPropertyDomain::Rate) override;
  SkinPropertyLookup<std::string_view>
  stringProperty(const SkinBuiltinPropertySelector &) override;
  SkinPropertyLookup<SkinRuntimeOffset> offsetProperty(int) override;
  [[nodiscard]] SkinLaneCoverStateView
  laneCoverState() const noexcept override;
  std::int64_t timerProperty(const SkinBuiltinPropertySelector &) override;
  [[nodiscard]] std::span<const SkinProjectedNoteView>
  projectedNotes() const noexcept override;
  [[nodiscard]] std::span<const SkinProjectedLongNoteView>
  projectedLongNotes() const noexcept override;
  [[nodiscard]] std::span<const SkinProjectedLineView>
  projectedLines() const noexcept override;
  [[nodiscard]] SkinGameplayGraphStateView
  gameplayGraphState() const noexcept override;
  [[nodiscard]] SkinGaugeStateView gaugeState() const noexcept override;
  [[nodiscard]] SkinJudgeStateView judgeState(int) const noexcept override;
  [[nodiscard]] SkinNoteExpansionStateView
  noteExpansionState() const noexcept override;
  [[nodiscard]] SkinPracticeStateView
  practiceState() const noexcept override;
  bool stagePracticeVisibleItemCount(int) override;
  [[nodiscard]] std::span<const SkinDiagnostic> diagnostics() const noexcept;

private:
  static constexpr std::size_t maximumDiagnostics = 128;

  enum class FramePhase : std::uint8_t { Closed, Active };

  void closeFrame() noexcept;
  void reportDiagnostic(SkinDiagnostic);
  void reportUnsupported(std::string_view kind,
                         const SkinBuiltinPropertySelector &);
  void reportUnsupportedEvent(int);
  [[nodiscard]] SkinHostCallResult updateCustomTimer(const SkinCustomTimer &);
  [[nodiscard]] SkinHostCallResult updateCustomEvent(const SkinCustomEvent &);
  [[nodiscard]] SkinHostCallResult invokeCustomEvent(
      const SkinCustomEvent &, std::span<const int> arguments);
  [[nodiscard]] SkinHostCallResult evaluateCustomCondition(
      SkinBooleanPropertyId, bool &condition);
  [[nodiscard]] SkinHostCallResult evaluateCustomTimer(
      SkinTimerPropertyId, std::int64_t &value);
  [[nodiscard]] SkinHostCallResult callbackFailure(SkinDiagnostic);
  void rollbackFrameWrites() noexcept;
  static LuaSkinEventExecutionResult executeHostEvent(
      void *, int, std::span<const int>) noexcept;
  [[nodiscard]] const PlayfieldVisualState *state() const noexcept;
  void updatePinnedLaneCoverOffsets();
  void updatePinnedPlayTimers();
  void updatePinnedPomyuTimers();
  [[nodiscard]] std::optional<int>
  numericSelector(const SkinBuiltinPropertySelector &) const noexcept;

  PlaySkinStateBridgeContext context_;
  FramePhase phase_ = FramePhase::Closed;
  bool runtimeBound_ = false;
  bool customObjectsUpdated_ = false;
  bool writerInvocationActive_ = false;
  std::optional<PlayfieldVisualState> state_;
  std::uint64_t frameSerial_ = 0;
  std::uint64_t lastAcceptedFrameSerial_ = 0;
  std::optional<BuiltInRendererTraversal> builtInTraversal_;
  SkinRuntimeOffset liftOffset_;
  SkinRuntimeOffset laneCoverOffset_;
  SkinRuntimeOffset hiddenCoverOffset_;
  PlayfieldSkinProjectionViews projection_;
  // StringPropertyFactory resolves target neighbours from a PlayerConfig
  // target ring. Keep their source names stable for every lookup in a frame.
  std::vector<std::string> targetNeighbourNames_;
  std::string targetScorePlayerName_;
  PlaySkinFrameCommit staged_;
  std::unordered_map<int, std::int64_t> customTimerValues_;
  std::unordered_map<int, std::int64_t> customEventLastExecutionMicros_;
  std::int64_t endOfNoteTimerStartMicros_ = kPlayfieldTimestampOff;
  std::int64_t musicEndTimerStartMicros_ = kPlayfieldTimestampOff;
  std::int64_t fadeoutTimerStartMicros_ = kPlayfieldTimestampOff;
  std::int64_t fullComboTimerStartMicros_ = kPlayfieldTimestampOff;
  std::int64_t rhythmTimerStartMicros_ = kPlayfieldTimestampOff;
  std::int64_t rhythmAccumulatorMicros_ = 0;
  std::optional<std::int64_t> rhythmPreviousClockMicros_;
  std::size_t rhythmSectionIndex_ = 0;
  // PlaySkin constructs PomyuCharaProcessor for every gameplay skin. Its
  // unconfigured motion durations are all one millisecond; a future decoded
  // pmchara object may replace those source defaults per timer.
  std::array<std::int64_t, 8> pomyuTimerStarts_ = {
      kPlayfieldTimestampOff, kPlayfieldTimestampOff,
      kPlayfieldTimestampOff, kPlayfieldTimestampOff,
      kPlayfieldTimestampOff, kPlayfieldTimestampOff,
      kPlayfieldTimestampOff, kPlayfieldTimestampOff};
  std::array<int, 2> pomyuLastNotes_{};
  std::int64_t pomyuDanceTimerStartMicros_ = kPlayfieldTimestampOff;
  std::unordered_map<int, std::int64_t> pinnedSwitchTimerStarts_;
  std::vector<SkinDiagnostic> diagnostics_;
};

} // namespace skin
