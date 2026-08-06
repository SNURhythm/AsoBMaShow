#include "PlaySkinStateBridge.h"

#include "GameplaySkinBuiltinCatalog.h"
#include "LuaSkinHostModules.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <type_traits>
#include <tuple>
#include <utility>

namespace {

std::int64_t capturedJudgeCount(const PlayfieldVisualState &snapshot,
                                Judgement judgement) {
  const auto found = snapshot.authority.judgementCounters.find(judgement);
  return found == snapshot.authority.judgementCounters.end() ? 0
                                                              : found->second;
}

std::int64_t capturedJudgeFastSlowCount(const PlayfieldVisualState &snapshot,
                                        Judgement judgement, bool fast) {
  const auto found = snapshot.authority.judgementFastSlowCounters.find(judgement);
  if (found == snapshot.authority.judgementFastSlowCounters.end()) {
    return 0;
  }
  return fast ? found->second.fast : found->second.slow;
}

double scoreRate(int score, int notes) {
  return notes == 0 ? 1.0
                    : static_cast<double>(static_cast<float>(score) /
                                          static_cast<float>(notes * 2));
}

int targetScore(const PlayfieldVisualState &snapshot) {
  return snapshot.authority.pacemakerTarget.enabled
             ? snapshot.authority.pacemakerTarget.finalScore
             : 0;
}

std::int64_t beatorajaKeyJudgeValue(const PlayfieldVisualState &snapshot,
                                    int selector) {
  // SkinPropertyMapper maps 500-519 as two groups of ten: player then key.
  // Gameplay is currently single-player, so the absent 2P group follows
  // JudgeManager.getJudge() and reports -1.
  const int player = (selector - 500) / 10;
  const int key = (selector - 500) % 10;
  if (player != 0 || key < 0 ||
      static_cast<std::size_t>(key) >= snapshot.lanes.size()) {
    return -1;
  }
  return snapshot.lanes[static_cast<std::size_t>(key)].beatorajaJudgeValue;
}

std::optional<int> decimalSuffix(std::string_view value, std::string_view prefix,
                                 std::string_view suffix, int first,
                                 int count, int authoredFirst = 1) {
  if (!value.starts_with(prefix) || !value.ends_with(suffix) ||
      value.size() <= prefix.size() + suffix.size()) {
    return std::nullopt;
  }
  const std::string_view digits =
      value.substr(prefix.size(), value.size() - prefix.size() - suffix.size());
  int parsed = 0;
  for (const char character : digits) {
    if (character < '0' || character > '9' ||
        parsed > (std::numeric_limits<int>::max() - 9) / 10) {
      return std::nullopt;
    }
    parsed = parsed * 10 + (character - '0');
  }
  const int offset = parsed - authoredFirst;
  return offset >= 0 && offset < count ? std::optional<int>(first + offset)
                                        : std::nullopt;
}

std::optional<int> namedStringPropertySelector(std::string_view name) {
  constexpr std::array<std::pair<std::string_view, int>, 26> direct = {{
      {"rival", 1},          {"player", 2},
      {"target", 3},         {"title", 10},
      {"subtitle", 11},      {"fulltitle", 12},
      {"genre", 13},         {"artist", 14},
      {"subartist", 15},     {"fullartist", 16},
      {"searchword", 30},    {"skinname", 50},
      {"skinauthor", 51},    {"mode", 60},
      {"sort", 61},          {"difficulty", 62},
      {"chartreplication", 86}, {"directory", 1000},
      {"tablename", 1001},   {"tablelevel", 1002},
      {"tablefull", 1003},   {"version", 1010},
      {"irname", 1020},      {"irUserName", 1021},
      {"songhashmd5", 1030}, {"songhashsha256", 1031},
  }};
  for (const auto &[candidate, id] : direct) {
    if (name == candidate) {
      return id;
    }
  }
  // StringPropertyFactory reverses the numeric order of targetnamep: its
  // authored name 1 resolves to the closest previous target (numeric 209),
  // while numeric 200 is the farthest one.  Preserve that source mapping
  // rather than treating the name as a conventional ascending alias.
  if (const auto previous = decimalSuffix(name, "targetnamep", "", 0, 10)) {
    return 209 - *previous;
  }
  constexpr std::array<std::tuple<std::string_view, std::string_view, int,
                                  int, int>,
                       12>
      patterns = {{{"key", "", 40, 10, 1},
                   {"key", "", 240, 44, 11},
                   {"skincategory", "", 100, 10, 1},
                   {"skinitem", "", 110, 10, 1},
                   {"rankingname", "", 120, 10, 1},
                   {"coursetitle", "", 150, 10, 1},
                   {"targetnamen", "", 210, 10, 1},
                   {"practice_item", "", 1040, 16, 1},
                   {"practice_item", "_label", 1060, 16, 1},
                   {"practice_item", "_value", 1080, 16, 1},
                   {"practice_item_label", "", 1060, 16, 1},
                   {"practice_item_value", "", 1080, 16, 1}}};
  for (const auto &[prefix, suffix, first, count, authoredFirst] : patterns) {
    if (const auto id = decimalSuffix(name, prefix, suffix, first, count,
                                      authoredFirst)) {
      return id;
    }
  }
  return std::nullopt;
}

std::optional<int> namedFloatPropertySelector(std::string_view name) {
  constexpr std::array<std::pair<std::string_view, int>, 71> direct = {{
      {"musicselect_position", 1}, {"lanecover", 4},
      {"lanecover2", 5},            {"music_progress", 6},
      {"skinselect_position", 7},  {"ranking_position", 8},
      {"mastervolume", 17},         {"keyvolume", 18},
      {"bgmvolume", 19},            {"practice_position", 20},
      {"music_progress_bar", 101},  {"load_progress", 102},
      {"level", 103},                {"level_beginner", 105},
      {"level_normal", 106},         {"level_hyper", 107},
      {"level_another", 108},        {"level_insane", 109},
      {"scorerate", 110},           {"scorerate_final", 111},
      {"bestscorerate_now", 112},   {"bestscorerate", 113},
      {"targetscorerate_now", 114}, {"targetscorerate", 115},
      {"rate_pgreat", 140},          {"rate_great", 141},
      {"rate_good", 142},            {"rate_bad", 143},
      {"rate_poor", 144},            {"rate_maxcombo", 145},
      {"rate_exscore", 147},
      {"score_rate", 1102},         {"total_rate", 1115},
      {"score_rate2", 155},         {"duration_average", 372},
      {"timing_average", 374},       {"timign_stddev", 376},
      {"perfect_rate", 85},          {"great_rate", 86},
      {"good_rate", 87},             {"bad_rate", 88},
      {"poor_rate", 89},             {"rival_perfect_rate", 285},
      {"rival_great_rate", 286},     {"rival_good_rate", 287},
      {"rival_bad_rate", 288},       {"rival_poor_rate", 289},
      {"best_rate", 183},            {"rival_rate", 122},
      {"target_rate", 135},          {"target_rate2", 157},
      {"hispeed", 310},              {"groovegauge_1p", 1107},
      {"chart_averagedensity", 367}, {"chart_enddensity", 362},
      {"chart_peakdensity", 360},    {"chart_totalgauge", 368},
      {"loading_progress", 165},     {"ir_totalclearrate", 227},
      {"ir_totalfullcomborate", 229},
      {"ir_player_noplay_rate", 203}, {"ir_player_failed_rate", 211},
      {"ir_player_assist_rate", 205},
      {"ir_player_lightassist_rate", 207},
      {"ir_player_easy_rate", 213}, {"ir_player_normal_rate", 215},
      {"ir_player_hard_rate", 217}, {"ir_player_exhard_rate", 209},
      {"ir_player_fullcombo_rate", 219},
      {"ir_player_perfect_rate", 223},
      {"ir_player_max_rate", 225},
  }};
  for (const auto &[candidate, id] : direct) {
    if (name == candidate) {
      return id;
    }
  }
  return std::nullopt;
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

std::pair<std::int64_t, std::int64_t> scoreRateParts(double rate) {
  // ScoreDataProperty stores these intermediate values as Java float before
  // applying the integer cast, so preserve the single-precision products.
  const auto javaRate = static_cast<float>(rate);
  const auto scaled = javaDoubleToInt(static_cast<float>(javaRate * 100.0F));
  const auto afterDot =
      javaDoubleToInt(static_cast<float>(javaRate * 10'000.0F)) % 100;
  return {scaled, afterDot};
}

std::int64_t javaLongSubtract(std::int64_t left, std::int64_t right) {
  const auto bits = static_cast<std::uint64_t>(left) -
                    static_cast<std::uint64_t>(right);
  return std::bit_cast<std::int64_t>(bits);
}

std::int32_t javaLongToInt(std::int64_t value) {
  return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value));
}

std::int64_t javaMathRoundToInt(double value) {
  if (std::isnan(value)) {
    return 0;
  }
  if (value >= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
    return javaLongToInt(std::numeric_limits<std::int64_t>::max());
  }
  if (value <= static_cast<double>(std::numeric_limits<std::int64_t>::min())) {
    return javaLongToInt(std::numeric_limits<std::int64_t>::min());
  }
  return javaLongToInt(static_cast<std::int64_t>(std::floor(value + 0.5)));
}

std::int32_t javaIntSubtract(std::int32_t left, std::int32_t right) {
  const auto bits = static_cast<std::uint32_t>(left) -
                    static_cast<std::uint32_t>(right);
  return std::bit_cast<std::int32_t>(bits);
}

std::int32_t javaIntAdd(std::int32_t left, std::int32_t right) {
  const auto bits = static_cast<std::uint32_t>(left) +
                    static_cast<std::uint32_t>(right);
  return std::bit_cast<std::int32_t>(bits);
}

std::int64_t playTimerElapsedMillis(const PlayfieldVisualState &snapshot) {
  if (!snapshot.clock.playTimer.active) {
    return 0;
  }
  return javaLongSubtract(snapshot.clock.gameplayTimeMicros,
                          snapshot.clock.playTimer.startMicros) /
         1000;
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
  builtInTraversal_ = projection.builtInTraversal;
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

  if (context_.model == nullptr ||
      (context_.model->model.customTimers.empty() &&
       context_.model->model.customEvents.empty())) {
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
  for (const auto &timer : context_.model->model.customTimers) {
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
  for (const auto &event : context_.model->model.customEvents) {
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

  if (context_.model != nullptr) {
    if (const auto event = std::ranges::find_if(
            context_.model->model.customEvents,
            [eventId](const SkinCustomEvent &candidate) {
              return candidate.id == eventId;
            }); event != context_.model->model.customEvents.end()) {
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
  if (context_.model == nullptr) {
    reportDiagnostic({.code = "skin.play_state.model_unavailable",
                      .message = "Custom event bindings require the decoded "
                                 "gameplay skin model."});
    return {.status = SkinHostCallStatus::CriticalFailure,
            .diagnostics = diagnostics_};
  }
  const auto binding = std::ranges::find_if(
      context_.model->model.events, [id](const SkinEventBinding &candidate) {
        return candidate.id == id;
      });
  if (binding == context_.model->model.events.end()) {
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
  if (context_.model == nullptr) {
    reportDiagnostic({.code = "skin.play_state.model_unavailable",
                      .message = "Custom conditions require the decoded "
                                 "gameplay skin model."});
    return {.status = SkinHostCallStatus::CriticalFailure,
            .diagnostics = diagnostics_};
  }
  const auto binding = std::ranges::find_if(
      context_.model->model.booleanProperties,
      [id](const SkinBooleanPropertyBinding &candidate) {
        return candidate.id == id;
      });
  if (binding == context_.model->model.booleanProperties.end()) {
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
  if (context_.model == nullptr) {
    reportDiagnostic({.code = "skin.play_state.model_unavailable",
                      .message = "Custom timers require the decoded gameplay "
                                 "skin model."});
    return {.status = SkinHostCallStatus::CriticalFailure,
            .diagnostics = diagnostics_};
  }
  const auto binding = std::ranges::find_if(
      context_.model->model.timerProperties,
      [id](const SkinTimerPropertyBinding &candidate) { return candidate.id == id; });
  if (binding == context_.model->model.timerProperties.end()) {
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

  if (context_.model == nullptr) {
    reportDiagnostic({.code = "skin.play_state.model_unavailable",
                      .message = "Skin writers require the decoded gameplay "
                                 "skin model."});
    return {.status = SkinHostCallStatus::Unsupported,
            .diagnostics = diagnostics_};
  }
  const auto binding = std::ranges::find_if(
      context_.model->model.floatWriters,
      [writerId](const SkinFloatWriterBinding &candidate) {
        return candidate.id == writerId;
      });
  if (binding == context_.model->model.floatWriters.end()) {
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
  if (*id < 0) {
    if (*id == std::numeric_limits<int>::min()) {
      reportUnsupported("boolean", selector);
      return {};
    }
    const auto positive = booleanProperty({-*id});
    if (!positive.supported) {
      return {};
    }
    return {.value = !positive.value, .supported = true};
  }
  switch (*id) {
  case 32:
    // AsoBMaShow has no autoplay play mode. Its regular gameplay session is
    // exactly BooleanPropertyFactory's non-autoplay branch.
    return {.value = true, .supported = true};
  case 33:
    return {.value = false, .supported = true};
  case 42:
    return {.value = gaugeTypeIndex(snapshot->authority.gaugeType) <= 2,
            .supported = true};
  case 43:
    return {.value = gaugeTypeIndex(snapshot->authority.gaugeType) >= 3,
            .supported = true};
  case 80:
    return {.value = snapshot->authority.loadingState ==
                         PlayfieldLoadingState::Loading,
            .supported = true};
  case 81:
    return {.value = snapshot->authority.loadingState ==
                         PlayfieldLoadingState::Loaded,
            .supported = true};
  case 84:
    return {.value = snapshot->authority.gameplayMode ==
                         PlayfieldGameplayMode::Replay,
            .supported = true};
  case 400:
    // BooleanPropertyFactory's OPTION_CONSTANT returns false outside a
    // player/selector with an enabled constant play config. AsoBMaShow has no
    // corresponding play-config mode, so its authoritative state is false.
    return {.value = false, .supported = true};
  case 271:
    return {.value = snapshot->authority.laneCoverEnabled, .supported = true};
  case 272:
    return {.value = snapshot->authority.liftEnabled, .supported = true};
  case 273:
    return {.value = snapshot->authority.hiddenEnabled, .supported = true};
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
      return {.value = false, .supported = true};
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
  case 1080:
    return {.value = snapshot->authority.gameplayMode ==
                         PlayfieldGameplayMode::Practice,
            .supported = true};
  default:
    break;
  }
  if (*id >= 230 && *id <= 240) {
    const auto gauge = gaugeState();
    if (!gauge.supported || !std::isfinite(gauge.maximum) ||
        gauge.maximum <= 0.0) {
      return {.value = false, .supported = true};
    }
    const int range = *id - 230;
    const double low = static_cast<double>(range) * 0.1 * gauge.maximum;
    const double high = static_cast<double>(range + 1) * 0.1 * gauge.maximum;
    return {.value = gauge.value >= low && gauge.value < high,
            .supported = true};
  }
  if (isPinnedBeatorajaBooleanPropertyId(*id)) {
    // Beatoraja's implementation makes each official property callable in
    // every MainState; most non-gameplay branches simply return false. Keep
    // that behavior for source families AsoBMaShow does not provide yet.
    return {.value = false, .supported = true};
  }
  reportUnsupported("boolean", selector);
  return {};
}

SkinPropertyLookup<std::int64_t> PlaySkinStateBridge::integerProperty(
    const SkinBuiltinPropertySelector &selector, SkinIntegerPropertyDomain domain) {
  const auto *snapshot = state();
  auto id = numericSelector(selector);
  if (!id && selector.value == decltype(selector.value){std::string{"nowbpm"}}) {
    id = 160;
  }
  if (snapshot == nullptr || !id) {
    reportUnsupported("integer", selector);
    return {};
  }
  if (domain == SkinIntegerPropertyDomain::ImageIndex) {
    // IntegerPropertyFactory.getImageIndexProperty is a distinct factory from
    // getIntegerProperty.  Keep selectors shared by both factories (for
    // example 90) out of this value-domain switch.
    if (*id >= 500 && *id <= 519) {
      return {.value = beatorajaKeyJudgeValue(*snapshot, *id),
              .supported = true};
    }
    if (*id == 308) {
      const auto &metadata = context_.chartModel.staticMetadata;
      if (metadata.hasAnyLongNote && !metadata.hasUndefinedLongNote) {
        if (metadata.hasLongNote) {
          return {.value = 0, .supported = true};
        }
        if (metadata.hasChargeNote) {
          return {.value = 1, .supported = true};
        }
        return {.value = 2, .supported = true};
      }
      // PlayerConfig.getLnmode() is one-based (LN/CN/HCN); IndexType.lnmode
      // exposes the zero-based skin value only for a chart with a declared
      // long-note type.  For undefined/no-long-note charts it returns that
      // configuration value directly.
      return {.value = metadata.selectedLongNoteMode,
              .supported = true};
    }
    reportUnsupported("image_index", selector);
    return {};
  }
  if (domain != SkinIntegerPropertyDomain::IntegerValue) {
    reportUnsupported("integer", selector);
    return {};
  }
  const auto durationValue = [&]() -> std::optional<std::int64_t> {
    if (*id != 312 && *id != 313 && (*id < 1312 || *id > 1327)) {
      return std::nullopt;
    }
    if (!builtInTraversal_ ||
        !std::isfinite(static_cast<double>(builtInTraversal_->hispeed)) ||
        builtInTraversal_->hispeed <= 0.0F) {
      return std::nullopt;
    }
    const auto durationFor = [&](double bpm, bool cover, bool green)
        -> std::optional<std::int64_t> {
      if (!std::isfinite(bpm) || bpm <= 0.0) {
        return std::nullopt;
      }
      double value = 240000.0 / bpm /
                     static_cast<double>(builtInTraversal_->hispeed);
      if (cover) {
        value *= 1.0 -
                 static_cast<double>(snapshot->authority.laneCoverPercent) /
                     100.0;
      }
      if (green) {
        value *= 0.6;
      }
      return javaMathRoundToInt(value);
    };
    if (*id == 312 || *id == 313) {
      // LaneRenderer.currentduration rounds its current speed/cover product.
      // ValueType.duration_green then applies its integer `* 3 / 5`
      // conversion afterwards.
      const auto current = durationFor(
          snapshot->authority.currentBpm, snapshot->authority.laneCoverEnabled,
          false);
      if (!current) {
        return std::nullopt;
      }
      return *id == 312 ? *current : (*current * 3) / 5;
    }
    const int relative = *id - 1312;
    const int bpmMode = relative / 4;
    const bool green = relative % 2 == 1;
    const bool cover = relative % 4 < 2;
    const double bpm = [&] {
      switch (bpmMode) {
      case 0:
        return snapshot->authority.currentBpm;
      case 1:
        return context_.chartModel.staticMetadata.mainBpm;
      case 2:
        return context_.chartModel.staticMetadata.minimumBpm;
      default:
        return context_.chartModel.staticMetadata.maximumBpm;
      }
    }();
    // IntegerPropertyFactory.createDurationLanecoverProperty uses the raw
    // PlayConfig lane-cover value for this family, independently of whether
    // the lane cover is currently enabled for current-duration rendering.
    return durationFor(bpm, cover, green);
  };
  if (const auto duration = durationValue()) {
    return {.value = *duration, .supported = true};
  }
  switch (*id) {
  case 14:
    return {.value = static_cast<std::int64_t>(
                snapshot->authority.laneCoverPercent) *
                    10,
            .supported = true};
  case 314:
    return {.value = javaDoubleToInt(snapshot->authority.liftRatio * 1000.0F),
            .supported = true};
  case 315:
    return {.value =
                javaDoubleToInt(snapshot->authority.hiddenRatio * 1000.0F),
            .supported = true};
  case 316: {
    const double laneCover =
        static_cast<double>(snapshot->authority.laneCoverPercent) / 100.0;
    return {.value = javaDoubleToInt(
                (1.0 - static_cast<double>(snapshot->authority.liftRatio)) *
                laneCover * 1000.0),
            .supported = true};
  }
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
  case 161:
  case 162: {
    if (!snapshot->clock.playTimer.elapsedMillisExact) {
      return {};
    }
    const std::int32_t elapsed = javaLongToInt(playTimerElapsedMillis(*snapshot));
    return {.value = *id == 161 ? elapsed / 60'000
                                : (elapsed / 1000) % 60,
            .supported = true};
  }
  case 163:
  case 164: {
    if (!snapshot->clock.playTimer.elapsedMillisExact ||
        !snapshot->clock.playTimer.playtimeMillis.has_value()) {
      return {};
    }
    const std::int32_t elapsed = javaLongToInt(playTimerElapsedMillis(*snapshot));
    const std::int32_t remaining = std::max(
        javaIntAdd(javaIntSubtract(*snapshot->clock.playTimer.playtimeMillis,
                                   elapsed),
                   1000),
        std::int32_t{0});
    return {.value = *id == 163 ? remaining / 60'000
                                : (remaining / 1000) % 60,
            .supported = true};
  }
  case 74:
  case 106:
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
  case 75:
  case 105:
    return {.value = snapshot->authority.maximumCombo, .supported = true};
  case 102:
  case 103: {
    const auto [integer, fractional] = scoreRateParts(scoreRate(
        snapshot->score, snapshot->authority.pacemakerStatus.playedNotes));
    return {.value = *id == 102 ? integer : fractional, .supported = true};
  }
  case 152:
    return {.value = static_cast<std::int64_t>(snapshot->score) -
                    snapshot->authority.bestScore,
            .supported = true};
  case 153:
    return {.value = static_cast<std::int64_t>(snapshot->score) -
                    targetScore(*snapshot),
            .supported = true};
  case 360:
  case 361:
  case 362:
  case 363:
  case 364:
  case 365:
  case 368:
    // IntegerPropertyFactory reads SongInformation for these selectors and
    // returns Integer.MIN_VALUE when it is absent. Aso does not retain that
    // optional analysis object in the immutable chart state.
    return {.value = std::numeric_limits<int>::min(), .supported = true};
  case 410:
  case 411:
  case 412:
  case 413:
  case 414:
  case 415:
  case 416:
  case 417:
  case 418:
  case 419:
  case 421:
  case 422: {
    const bool fast = *id == 410 || *id == 412 || *id == 414 ||
                      *id == 416 || *id == 418 || *id == 421;
    const Judgement judgement = [&] {
      switch (*id) {
      case 410:
      case 411:
        return PGreat;
      case 412:
      case 413:
        return Great;
      case 414:
      case 415:
        return Good;
      case 416:
      case 417:
        return Bad;
      case 418:
      case 419:
        return Poor;
      default:
        return Kpoor;
      }
    }();
    return {.value = capturedJudgeFastSlowCount(*snapshot, judgement, fast),
            .supported = true};
  }
  case 420:
    return {.value = capturedJudgeCount(*snapshot, Kpoor), .supported = true};
  case 425:
    return {.value = snapshot->authority.comboBreak, .supported = true};
  case 525:
    return {.value = snapshot->fastSlowMicros, .supported = true};
  case 526:
  case 527:
    return {.value = 0, .supported = true};
  default:
    break;
  }
  reportUnsupported("integer", selector);
  return {};
}

SkinPropertyLookup<double> PlaySkinStateBridge::floatProperty(
    const SkinBuiltinPropertySelector &selector, SkinFloatPropertyDomain domain) {
  const auto *snapshot = state();
  auto id = numericSelector(selector);
  if (!id) {
    if (const auto *name = std::get_if<std::string>(&selector.value)) {
      id = namedFloatPropertySelector(*name);
    }
  }
  if (snapshot == nullptr || !id) {
    reportUnsupported("float", selector);
    return {};
  }
  const int totalNotes = context_.chartModel.staticMetadata.totalNotes;
  const int playedNotes = snapshot->authority.pacemakerStatus.playedNotes;
  const auto currentFullRate = scoreRate(snapshot->score, totalNotes);
  const auto currentRate = scoreRate(snapshot->score, playedNotes);
  const auto bestFullRate =
      scoreRate(snapshot->authority.bestScore, totalNotes);
  const auto targetFullRate = scoreRate(targetScore(*snapshot), totalNotes);
  const auto partialRate = [&](int score) {
    if (totalNotes == 0) {
      return 0.0;
    }
    return static_cast<double>(static_cast<float>(score) * playedNotes /
                               static_cast<float>(totalNotes * totalNotes * 2));
  };
  if (domain == SkinFloatPropertyDomain::FloatValue) {
    const double floatMinimum =
        static_cast<double>(std::numeric_limits<float>::min());
    switch (*id) {
    case 1102:
      return {.value = currentRate, .supported = true};
    case 1115:
    case 155:
      return {.value = currentFullRate, .supported = true};
    case 85:
    case 86:
    case 87:
    case 88:
    case 89: {
      if (totalNotes <= 0) {
        return {.value = floatMinimum, .supported = true};
      }
      const Judgement judgement = *id == 85   ? PGreat
                                  : *id == 86 ? Great
                                  : *id == 87 ? Good
                                  : *id == 88 ? Bad
                                              : Poor;
      return {.value = static_cast<double>(capturedJudgeCount(*snapshot, judgement)) /
                           static_cast<double>(totalNotes),
              .supported = true};
    }
    case 183:
      return {.value = bestFullRate, .supported = true};
    case 122:
    case 135:
    case 157:
      return {.value = targetFullRate, .supported = true};
    case 1107:
      return {.value = snapshot->authority.currentGauge, .supported = true};
    case 360:
    case 362:
    case 367:
    case 368:
    case 372:
    case 374:
    case 376:
    case 203:
    case 205:
    case 207:
    case 209:
    case 211:
    case 213:
    case 215:
    case 217:
    case 219:
    case 223:
    case 225:
    case 227:
    case 229:
      return {.value = floatMinimum, .supported = true};
    default:
      break;
    }
    // FloatPropertyFactory.getFloatProperty falls through to RateType after
    // its FloatType and pattern tables, so continue with the rate switch.
  } else if (domain != SkinFloatPropertyDomain::Rate) {
    reportUnsupported("float", selector);
    return {};
  }
  switch (*id) {
  case 102:
    return {.value = snapshot->authority.loadingState ==
                         PlayfieldLoadingState::Loaded
                         ? 1.0
                         : 0.0,
            .supported = true};
  case 110:
    return {.value = currentFullRate, .supported = true};
  case 111:
    return {.value = currentRate, .supported = true};
  case 112:
    return {.value = partialRate(snapshot->authority.bestScore),
            .supported = true};
  case 113:
    return {.value = bestFullRate, .supported = true};
  case 114:
    return {.value = partialRate(targetScore(*snapshot)), .supported = true};
  case 115:
    return {.value = targetFullRate, .supported = true};
  // These RateType values only have an implementation in music selection or
  // skin configuration.  FloatPropertyFactory returns zero for BMSPlayer,
  // which is the authoritative gameplay-state fallback.
  case 1:
  case 7:
  case 8:
  case 103:
  case 105:
  case 106:
  case 107:
  case 108:
  case 109:
  case 140:
  case 141:
  case 142:
  case 143:
  case 144:
  case 145:
  case 147:
    return {.value = 0.0, .supported = true};
  default:
    break;
  }
  if (*id == 6) {
    if (!snapshot->clock.playTimer.elapsedMillisExact) {
      return {};
    }
    if (!snapshot->clock.playTimer.active) {
      return {.value = 0.0, .supported = true};
    }
    if (!snapshot->clock.playTimer.playtimeMillis.has_value()) {
      return {};
    }
    const float progress =
        static_cast<float>(playTimerElapsedMillis(*snapshot)) /
        static_cast<float>(*snapshot->clock.playTimer.playtimeMillis);
    return {.value = std::min(progress, 1.0F), .supported = true};
  }
  if (*id == 4 || *id == 5) {
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
  auto id = numericSelector(selector);
  if (!id) {
    if (const auto *name = std::get_if<std::string>(&selector.value)) {
      id = namedStringPropertySelector(*name);
    }
  }
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
  case 16:
    return {.value = text.fullArtist, .supported = true};
  case 1003: {
    const auto audited = text.auditedStringProperties.find(*id);
    if (audited != text.auditedStringProperties.end()) {
      return {.value = audited->second, .supported = true};
    }
    // PlayerResource.getTableFullname() is an empty concatenation when no
    // table context is selected, which is the authoritative app state here.
    static constexpr std::string_view empty;
    return {.value = empty, .supported = true};
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
  // These StringPropertyFactory families intentionally return an empty string
  // outside their owning scenes (selection, result, key config, or practice).
  // A gameplay skin may still declare them, so preserve the upstream empty
  // value instead of turning the destination into an app-specific failure.
  if (*id == 1 || *id == 2 || *id == 3 || *id == 30 || *id == 50 ||
      *id == 51 || *id == 60 || *id == 61 || *id == 62 || *id == 86 ||
      *id == 1000 || *id == 1001 || *id == 1002 || *id == 1010 ||
      *id == 1020 || *id == 1021 || *id == 1030 || *id == 1031 ||
      (*id >= 40 && *id <= 49) || (*id >= 100 && *id <= 129) ||
      (*id >= 150 && *id <= 159) || (*id >= 200 && *id <= 219) ||
      (*id >= 240 && *id <= 283) || (*id >= 1040 && *id <= 1055) ||
      (*id >= 1060 && *id <= 1075) || (*id >= 1080 && *id <= 1095)) {
    static constexpr std::string_view empty;
    return {.value = empty, .supported = true};
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
    // MainController constructs a SkinOffset for every slot from zero through
    // SkinProperty.OFFSET_MAX. JSONSkinLoader configures only the selected
    // custom offsets, so an otherwise unconfigured valid slot remains the
    // default all-zero offset rather than becoming an unsupported lookup.
    if (id >= 0 && id <= SkinCommandPolicy::maximumBeatorajaOffsetId) {
      return {.value = ConfigOffset{}, .supported = true};
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
                 long long LanePresentationState::*field, int timerId,
                 std::optional<bool> requiredPressed)
          -> std::optional<std::int64_t> {
    const auto wide = static_cast<std::int64_t>(timerId);
    const auto first = static_cast<std::int64_t>(firstId);
    if (wide < first || wide >= first + count) {
      return std::nullopt;
    }
    const auto index = static_cast<std::size_t>(wide - first);
    if (index >= snapshot->lanes.size()) {
      return INT64_MIN;
    }
    const auto &lane = snapshot->lanes[index];
    // KeyInputProccessor starts key-off and clears key-on on release, then
    // clears key-off before starting key-on on the following press.  Retaining
    // both timestamps makes skins such as simple-play-simple draw the stale
    // key-on destination over their release animation.
    if (requiredPressed && lane.pressed != *requiredPressed) {
      return INT64_MIN;
    }
    return lane.*field;
  };
  if (const auto value =
          laneTimer(100, 20, &LanePresentationState::pressMicros, *id,
                    true)) {
    return *value;
  }
  if (const auto value =
          laneTimer(120, 20, &LanePresentationState::releaseMicros, *id,
                    false)) {
    return *value;
  }
  if (const auto value =
          laneTimer(50, 20, &LanePresentationState::bombMicros, *id,
                    std::nullopt)) {
    return *value;
  }
  switch (*id) {
  case 41:
    return snapshot->clock.playTimer.active &&
                   snapshot->clock.playTimer.elapsedMillisExact
               ? snapshot->clock.playTimer.startMicros
               : INT64_MIN;
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
  builtInTraversal_.reset();
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
  if (name == "fullartist")
    return 16;
  if (name == "tablefull")
    return 1003;
  if (name == "nowbpm")
    return 160;
  if (name == "lanecover")
    return 4;
  if (name == "lanecover2")
    return 5;
  if (name == "judge_1p_perfect")
    return 241;
  if (name == "judge_1p_early")
    return 1242;
  if (name == "judge_1p_late")
    return 1243;
  return std::nullopt;
}

} // namespace skin
