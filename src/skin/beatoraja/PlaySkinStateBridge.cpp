#include "PlaySkinStateBridge.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace skin {

SkinEventMutationTable::SkinEventMutationTable(
    std::vector<SkinEventMutationRule> rules)
    : rules_(std::move(rules)) {
  std::sort(rules_.begin(), rules_.end(),
            [](const SkinEventMutationRule &left,
               const SkinEventMutationRule &right) {
              return left.builtInEventId < right.builtInEventId;
            });
}

const SkinEventMutationRule *
SkinEventMutationTable::find(int eventId) const noexcept {
  const auto found =
      std::lower_bound(rules_.begin(), rules_.end(), eventId,
                       [](const SkinEventMutationRule &rule, int value) {
                         return rule.builtInEventId < value;
                       });
  return found != rules_.end() && found->builtInEventId == eventId ? &*found
                                                                   : nullptr;
}

SkinEventMutationTable makePinnedSkinEventMutationTableV1() {
  // Pinned Beatoraja c2ed5db1 EventFactory permits these controls, but event
  // 74 mutates gameplay judgment authority and is intentionally unavailable.
  // The selected SCURO selector events are harmless gameplay no-ops.
  return SkinEventMutationTable({
      {.builtInEventId = 74},
      {.builtInEventId = 301,
       .kind = SkinEventMutationKind::ReadOnly,
       .maximumArguments = 2},
      {.builtInEventId = 302,
       .kind = SkinEventMutationKind::ReadOnly,
       .maximumArguments = 2},
      {.builtInEventId = 303,
       .kind = SkinEventMutationKind::ReadOnly,
       .maximumArguments = 2},
      {.builtInEventId = 304,
       .kind = SkinEventMutationKind::ReadOnly,
       .maximumArguments = 2},
      {.builtInEventId = 305,
       .kind = SkinEventMutationKind::ReadOnly,
       .maximumArguments = 2},
      {.builtInEventId = 306,
       .kind = SkinEventMutationKind::ReadOnly,
       .maximumArguments = 2},
      {.builtInEventId = 307,
       .kind = SkinEventMutationKind::ReadOnly,
       .maximumArguments = 2},
      {.builtInEventId = 308,
       .kind = SkinEventMutationKind::ReadOnly,
       .maximumArguments = 2},
  });
}

PlaySkinStateBridge::PlaySkinStateBridge(PlaySkinStateBridgeContext context)
    : context_(context) {}

PlaySkinStateBridge::~PlaySkinStateBridge() { closeFrame(); }

void PlaySkinStateBridge::beginFrame(
    const PlayfieldVisualState &state,
    const PlayfieldProjectionResult &projection) {
  closeFrame();
  diagnostics_.clear();
  if (state.clock.serial == 0 || projection.frameSerial != state.clock.serial) {
    reportDiagnostic({.code = "skin.play_state.frame_serial_invalid",
                      .message = "Play skin state and projection must share a "
                                 "nonzero frame serial."});
    return;
  }
  if (state.clock.serial <= lastAcceptedFrameSerial_) {
    reportDiagnostic(
        {.code = "skin.play_state.frame_serial_not_increasing",
         .message = "Play skin frame serials must increase within a session."});
    return;
  }

  state_ = state;
  projection_ = adaptPlayfieldProjectionForSkin(projection);
  frameSerial_ = state.clock.serial;
  staged_ = {.frameSerial = frameSerial_};
  phase_ = FramePhase::Active;
  customObjectsUpdated_ = false;

  context_.runtime.setFrameState(this);
  runtimeBound_ = true;
  lastAcceptedFrameSerial_ = frameSerial_;
}

SkinHostCallResult PlaySkinStateBridge::updateCustomObjects() {
  if (phase_ != FramePhase::Active) {
    reportDiagnostic({.code = "skin.play_state.frame_inactive",
                      .message = "Custom objects require an active frame."});
    return {.status = SkinHostCallStatus::CriticalFailure,
            .diagnostics = diagnostics_};
  }
  if (customObjectsUpdated_) {
    return {.diagnostics = diagnostics_};
  }

  if (context_.model.model.customTimers.empty() &&
      context_.model.model.customEvents.empty()) {
    customObjectsUpdated_ = true;
    return {.diagnostics = diagnostics_};
  }
  reportDiagnostic(
      {.code = "skin.play_state.custom_objects_unavailable",
       .message = "Custom object callbacks need one shared Lua frame owner; "
                  "the current renderer owns beginFrame and cannot be "
                  "coordinated from the state bridge alone.",
       .severity = DiagnosticSeverity::Warning});
  return {.status = SkinHostCallStatus::Unsupported,
          .diagnostics = diagnostics_};
}

