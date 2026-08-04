#include "PlaySkinStateBridge.h"

#include "LuaSkinHostModules.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>
#include <utility>

namespace {

std::int64_t capturedJudgeCount(const PlayfieldVisualState &snapshot,
                                Judgement judgement) {
  const auto found = snapshot.authority.judgementCounters.find(judgement);
  return found == snapshot.authority.judgementCounters.end() ? 0
                                                              : found->second;
}

std::int64_t javaDoubleToInt(double value) {
  if (std::isnan(value)) {
    return 0;
  }
  if (value >= static_cast<double>(std::numeric_limits<int>::max())) {
    return std::numeric_limits<int>::max();
  }
  if (value <= static_cast<double>(std::numeric_limits<int>::min())) {
    return std::numeric_limits<int>::min();
  }
  return static_cast<int>(value);
}

} // namespace

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
  customTimerValues_.clear();

  context_.runtime.setFrameState(this);
  context_.runtime.setEventExecutor(
      {.context = this, .execute = &PlaySkinStateBridge::executeHostEvent});
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
      {.code = "custom_object_order_authored_divergence",
       .message = "Pinned custom object construction order differs from "
                  "runtime timer/event phase order",
       .severity = DiagnosticSeverity::Warning});

  SkinHostCallResult result;
  // IntMap iteration in the pinned target is not reproducible from IDs alone.
  // The validated model preserves declaration order, so use it directly.
  for (const auto &timer : context_.model.model.customTimers) {
    const auto updated = updateCustomTimer(timer);
    result.callbacksInvoked += updated.callbacksInvoked;
    if (updated.status != SkinHostCallStatus::Completed) {
      rollbackFrameWrites();
      customObjectsUpdated_ = true;
      result.status = updated.status;
      result.diagnostics = diagnostics_;
      return result;
    }
  }
  for (const auto &event : context_.model.model.customEvents) {
    const auto updated = updateCustomEvent(event);
    result.callbacksInvoked += updated.callbacksInvoked;
    if (updated.status != SkinHostCallStatus::Completed) {
      rollbackFrameWrites();
      customObjectsUpdated_ = true;
      result.status = updated.status;
      result.diagnostics = diagnostics_;
      return result;
    }
  }
  customObjectsUpdated_ = true;
  result.diagnostics = diagnostics_;
  return result;
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

  if (const auto event = std::ranges::find_if(
          context_.model.model.customEvents,
          [eventId](const SkinCustomEvent &candidate) {
            return candidate.id == eventId;
          }); event != context_.model.model.customEvents.end()) {
    auto invoked = invokeCustomEvent(*event, arguments);
    if (invoked.status == SkinHostCallStatus::Completed) {
      try {
        // Pinned CustomEvent.execute shares this clock with automatic update,
        // so a manual invocation suppresses the same event until minInterval.
        customEventLastExecutionMicros_.insert_or_assign(
            event->id, state_->clock.visualTimeMicros);
      } catch (...) {
        reportDiagnostic({.code = "skin.play_state.custom_event_clock_failed",
                          .message =
                              "Custom event clock could not be recorded."});
        invoked.status = SkinHostCallStatus::CriticalFailure;
      }
    }
    if (invoked.status != SkinHostCallStatus::Completed) {
      rollbackFrameWrites();
    }
    return {.status = invoked.status,
            .callbacksInvoked = invoked.callbacksInvoked,
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
  if (rule->kind == SkinEventMutationKind::SessionPresentation) {
    SessionPresentationWrite mutation{
        .eventId = eventId,
        .argumentCount = static_cast<std::uint8_t>(arguments.size())};
    std::ranges::copy(arguments, mutation.arguments.begin());
    try {
      staged_.orderedMutations.emplace_back(std::move(mutation));
    } catch (...) {
      reportDiagnostic(
          {.code = "skin.play_state.mutation_limit_exceeded",
           .message = "Skin event mutation could not be staged."});
      return {.status = SkinHostCallStatus::CriticalFailure,
              .diagnostics = diagnostics_};
    }
    return {.diagnostics = diagnostics_};
  }
  // Version 1 intentionally has no mutating rules. Keeping the default closed
  // prevents a future table-only edit from bypassing session integration.
  reportUnsupportedEvent(eventId);
  return {.status = SkinHostCallStatus::Unsupported,
          .diagnostics = diagnostics_};
}

SkinHostCallResult
PlaySkinStateBridge::updateCustomTimer(const SkinCustomTimer &timer) {
  std::int64_t value = INT64_MIN;
  if (timer.timer) {
    const auto resolved = evaluateCustomTimer(*timer.timer, value);
    if (resolved.status != SkinHostCallStatus::Completed) {
      return resolved;
    }
    try {
      customTimerValues_.insert_or_assign(timer.id, value);
    } catch (...) {
      reportDiagnostic({.code = "skin.play_state.custom_timer_cache_failed",
                        .message = "Custom timer value could not be cached."});
      return {.status = SkinHostCallStatus::CriticalFailure,
              .callbacksInvoked = resolved.callbacksInvoked,
              .diagnostics = diagnostics_};
    }
    return {.callbacksInvoked = resolved.callbacksInvoked,
            .diagnostics = diagnostics_};
  }
  try {
    customTimerValues_.insert_or_assign(timer.id, INT64_MIN);
  } catch (...) {
    reportDiagnostic({.code = "skin.play_state.custom_timer_cache_failed",
                      .message = "Passive custom timer value could not be cached."});
    return {.status = SkinHostCallStatus::CriticalFailure,
            .diagnostics = diagnostics_};
  }
  return {.diagnostics = diagnostics_};
}

SkinHostCallResult
PlaySkinStateBridge::updateCustomEvent(const SkinCustomEvent &event) {
  if (!event.condition) {
    return {.diagnostics = diagnostics_};
  }
  bool condition = false;
  const auto evaluated = evaluateCustomCondition(*event.condition, condition);
  if (evaluated.status != SkinHostCallStatus::Completed || !condition) {
    return evaluated;
  }
  const std::int64_t now = state_->clock.visualTimeMicros;
  if (const auto previous = customEventLastExecutionMicros_.find(event.id);
      previous != customEventLastExecutionMicros_.end()) {
    const __int128 elapsed = static_cast<__int128>(now) - previous->second;
    if (elapsed / 1000 < event.minimumIntervalMillis) {
      return evaluated;
    }
  }
  const auto invoked = invokeCustomEvent(event, {});
  SkinHostCallResult result{
      .status = invoked.status,
      .callbacksInvoked = evaluated.callbacksInvoked + invoked.callbacksInvoked,
      .diagnostics = diagnostics_};
  if (invoked.status != SkinHostCallStatus::Completed) {
    return result;
  }
  try {
    customEventLastExecutionMicros_.insert_or_assign(event.id, now);
  } catch (...) {
    reportDiagnostic({.code = "skin.play_state.custom_event_clock_failed",
                      .message = "Custom event clock could not be recorded."});
    result.status = SkinHostCallStatus::CriticalFailure;
  }
  result.diagnostics = diagnostics_;
  return result;
}

SkinHostCallResult PlaySkinStateBridge::invokeCustomEvent(
    const SkinCustomEvent &event, std::span<const int> arguments) {
  return invokeEventBinding(event.action, arguments);
}

SkinHostCallResult PlaySkinStateBridge::invokeEventBinding(
    SkinEventBindingId id, std::span<const int> arguments) {
  const auto binding = std::ranges::find_if(
      context_.model.model.events, [id](const SkinEventBinding &candidate) {
        return candidate.id == id;
      });
  if (binding == context_.model.model.events.end()) {
    reportDiagnostic({.code = "skin.play_state.custom_event_binding_missing",
                      .message = "Custom event action binding is absent."});
    return {.status = SkinHostCallStatus::CriticalFailure,
            .diagnostics = diagnostics_};
  }
  if (const auto *builtin =
          std::get_if<SkinBuiltinPropertySelector>(&binding->source)) {
    const auto target = numericSelector(*builtin);
    if (!target) {
      reportUnsupportedEvent(0);
      return {.status = SkinHostCallStatus::Unsupported,
              .diagnostics = diagnostics_};
    }
    return executeEvent(*target, arguments);
  }

  std::array<LuaScalar, 2> luaArguments{};
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    luaArguments[index] = static_cast<std::int64_t>(arguments[index]);
  }
  LuaCallbackResult callback;
  try {
    callback = context_.runtime.invoke(
        std::get<LuaCallbackId>(binding->source),
        std::span<const LuaScalar>{luaArguments.data(), arguments.size()});
  } catch (...) {
    reportDiagnostic({.code = "skin.play_state.custom_event_callback_failed",
                      .message = "Custom event callback failed within host limits."});
    return {.status = SkinHostCallStatus::CriticalFailure,
            .callbacksInvoked = 1,
            .diagnostics = diagnostics_};
  }
  if (callback.failure) {
    return callbackFailure(std::move(*callback.failure));
  }
  return {.callbacksInvoked = 1, .diagnostics = diagnostics_};
}