SkinHostCallResult
PlaySkinStateBridge::executeEvent(int eventId, std::span<const int> arguments) {
  if (phase_ != FramePhase::Active) {
    reportDiagnostic({.code = "skin.play_state.frame_inactive",
                      .message = "Skin events require an active frame."});
    return {.status = SkinHostCallStatus::CriticalFailure,
            .diagnostics = diagnostics_};
  }
  if (arguments.size() > 2) {
    reportUnsupportedEvent(eventId);
    return {.status = SkinHostCallStatus::Unsupported,
            .diagnostics = diagnostics_};
  }

  if (std::ranges::any_of(context_.model.model.customEvents,
                          [eventId](const SkinCustomEvent &event) {
                            return event.id == eventId;
                          })) {
    reportDiagnostic(
        {.code = "skin.play_state.custom_objects_unavailable",
         .message =
             "Manual custom events require the shared Lua frame owner."});
    return {.status = SkinHostCallStatus::Unsupported,
            .diagnostics = diagnostics_};
  }

  const auto *rule = context_.mutationTable.find(eventId);
  if (rule == nullptr || arguments.size() < rule->minimumArguments ||
      arguments.size() > rule->maximumArguments ||
      rule->kind == SkinEventMutationKind::Unsupported) {
    reportUnsupportedEvent(eventId);
    return {.status = SkinHostCallStatus::Unsupported,
            .diagnostics = diagnostics_};
  }
  if (rule->kind == SkinEventMutationKind::ReadOnly) {
    return {.diagnostics = diagnostics_};
  }
  // Version 1 intentionally has no mutating rules. Keeping the default closed
  // prevents a future table-only edit from bypassing session integration.
  reportUnsupportedEvent(eventId);
  return {.status = SkinHostCallStatus::Unsupported,
          .diagnostics = diagnostics_};
}

PlaySkinFrameCommit PlaySkinStateBridge::commitFrame() {
  if (phase_ != FramePhase::Active) {
    reportDiagnostic({.code = "skin.play_state.frame_already_closed",
                      .message = "A play-skin frame can be committed once."});
    return {};
  }
  auto result = std::move(staged_);
  closeFrame();
  return result;
}

void PlaySkinStateBridge::discardFrame() noexcept { closeFrame(); }

std::uint64_t PlaySkinStateBridge::frameSerial() const noexcept {
  return phase_ == FramePhase::Active ? frameSerial_ : 0;
}

SkinPropertyLookup<bool> PlaySkinStateBridge::booleanProperty(
    const SkinBuiltinPropertySelector &selector) {
  const auto *snapshot = state();
  const auto id = numericSelector(selector);
  if (snapshot == nullptr || !id) {
    reportUnsupported("boolean", selector);
    return {};
  }
  switch (*id) {
  case 42:
    return {.value = gaugeTypeIndex(snapshot->authority.gaugeType) <= 2,
            .supported = true};
  case 43:
    return {.value = gaugeTypeIndex(snapshot->authority.gaugeType) >= 3,
            .supported = true};
  case 1240: {
    const auto gauge = gaugeState();
    if (!gauge.supported) {
      reportUnsupported("boolean", selector);
      return {};
    }
    return {.value = gauge.value > 0.0 && gauge.value >= gauge.border,
            .supported = true};
  }
  case 241:
    return {.value = snapshot->lastJudge.judgement == PGreat,
            .supported = true};
  case 1242:
    return {.value = snapshot->lastJudge.judgement != None &&
                     snapshot->fastSlowMicros > 0,
            .supported = true};
  case 1243:
    return {.value = snapshot->lastJudge.judgement != None &&
                     snapshot->fastSlowMicros < 0,
            .supported = true};
  default:
    break;
  }
  if (*id >= 230 && *id <= 240) {
    const auto gauge = gaugeState();
    if (!gauge.supported || !std::isfinite(gauge.maximum) ||
        gauge.maximum <= 0.0) {
      reportUnsupported("boolean", selector);
      return {};
    }
    const int range = *id - 230;
    const double low = static_cast<double>(range) * 0.1 * gauge.maximum;
    const double high = static_cast<double>(range + 1) * 0.1 * gauge.maximum;
    return {.value = gauge.value >= low && gauge.value < high,
            .supported = true};
  }
  reportUnsupported("boolean", selector);
  return {};
}

SkinPropertyLookup<std::int64_t> PlaySkinStateBridge::integerProperty(
    const SkinBuiltinPropertySelector &selector) {
  const auto *snapshot = state();
  const auto id = numericSelector(selector);
  if (snapshot == nullptr || !id) {
    reportUnsupported("integer", selector);
    return {};
  }
  switch (*id) {
  case 160:
    return {.value = static_cast<std::int64_t>(snapshot->authority.currentBpm),
            .supported = true};
  default:
    reportUnsupported("integer", selector);
    return {};
  }
}