SkinHostCallResult PlaySkinStateBridge::evaluateCustomCondition(
    SkinBooleanPropertyId id, bool &condition) {
  const auto binding = std::ranges::find_if(
      context_.model.model.booleanProperties,
      [id](const SkinBooleanPropertyBinding &candidate) {
        return candidate.id == id;
      });
  if (binding == context_.model.model.booleanProperties.end()) {
    reportDiagnostic({.code = "skin.play_state.custom_event_condition_missing",
                      .message = "Custom event condition binding is absent."});
    return {.status = SkinHostCallStatus::CriticalFailure,
            .diagnostics = diagnostics_};
  }
  if (const auto *builtin =
          std::get_if<SkinBuiltinPropertySelector>(&binding->source)) {
    const auto value = booleanProperty(*builtin);
    if (!value.supported) {
      return {.status = SkinHostCallStatus::CriticalFailure,
              .diagnostics = diagnostics_};
    }
    condition = value.value;
    return {.diagnostics = diagnostics_};
  }
  LuaCallbackResult callback;
  try {
    callback = context_.runtime.invoke(std::get<LuaCallbackId>(binding->source), {});
  } catch (...) {
    reportDiagnostic({.code = "skin.play_state.custom_event_condition_failed",
                      .message = "Custom event condition failed within host limits."});
    return {.status = SkinHostCallStatus::CriticalFailure,
            .callbacksInvoked = 1,
            .diagnostics = diagnostics_};
  }
  if (callback.failure) {
    return callbackFailure(std::move(*callback.failure));
  }
  if (!callback.value || !std::holds_alternative<bool>(*callback.value)) {
    reportDiagnostic({.code = "skin.play_state.custom_event_condition_type",
                      .message = "Custom event condition did not return a boolean."});
    return {.status = SkinHostCallStatus::CriticalFailure,
            .callbacksInvoked = 1,
            .diagnostics = diagnostics_};
  }
  condition = std::get<bool>(*callback.value);
  return {.callbacksInvoked = 1, .diagnostics = diagnostics_};
}

SkinHostCallResult PlaySkinStateBridge::evaluateCustomTimer(
    SkinTimerPropertyId id, std::int64_t &value) {
  const auto binding = std::ranges::find_if(
      context_.model.model.timerProperties,
      [id](const SkinTimerPropertyBinding &candidate) { return candidate.id == id; });
  if (binding == context_.model.model.timerProperties.end()) {
    reportDiagnostic({.code = "skin.play_state.custom_timer_binding_missing",
                      .message = "Custom timer binding is absent."});
    return {.status = SkinHostCallStatus::CriticalFailure,
            .diagnostics = diagnostics_};
  }
  if (const auto *builtin =
          std::get_if<SkinBuiltinPropertySelector>(&binding->source)) {
    value = timerProperty(*builtin);
    return {.diagnostics = diagnostics_};
  }
  LuaCallbackResult callback;
  try {
    callback = context_.runtime.invoke(std::get<LuaCallbackId>(binding->source), {});
  } catch (...) {
    reportDiagnostic({.code = "skin.play_state.custom_timer_callback_failed",
                      .message = "Custom timer callback failed within host limits."});
    return {.status = SkinHostCallStatus::CriticalFailure,
            .callbacksInvoked = 1,
            .diagnostics = diagnostics_};
  }
  if (callback.failure) {
    return callbackFailure(std::move(*callback.failure));
  }
  const auto numeric = callback.value
                           ? std::visit(
                                 [](const auto &candidate) -> std::optional<std::int64_t> {
                                   using Candidate = std::decay_t<decltype(candidate)>;
                                   if constexpr (std::is_same_v<Candidate, std::int64_t>) {
                                     return candidate;
                                   } else if constexpr (std::is_same_v<Candidate, double>) {
                                     if (std::isfinite(candidate) &&
                                         candidate >= static_cast<double>(INT64_MIN) &&
                                         candidate <= static_cast<double>(INT64_MAX)) {
                                       return static_cast<std::int64_t>(candidate);
                                     }
                                   }
                                   return std::nullopt;
                                 },
                                 *callback.value)
                           : std::nullopt;
  if (!numeric) {
    reportDiagnostic({.code = "skin.play_state.custom_timer_type",
                      .message = "Custom timer callback did not return an integer."});
    return {.status = SkinHostCallStatus::CriticalFailure,
            .callbacksInvoked = 1,
            .diagnostics = diagnostics_};
  }
  value = *numeric;
  return {.callbacksInvoked = 1, .diagnostics = diagnostics_};
}