SkinPropertyLookup<double> PlaySkinStateBridge::floatProperty(
    const SkinBuiltinPropertySelector &selector) {
  const auto *snapshot = state();
  const auto id = numericSelector(selector);
  if (snapshot != nullptr && id && *id == 4) {
    return {.value = static_cast<double>(snapshot->authority.laneCoverPercent) /
                     100.0,
            .supported = true};
  }
  reportUnsupported("float", selector);
  return {};
}

SkinPropertyLookup<std::string_view> PlaySkinStateBridge::stringProperty(
    const SkinBuiltinPropertySelector &selector) {
  if (state() == nullptr) {
    reportUnsupported("string", selector);
    return {};
  }
  const auto id = numericSelector(selector);
  if (!id) {
    reportUnsupported("string", selector);
    return {};
  }
  const auto &text = context_.chartModel.text;
  switch (*id) {
  case 10:
    return {.value = text.title, .supported = true};
  case 11:
    return {.value = text.subtitle, .supported = true};
  case 12: {
    const auto audited = text.auditedStringProperties.find(12);
    if (audited != text.auditedStringProperties.end()) {
      return {.value = audited->second, .supported = true};
    }
    break;
  }
  case 13:
    return {.value = text.genre, .supported = true};
  case 14:
    return {.value = text.artist, .supported = true};
  case 15:
    return {.value = text.subartist, .supported = true};
  default:
    break;
  }
  reportUnsupported("string", selector);
  return {};
}

SkinPropertyLookup<ConfigOffset> PlaySkinStateBridge::offsetProperty(int id) {
  if (state() != nullptr) {
    const auto found = context_.configuration.offsetsById.find(id);
    if (found != context_.configuration.offsetsById.end()) {
      return {.value = found->second, .supported = true};
    }
  }
  reportUnsupported("offset", SkinBuiltinPropertySelector{.value = id});
  return {};
}

std::int64_t PlaySkinStateBridge::timerProperty(
    const SkinBuiltinPropertySelector &selector) {
  const auto *snapshot = state();
  const auto id = numericSelector(selector);
  if (snapshot == nullptr || !id) {
    reportUnsupported("timer", selector);
    return INT64_MIN;
  }
  if (*id < 0) {
    reportUnsupported("timer", selector);
    return INT64_MIN;
  }
  const auto laneTimer =
      [snapshot](int firstId, int count,
                 long long LanePresentationState::*field,
                 int timerId) -> std::optional<std::int64_t> {
    const auto wide = static_cast<std::int64_t>(timerId);
    const auto first = static_cast<std::int64_t>(firstId);
    if (wide < first || wide >= first + count) {
      return std::nullopt;
    }
    const auto index = static_cast<std::size_t>(wide - first);
    return index < snapshot->lanes.size()
               ? std::optional<std::int64_t>{snapshot->lanes[index].*field}
               : std::optional<std::int64_t>{INT64_MIN};
  };
  if (const auto value =
          laneTimer(100, 20, &LanePresentationState::pressMicros, *id)) {
    return *value;
  }
  if (const auto value =
          laneTimer(120, 20, &LanePresentationState::releaseMicros, *id)) {
    return *value;
  }
  if (const auto value =
          laneTimer(50, 20, &LanePresentationState::bombMicros, *id)) {
    return *value;
  }
  switch (*id) {
  case 41:
    return snapshot->playStartMicros;
  case 46:
    return snapshot->lastJudgeVisualMicros;
  default:
    // MainStatePropertyLuaApiExporter reads arbitrary nonnegative timer IDs
    // directly, and TimerPropertyFactory recognizes every nonnegative ID.
    return INT64_MIN;
  }
}

std::span<const SkinProjectedNoteView>
PlaySkinStateBridge::projectedNotes() const noexcept {
  return phase_ == FramePhase::Active
             ? std::span<const SkinProjectedNoteView>{projection_.notes}
             : std::span<const SkinProjectedNoteView>{};
}

std::span<const SkinProjectedLongNoteView>
PlaySkinStateBridge::projectedLongNotes() const noexcept {
  return phase_ == FramePhase::Active
             ? std::span<const SkinProjectedLongNoteView>{projection_.longNotes}
             : std::span<const SkinProjectedLongNoteView>{};
}