SkinHostCallResult PlaySkinStateBridge::callbackFailure(SkinDiagnostic failure) {
  const std::string code = failure.code;
  reportDiagnostic(std::move(failure));
  const bool budgetExceeded =
      code == "skin_lua_frame_budget_exceeded" ||
      code == "skin_lua_instruction_limit_exceeded" ||
      code == "skin_lua_wall_time_limit_exceeded";
  return {.status = budgetExceeded ? SkinHostCallStatus::BudgetExceeded
                                   : SkinHostCallStatus::CriticalFailure,
          .callbacksInvoked = 1,
          .diagnostics = diagnostics_};
}

void PlaySkinStateBridge::rollbackFrameWrites() noexcept {
  staged_.orderedMutations.clear();
}

SkinHostCallResult PlaySkinStateBridge::invokeWriter(
    SkinFloatWriterId writerId, double normalizedValue) {
  if (phase_ != FramePhase::Active) {
    reportDiagnostic({.code = "skin.play_state.frame_inactive",
                      .message = "Skin writers require an active frame."});
    return {.status = SkinHostCallStatus::CriticalFailure,
            .diagnostics = diagnostics_};
  }
  if (!std::isfinite(normalizedValue)) {
    reportDiagnostic(
        {.code = "skin.play_state.writer_value_nonfinite",
         .message = "Skin writer input must be finite before clamping."});
    return {.status = SkinHostCallStatus::CriticalFailure,
            .diagnostics = diagnostics_};
  }
  if (writerInvocationActive_) {
    reportDiagnostic(
        {.code = "skin.play_state.writer_reentrant",
         .message = "A skin writer callback is already active."});
    return {.status = SkinHostCallStatus::CriticalFailure,
            .diagnostics = diagnostics_};
  }

  const auto binding = std::ranges::find_if(
      context_.model.model.floatWriters,
      [writerId](const SkinFloatWriterBinding &candidate) {
        return candidate.id == writerId;
      });
  if (binding == context_.model.model.floatWriters.end()) {
    reportDiagnostic(
        {.code = "skin.play_state.writer_missing",
         .message = "Skin writer ID is not present in the validated model.",
         .virtualPath = std::to_string(writerId.value)});
    return {.status = SkinHostCallStatus::Unsupported,
            .diagnostics = diagnostics_};
  }
  if (std::holds_alternative<SkinBuiltinPropertySelector>(binding->source)) {
    reportDiagnostic(
        {.code = "skin.play_state.writer_builtin_unsupported",
         .message = "No built-in gameplay writer is allowlisted for this "
                    "skin surface.",
         .virtualPath = std::to_string(writerId.value)});
    return {.status = SkinHostCallStatus::Unsupported,
            .diagnostics = diagnostics_};
  }

  const std::size_t savepoint = staged_.orderedMutations.size();
  writerInvocationActive_ = true;
  LuaCallbackResult callback;
  try {
    const std::array<LuaScalar, 1> arguments{
        LuaScalar{std::clamp(normalizedValue, 0.0, 1.0)}};
    callback = context_.runtime.invoke(
        std::get<LuaCallbackId>(binding->source), arguments);
  } catch (...) {
    writerInvocationActive_ = false;
    staged_.orderedMutations.resize(savepoint);
    reportDiagnostic({.code = "skin.play_state.writer_callback_failed",
                      .message = "Skin writer callback failed within host "
                                 "limits."});
    return {.status = SkinHostCallStatus::CriticalFailure,
            .callbacksInvoked = 1,
            .diagnostics = diagnostics_};
  }
  writerInvocationActive_ = false;
  if (callback.failure) {
    staged_.orderedMutations.resize(savepoint);
    const std::string code = callback.failure->code;
    reportDiagnostic(std::move(*callback.failure));
    const bool budgetExceeded =
        code == "skin_lua_frame_budget_exceeded" ||
        code == "skin_lua_instruction_limit_exceeded" ||
        code == "skin_lua_wall_time_limit_exceeded";
    return {.status = budgetExceeded ? SkinHostCallStatus::BudgetExceeded
                                     : SkinHostCallStatus::CriticalFailure,
            .callbacksInvoked = 1,
            .diagnostics = diagnostics_};
  }
  return {.callbacksInvoked = 1, .diagnostics = diagnostics_};
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
  case 172:
  case 173: {
    const bool hasLongNote = std::ranges::any_of(
        context_.chartModel.notes, [](const ChartVisualNote &note) {
          return note.kind == ChartVisualNoteKind::LongHead ||
                 note.kind == ChartVisualNoteKind::LongBody ||
                 note.kind == ChartVisualNoteKind::LongTail;
        });
    return {.value = *id == 172 ? !hasLongNote : hasLongNote,
            .supported = true};
  }
  case 150:
    return {.value = context_.chartModel.staticMetadata.difficulty <= 0 ||
                     context_.chartModel.staticMetadata.difficulty > 5,
            .supported = true};
  case 151:
  case 152:
  case 153:
  case 154:
  case 155:
    return {.value = context_.chartModel.staticMetadata.difficulty == *id - 150,
            .supported = true};
  case 170:
  case 171:
    return {.value = *id == 171
                         ? context_.chartModel.staticMetadata.hasBga
                         : !context_.chartModel.staticMetadata.hasBga,
            .supported = true};
  case 176:
  case 177: {
    const auto &metadata = context_.chartModel.staticMetadata;
    return {.value = *id == 176 ? metadata.minimumBpm == metadata.maximumBpm
                                 : metadata.minimumBpm < metadata.maximumBpm,
            .supported = true};
  }
  case 180: {
    const int judgeRank = context_.chartModel.staticMetadata.judgeRank;
    return {.value = judgeRank == 0 || (judgeRank >= 10 && judgeRank < 35),
            .supported = true};
  }
  case 181: {
    const int judgeRank = context_.chartModel.staticMetadata.judgeRank;
    return {.value = judgeRank == 1 || (judgeRank >= 35 && judgeRank < 60),
            .supported = true};
  }
  case 182: {
    const int judgeRank = context_.chartModel.staticMetadata.judgeRank;
    return {.value = judgeRank == 2 || (judgeRank >= 60 && judgeRank < 85),
            .supported = true};
  }
  case 183: {
    const int judgeRank = context_.chartModel.staticMetadata.judgeRank;
    return {.value = judgeRank == 3 || (judgeRank >= 85 && judgeRank < 110),
            .supported = true};
  }
  case 184: {
    const int judgeRank = context_.chartModel.staticMetadata.judgeRank;
    return {.value = judgeRank == 4 || judgeRank >= 110, .supported = true};
  }
  case 190:
  case 191:
    return {.value = *id == 191
                         ? !context_.chartModel.staticMetadata.stageFilePath.empty()
                         : context_.chartModel.staticMetadata.stageFilePath.empty(),
            .supported = true};
  case 194:
  case 195:
    return {.value = *id == 195
                         ? !context_.chartModel.staticMetadata.backBmpPath.empty()
                         : context_.chartModel.staticMetadata.backBmpPath.empty(),
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
  case 2243:
    return {.value = capturedJudgeCount(*snapshot, Good) > 0,
            .supported = true};
  case 2244:
    return {.value = capturedJudgeCount(*snapshot, Bad) > 0,
            .supported = true};
  case 2245:
    return {.value = capturedJudgeCount(*snapshot, Poor) > 0,
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
  case 14:
    return {.value = static_cast<std::int64_t>(
                snapshot->authority.laneCoverPercent) *
                    10,
            .supported = true};
  case 71:
  case 101:
  case 171:
    return {.value = snapshot->score, .supported = true};
  case 107:
    return {.value = static_cast<std::int64_t>(
                snapshot->authority.currentGauge),
            .supported = true};
  case 110:
    return {.value = capturedJudgeCount(*snapshot, PGreat),
            .supported = true};
  case 111:
    return {.value = capturedJudgeCount(*snapshot, Great),
            .supported = true};
  case 112:
    return {.value = capturedJudgeCount(*snapshot, Good),
            .supported = true};
  case 113:
    return {.value = capturedJudgeCount(*snapshot, Bad),
            .supported = true};
  case 114:
    return {.value = capturedJudgeCount(*snapshot, Poor),
            .supported = true};
  case 160:
    return {.value = static_cast<std::int64_t>(snapshot->authority.currentBpm),
            .supported = true};
  case 74:
    return {.value = context_.chartModel.staticMetadata.totalNotes,
            .supported = true};
  case 90:
    return {.value =
                javaDoubleToInt(context_.chartModel.staticMetadata.maximumBpm),
            .supported = true};
  case 91:
    return {.value =
                javaDoubleToInt(context_.chartModel.staticMetadata.minimumBpm),
            .supported = true};
  case 92:
    return {.value =
                javaDoubleToInt(context_.chartModel.staticMetadata.mainBpm),
            .supported = true};
  case 96:
    return {.value = context_.chartModel.staticMetadata.playLevel,
            .supported = true};
  case 350:
    return {.value = context_.chartModel.staticMetadata.normalKeyNotes,
            .supported = true};
  case 351:
    return {.value = context_.chartModel.staticMetadata.longKeyNotes,
            .supported = true};
  case 352:
    return {.value = context_.chartModel.staticMetadata.normalScratchNotes,
            .supported = true};
  case 353:
    return {.value = context_.chartModel.staticMetadata.longScratchNotes,
            .supported = true};
  case 1163:
    return {.value = (context_.chartModel.staticMetadata.durationMicros /
                      1'000'000 / 60) %
                     60,
            .supported = true};
  case 1164:
    return {.value = (context_.chartModel.staticMetadata.durationMicros /
                      1'000'000) %
                     60,
            .supported = true};
  case 407: {
    const float gauge = snapshot->authority.currentGauge;
    const int tenths =
        gauge > 0.0F && gauge < 0.1F ? 1 : static_cast<int>(gauge * 10.0F);
    return {.value = tenths % 10, .supported = true};
  }
  case 427:
    return {.value = capturedJudgeCount(*snapshot, Bad) +
                     capturedJudgeCount(*snapshot, Poor) +
                     capturedJudgeCount(*snapshot, Kpoor),
            .supported = true};
  case 525:
    return {.value = snapshot->fastSlowMicros, .supported = true};
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
    if (!snapshot->authority.laneCoverEnabled) {
      return {.value = 0.0, .supported = true};
    }
    double laneCover =
        static_cast<double>(snapshot->authority.laneCoverPercent) / 100.0;
    if (snapshot->authority.liftEnabled) {
      laneCover *= 1.0 - static_cast<double>(snapshot->authority.liftRatio);
    }
    return {.value = laneCover, .supported = true};
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
  if (const auto custom = customTimerValues_.find(*id);
      custom != customTimerValues_.end()) {
    return custom->second;
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
    context_.runtime.setEventExecutor({});
    context_.runtime.setFrameState(nullptr);
    runtimeBound_ = false;
  }
  phase_ = FramePhase::Closed;
  customObjectsUpdated_ = false;
  writerInvocationActive_ = false;
  state_.reset();
  frameSerial_ = 0;
  projection_ = {};
  staged_ = {};
  customTimerValues_.clear();
}

LuaSkinEventExecutionResult PlaySkinStateBridge::executeHostEvent(
    void *opaque, int eventId, std::span<const int> arguments) noexcept {
  if (opaque == nullptr) {
    return {.failure = SkinDiagnostic{
                .code = "skin_lua_event_executor_unavailable",
                .message = "Skin event executor has no active bridge."}};
  }
  auto &bridge = *static_cast<PlaySkinStateBridge *>(opaque);
  try {
    const auto result = bridge.executeEvent(eventId, arguments);
    if (result.status == SkinHostCallStatus::Completed) {
      return {};
    }
    if (!result.diagnostics.empty()) {
      return {.failure = result.diagnostics.back()};
    }
    return {.failure = SkinDiagnostic{
                .code = "skin_lua_event_execution_failed",
                .message = "Skin event execution was rejected."}};
  } catch (...) {
    return {.failure = SkinDiagnostic{
                .code = "skin_lua_event_execution_failed",
                .message = "Skin event execution failed within host limits."}};
  }
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