std::span<const SkinProjectedLineView>
PlaySkinStateBridge::projectedLines() const noexcept {
  return phase_ == FramePhase::Active
             ? std::span<const SkinProjectedLineView>{projection_.lines}
             : std::span<const SkinProjectedLineView>{};
}

SkinGaugeStateView PlaySkinStateBridge::gaugeState() const noexcept {
  const auto *snapshot = state();
  if (snapshot == nullptr || !snapshot->authority.gaugeRules.compiled) {
    return {};
  }
  const int type = gaugeTypeIndex(snapshot->authority.gaugeType);
  if (type < 0 || static_cast<std::size_t>(type) >=
                      snapshot->authority.gaugeRules.gauges.size()) {
    return {};
  }
  const auto &definition =
      snapshot->authority.gaugeRules.gauges[static_cast<std::size_t>(type)];
  return {.supported = true,
          .value = snapshot->authority.currentGauge,
          .gaugeType = type,
          .minimum = definition.minimum,
          .maximum = definition.maximum,
          .border = definition.clearBorder};
}

SkinJudgeStateView PlaySkinStateBridge::judgeState(int player) const noexcept {
  const auto *snapshot = state();
  if (snapshot == nullptr || player != 0 ||
      snapshot->lastJudge.judgement == None) {
    return {};
  }
  const auto gauge = gaugeState();
  return {.supported = true,
          .optionalZeroBasedGrade =
              static_cast<int>(snapshot->lastJudge.judgement),
          .combo = snapshot->combo,
          .maximumGauge = gauge.supported && gauge.value >= gauge.maximum};
}

SkinNoteExpansionStateView
PlaySkinStateBridge::noteExpansionState() const noexcept {
  return {};
}

std::span<const SkinDiagnostic>
PlaySkinStateBridge::diagnostics() const noexcept {
  return diagnostics_;
}

void PlaySkinStateBridge::closeFrame() noexcept {
  if (runtimeBound_) {
    context_.runtime.setFrameState(nullptr);
    runtimeBound_ = false;
  }
  phase_ = FramePhase::Closed;
  customObjectsUpdated_ = false;
  state_.reset();
  frameSerial_ = 0;
  projection_ = {};
  staged_ = {};
}

void PlaySkinStateBridge::reportDiagnostic(SkinDiagnostic diagnostic) {
  const auto existing = std::ranges::find_if(
      diagnostics_, [&diagnostic](const SkinDiagnostic &candidate) {
        return candidate.code == diagnostic.code &&
               candidate.virtualPath == diagnostic.virtualPath;
      });
  if (existing != diagnostics_.end() ||
      diagnostics_.size() >= maximumDiagnostics) {
    return;
  }
  if (diagnostics_.size() == maximumDiagnostics - 1) {
    diagnostics_.push_back(
        {.code = "skin.play_state.diagnostics_truncated",
         .message = "Additional play-skin diagnostics were suppressed.",
         .severity = DiagnosticSeverity::Warning});
    return;
  }
  diagnostics_.push_back(std::move(diagnostic));
}

void PlaySkinStateBridge::reportUnsupported(
    std::string_view kind, const SkinBuiltinPropertySelector &selector) {
  std::string subject = std::string(kind) + ':';
  if (const auto *numeric = std::get_if<int>(&selector.value)) {
    subject += std::to_string(*numeric);
  } else {
    subject += std::get<std::string>(selector.value);
  }
  reportDiagnostic(
      {.code = "skin.play_state.unsupported",
       .message = "No authoritative gameplay source for " + subject,
       .virtualPath = std::move(subject)});
}

void PlaySkinStateBridge::reportUnsupportedEvent(int eventId) {
  reportUnsupported("event", SkinBuiltinPropertySelector{.value = eventId});
}

const PlayfieldVisualState *PlaySkinStateBridge::state() const noexcept {
  return phase_ == FramePhase::Active && state_ ? &*state_ : nullptr;
}

std::optional<int> PlaySkinStateBridge::numericSelector(
    const SkinBuiltinPropertySelector &selector) const noexcept {
  if (const auto *numeric = std::get_if<int>(&selector.value)) {
    return *numeric;
  }
  const auto &name = std::get<std::string>(selector.value);
  if (name == "title")
    return 10;
  if (name == "subtitle")
    return 11;
  if (name == "fulltitle")
    return 12;
  if (name == "genre")
    return 13;
  if (name == "artist")
    return 14;
  if (name == "subartist")
    return 15;
  if (name == "nowbpm")
    return 160;
  if (name == "lanecover")
    return 4;
  if (name == "judge_1p_perfect")
    return 241;
  if (name == "judge_1p_early")
    return 1242;
  if (name == "judge_1p_late")
    return 1243;
  return std::nullopt;
}

} // namespace skin
