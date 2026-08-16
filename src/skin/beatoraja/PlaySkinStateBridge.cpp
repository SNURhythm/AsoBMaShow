#include "PlaySkinStateBridge.h"

#include "GameplaySkinEndAnimation.h"
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

constexpr std::array<int, 9> kBeatorajaScoreRankBoundaries = {
    0, 6, 9, 12, 15, 18, 21, 24, 28};

std::int64_t javaDoubleToInt(double value);

bool matchesBeatorajaScoreRank(double rate, int totalNotes, int rank) {
  if (totalNotes == 0 || rank < 0 ||
      static_cast<std::size_t>(rank + 1) >=
          kBeatorajaScoreRankBoundaries.size()) {
    return false;
  }
  const float pinnedRate = static_cast<float>(rate);
  const int low = kBeatorajaScoreRankBoundaries[rank];
  const int high = kBeatorajaScoreRankBoundaries[rank + 1];
  return pinnedRate >= static_cast<float>(low) / 27.0F &&
         (high > 27 || !(pinnedRate >= static_cast<float>(high) / 27.0F));
}

bool reachesBeatorajaScoreRank(double rate, int totalNotes, int threshold) {
  if (totalNotes == 0) {
    return false;
  }
  return static_cast<float>(rate) >= static_cast<float>(threshold) / 27.0F;
}

std::int64_t beatorajaNextRank(const PlayfieldVisualState &snapshot,
                               int totalNotes) {
  const int passedNotes = std::max(0, snapshot.authority.stagePassedNotes);
  const float rate = static_cast<float>(scoreRate(snapshot.score, totalNotes));
  if (totalNotes != 0) {
    for (int rank = 0; rank < 27; ++rank) {
      const bool qualified =
          rate >= static_cast<float>(rank) / 27.0F;
      if (rank % 3 == 0 && !qualified) {
        const double needed =
            static_cast<double>(rank * (passedNotes * 2)) / 27.0 -
            static_cast<double>(rate) * static_cast<double>(passedNotes * 2);
        return javaDoubleToInt(std::ceil(needed));
      }
    }
  }
  return static_cast<std::int64_t>(passedNotes) * 2 - snapshot.score;
}

int targetScore(const PlayfieldVisualState &snapshot) {
  return snapshot.authority.pacemakerTarget.enabled
             ? snapshot.authority.pacemakerTarget.finalScore
             : 0;
}

int passedNotes(const PlayfieldVisualState &snapshot, int totalNotes) {
  int passed = 0;
  for (const Judgement judgement : {PGreat, Great, Good, Bad, Poor}) {
    const auto found = snapshot.authority.judgementCounters.find(judgement);
    if (found != snapshot.authority.judgementCounters.end()) {
      passed += found->second;
    }
  }
  return std::clamp(passed, 0, std::max(0, totalNotes));
}

int bestScoreAtPassedNotes(const PlayfieldVisualState &snapshot) {
  const auto &target = snapshot.authority.bestScoreTarget;
  if (!target.enabled) {
    return 0;
  }
  return pacemaker::targetScoreAtPlayedNotes(
      target, passedNotes(snapshot, target.totalNotes));
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

std::optional<int>
beatorajaPlayerOneSkinLaneOffset(const PlayfieldChartVisualModel &chart,
                                 int lane) noexcept {
  // Pinned LaneProperty.java maps BMS lane identities to a skin offset rather
  // than using their physical chart order. Aso's parser retains the original
  // BMS lane IDs, including scratch at 7 (and 15 for 2P), so preserve that
  // distinction here. 2P has no gameplay authority yet and is intentionally
  // not projected into 1P timer IDs.
  switch (chart.keyCount) {
  case 5:
    if (lane == 7) {
      return 0;
    }
    return lane >= 0 && lane < 5 ? std::optional<int>(lane + 1)
                                 : std::nullopt;
  case 7:
    if (lane == 7) {
      return 0;
    }
    return lane >= 0 && lane < 7 ? std::optional<int>(lane + 1)
                                 : std::nullopt;
  case 9:
    return lane >= 0 && lane < 9 ? std::optional<int>(lane + 1)
                                 : std::nullopt;
  case 10:
    if (lane == 7) {
      return 0;
    }
    return lane >= 0 && lane < 5 ? std::optional<int>(lane + 1)
                                 : std::nullopt;
  case 14:
    if (lane == 7) {
      return 0;
    }
    return lane >= 0 && lane < 7 ? std::optional<int>(lane + 1)
                                 : std::nullopt;
  default:
    return std::nullopt;
  }
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

constexpr std::int64_t saturatingSubtract(std::int64_t left,
                                          std::int64_t right) noexcept {
  if (right > 0 &&
      left < std::numeric_limits<std::int64_t>::min() + right) {
    return std::numeric_limits<std::int64_t>::min();
  }
  if (right < 0 &&
      left > std::numeric_limits<std::int64_t>::max() + right) {
    return std::numeric_limits<std::int64_t>::max();
  }
  return left - right;
}

constexpr std::int64_t addVisualTimestamp(std::int64_t left,
                                          std::int64_t right) noexcept {
  if (right > 0 &&
      left > std::numeric_limits<std::int64_t>::max() - right) {
    return std::numeric_limits<std::int64_t>::max();
  }
  // INT64_MIN is the timer-off sentinel, so clamp both an exact result and
  // negative overflow to the first representable active timestamp.
  if (right < 0 &&
      left <= std::numeric_limits<std::int64_t>::min() - right) {
    return std::numeric_limits<std::int64_t>::min() + 1;
  }
  return left + right;
}

std::int64_t saturatingVisualTimestampForGameplayTimestamp(
    const PlayfieldVisualState &snapshot,
    std::int64_t gameplayTimestampMicros) {
  const auto visual = snapshot.clock.visualTimeMicros;
  const auto gameplay = snapshot.clock.gameplayTimeMicros;
  if (visual >= 0 && gameplay < 0 &&
      visual > std::numeric_limits<std::int64_t>::max() + gameplay) {
    if (gameplayTimestampMicros >= 0) {
      return std::numeric_limits<std::int64_t>::max();
    }
    return addVisualTimestamp(gameplayTimestampMicros - gameplay, visual);
  }
  if (visual < 0 && gameplay > 0 &&
      visual < std::numeric_limits<std::int64_t>::min() + gameplay) {
    if (gameplayTimestampMicros <= 0) {
      return std::numeric_limits<std::int64_t>::min() + 1;
    }
    return addVisualTimestamp(gameplayTimestampMicros - gameplay, visual);
  }
  return addVisualTimestamp(gameplayTimestampMicros, visual - gameplay);
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

void PlaySkinStateBridge::updatePinnedPlayTimers() {
  const auto *snapshot = state();
  if (snapshot == nullptr) {
    return;
  }

  const auto &playTimer = snapshot->clock.playTimer;
  if (playTimer.active && playTimer.elapsedMillisExact &&
      playTimer.playtimeMillis.has_value()) {
    const std::int64_t elapsedMillis = playTimerElapsedMillis(*snapshot);
    const bool musicEndWasActive =
        musicEndTimerStartMicros_ != kPlayfieldTimestampOff;
    if (elapsedMillis > *playTimer.playtimeMillis) {
      if (!musicEndWasActive) {
        musicEndTimerStartMicros_ = skinStateClockMicros(*snapshot);
      }
    } else if (elapsedMillis >
               static_cast<std::int64_t>(*playTimer.playtimeMillis) -
                   skin::kBeatorajaEndOfNotesMarginMillis) {
      if (endOfNoteTimerStartMicros_ == kPlayfieldTimestampOff) {
        endOfNoteTimerStartMicros_ = skinStateClockMicros(*snapshot);
      }
    }
    // STATE_PLAY starts TIMER_MUSIC_END, and only the following BMSPlayer
    // update executes STATE_FINISHED. Keep that one-frame ordering so an
    // authored zero finishmargin still observes the music-end timer first.
    if (musicEndWasActive) {
      const std::int64_t musicEndElapsedMillis =
          saturatingSubtract(skinStateClockMicros(*snapshot),
                             musicEndTimerStartMicros_) /
          1'000;
      if (musicEndElapsedMillis > context_.model->model.timing.finishMarginMillis &&
          fadeoutTimerStartMicros_ == kPlayfieldTimestampOff) {
        fadeoutTimerStartMicros_ = skinStateClockMicros(*snapshot);
      }
    }
  }

  const int totalNotes = context_.chartModel.staticMetadata.totalNotes;
  const auto pinnedTimerNow = [&]() {
    const long long source =
        snapshot->lastJudgeVisualMicros == kPlayfieldTimestampOff
            ? snapshot->clock.visualTimeMicros
            : snapshot->lastJudgeVisualMicros;
    return skinStateTimestampMicros(*snapshot, source);
  };
  const auto switchPinnedTimer = [this, &pinnedTimerNow](int id, bool on) {
    if (on) {
      pinnedSwitchTimerStarts_.try_emplace(id, pinnedTimerNow());
    } else {
      pinnedSwitchTimerStarts_.erase(id);
    }
  };
  const double finalRate = scoreRate(snapshot->score, totalNotes);
  const int gaugeType = gaugeTypeIndex(snapshot->authority.gaugeType);
  const bool gaugeAtMaximum =
      snapshot->authority.gaugeRules.compiled && gaugeType >= 0 &&
      static_cast<std::size_t>(gaugeType) <
          snapshot->authority.gaugeRules.gauges.size() &&
      snapshot->authority.currentGauge ==
          snapshot->authority.gaugeRules
              .gauges[static_cast<std::size_t>(gaugeType)]
              .maximum;
  switchPinnedTimer(44, gaugeAtMaximum);
  switchPinnedTimer(348, reachesBeatorajaScoreRank(finalRate, totalNotes, 18));
  switchPinnedTimer(349, reachesBeatorajaScoreRank(finalRate, totalNotes, 21));
  switchPinnedTimer(350, reachesBeatorajaScoreRank(finalRate, totalNotes, 24));
  switchPinnedTimer(351, snapshot->score >= snapshot->authority.bestScore);
  switchPinnedTimer(352, snapshot->score >= targetScore(*snapshot));

  // JudgeManager switches each LaneState.timerHold while a classic long note
  // is processing or an HCN is actively increasing. NotePresentationState
  // already carries that active truth for every gameplay surface. Aggregate
  // it once per frame, then use LaneProperty's source offset table above.
  std::array<bool, 10> playerOneLongHeld{};
  const std::span<const NotePresentationState> noteStates =
      snapshot->noteStates();
  if (noteStates.size() == context_.chartModel.notes.size()) {
    for (std::size_t index = 0; index < noteStates.size(); ++index) {
      const auto &note = context_.chartModel.notes[index];
      if (note.kind != ChartVisualNoteKind::LongHead &&
          note.kind != ChartVisualNoteKind::LongTail) {
        continue;
      }
      const auto offset =
          beatorajaPlayerOneSkinLaneOffset(context_.chartModel, note.lane);
      if (offset && *offset >= 0 &&
          static_cast<std::size_t>(*offset) < playerOneLongHeld.size() &&
          noteStates[index].longActive) {
        playerOneLongHeld[static_cast<std::size_t>(*offset)] = true;
      }
    }
  }
  const auto switchLaneTimer = [this, snapshot](int id, bool on) {
    if (on) {
      pinnedSwitchTimerStarts_.try_emplace(id, skinStateClockMicros(*snapshot));
    } else {
      pinnedSwitchTimerStarts_.erase(id);
    }
  };
  for (std::size_t offset = 0; offset < playerOneLongHeld.size(); ++offset) {
    switchLaneTimer(70 + static_cast<int>(offset),
                    playerOneLongHeld[offset]);
  }

  const bool fullCombo = totalNotes > 0 &&
                         snapshot->authority.stagePassedNotes == totalNotes &&
                         snapshot->authority.stageCombo == totalNotes;
  if (!fullCombo) {
    fullComboTimerStartMicros_ = kPlayfieldTimestampOff;
  } else if (fullComboTimerStartMicros_ == kPlayfieldTimestampOff) {
    const long long source =
        snapshot->lastJudgeVisualMicros == kPlayfieldTimestampOff
            ? snapshot->clock.visualTimeMicros
            : snapshot->lastJudgeVisualMicros;
    fullComboTimerStartMicros_ = skinStateTimestampMicros(*snapshot, source);
  }
}

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
  updatePinnedPlayTimers();

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
              event->id, skinStateClockMicros(*state_));
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
  const std::int64_t now = skinStateClockMicros(*state_);
  if (const auto previous = customEventLastExecutionMicros_.find(event.id);
      previous != customEventLastExecutionMicros_.end()) {
    const auto elapsed = saturatingSubtract(now, previous->second);
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
  case 40:
  case 41:
    return {.value = *id == 41 ? context_.chartModel.staticMetadata.hasBga
                                : !context_.chartModel.staticMetadata.hasBga,
            .supported = true};
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
  case 82:
    return {.value = snapshot->authority.gameplayMode ==
                         PlayfieldGameplayMode::Play ||
                         snapshot->authority.gameplayMode ==
                             PlayfieldGameplayMode::Practice,
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
  case 160:
  case 161:
  case 162:
  case 163:
  case 164:
  case 1160:
  case 1161: {
    // BooleanPropertyFactory compares SongData's Mode id. Aso's immutable
    // visual model retains that same canonical key-mode count.
    const int keyMode = context_.chartModel.keyCount;
    const int expected = *id == 160   ? 7
                         : *id == 161 ? 5
                         : *id == 162 ? 14
                         : *id == 163 ? 10
                         : *id == 164 ? 9
                         : *id == 1160 ? 24
                                       : 48;
    return {.value = keyMode == expected, .supported = true};
  }
  case 1046: {
    // BooleanPropertyFactory.gauge_ex accepts Beatoraja gauge indexes
    // 0, 1, 4, 5, 7, and 8. Aso's authoritative gauge catalog contains the
    // shared 0..5 entries; its indexes match those source entries exactly.
    const int type = gaugeTypeIndex(snapshot->authority.gaugeType);
    return {.value = type == 0 || type == 1 || type == 4 || type == 5,
            .supported = true};
  }
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
  case 178:
  case 179:
    return {.value = *id == 179
                         ? context_.chartModel.staticMetadata.hasRandomSequence
                         : !context_.chartModel.staticMetadata.hasRandomSequence,
            .supported = true};
  case 1177:
    return {.value = context_.chartModel.staticMetadata.hasBpmStop,
            .supported = true};
  case 270:
    return {.value = snapshot->authority.laneCoverAdjustmentHeld,
            .supported = true};
  case 280:
  case 281:
  case 282:
  case 283: {
    const int stage = *id - 280;
    return {.value = snapshot->authority.courseMode &&
                         snapshot->authority.courseStageCount > 0 &&
                         snapshot->authority.courseStageIndex == stage &&
                         stage != snapshot->authority.courseStageCount - 1,
            .supported = true};
  }
  case 289:
    return {.value = snapshot->authority.courseMode &&
                         snapshot->authority.courseStageCount > 0 &&
                         snapshot->authority.courseStageIndex ==
                             snapshot->authority.courseStageCount - 1,
            .supported = true};
  case 290:
    return {.value = snapshot->authority.courseMode, .supported = true};
  case 1002:
  case 1003:
  case 1004:
  case 1005:
  case 1006:
  case 1007:
  case 1010:
  case 1011:
  case 1012:
  case 1013:
  case 1014:
  case 1015:
  case 1016:
  case 1017:
    // BooleanPropertyFactory exposes these only to MusicSelector. During a
    // BMSPlayer gameplay skin they are recognized but always false.
    return {.value = false, .supported = true};
  case 200:
  case 201:
  case 202:
  case 203:
  case 204:
  case 205:
  case 206:
  case 207:
  case 300:
  case 301:
  case 302:
  case 303:
  case 304:
  case 305:
  case 306:
  case 307:
  case 340:
  case 341:
  case 342:
  case 343:
  case 344:
  case 345:
  case 346:
  case 347: {
    const int groupStart = *id >= 340 ? 340 : *id >= 300 ? 300 : 200;
    const int rank = 7 - (*id - groupStart);
    return {.value = matchesBeatorajaScoreRank(
                scoreRate(snapshot->score, snapshot->authority.stagePassedNotes),
                context_.chartModel.staticMetadata.totalNotes, rank),
            .supported = true};
  }
  case 220:
  case 221:
  case 222:
  case 223:
  case 224:
  case 225:
  case 226:
  case 227: {
    const int rank = 7 - (*id - 220);
    return {.value = reachesBeatorajaScoreRank(
                scoreRate(snapshot->score,
                          context_.chartModel.staticMetadata.totalNotes),
                context_.chartModel.staticMetadata.totalNotes,
                kBeatorajaScoreRankBoundaries[rank]),
            .supported = true};
  }
  case 320:
  case 321:
  case 322:
  case 323:
  case 324:
  case 325:
  case 326:
  case 327: {
    const int rank = 7 - (*id - 320);
    return {.value = matchesBeatorajaScoreRank(
                scoreRate(snapshot->authority.bestScore,
                          context_.chartModel.staticMetadata.totalNotes),
                context_.chartModel.staticMetadata.totalNotes, rank),
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
                     snapshot->fastSlowMicros < 0,
            .supported = true};
  case 1243:
    return {.value = snapshot->lastJudge.judgement != None &&
                     snapshot->fastSlowMicros > 0,
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
    // IntegerPropertyFactory.IndexType.markprocessednote is the source's
    // image-index reflection of PlayerConfig.markprocessednote.
    if (*id == 305) {
      return {.value = snapshot->configuration.markProcessedNotes ? 1 : 0,
              .supported = true};
    }
    switch (*id) {
    case 40:
      return {.value = gaugeTypeIndex(snapshot->authority.gaugeType),
              .supported = true};
    case 42:
      return {.value = snapshot->authority.player1RandomOption,
              .supported = true};
    case 43:
      return {.value = snapshot->authority.player2RandomOption,
              .supported = true};
    case 54:
      return {.value = snapshot->authority.doublePlayOption,
              .supported = true};
    case 55:
      return {.value = static_cast<int>(snapshot->configuration.hispeedFixMode),
              .supported = true};
    case 72:
      // Config.BGA_ON is 0 and Config.BGA_OFF is 2. Aso has no source-faithful
      // equivalent of Config.BGA_AUTO (1), so do not synthesize that state.
      return {.value = snapshot->configuration.bgaEnabled ? 0 : 2,
              .supported = true};
    case 78:
      return {.value = gaugeAutoShiftModeValue(
                  snapshot->authority.gaugeAutoShift),
              .supported = true};
    case 306:
      return {.value = snapshot->configuration.bpmGuideEnabled ? 1 : 0,
              .supported = true};
    case 330:
      return {.value = snapshot->authority.laneCoverEnabled ? 1 : 0,
              .supported = true};
    case 331:
      return {.value = snapshot->authority.liftEnabled ? 1 : 0,
              .supported = true};
    case 332:
      return {.value = snapshot->authority.hiddenEnabled ? 1 : 0,
              .supported = true};
    case 342:
      return {.value = snapshot->configuration.hispeedAutoAdjust ? 1 : 0,
              .supported = true};
    case 340:
      return {.value = snapshot->configuration.judgeAlgorithmImageIndex,
              .supported = true};
    default:
      break;
    }
    // IntegerPropertyFactory accepts the complete unsigned cache domain.
    // An unavailable image-index source naturally selects frame zero, which
    // is the safe source-neutral equivalent of the absent upstream property.
    if (*id >= 0 && *id <= 65'535) {
      return {.value = 0, .supported = true};
    }
    reportUnsupported("image_index", selector);
    return {};
  }
  if (domain != SkinIntegerPropertyDomain::IntegerValue) {
    reportUnsupported("integer", selector);
    return {};
  }
  // Pinned from IntegerPropertyFactory.createHispeedProperty() at Beatoraja
  // c2ed5db1a46145ed10790c3872f717e95b59db9d. The source reads
  // LaneRenderer.getHispeed(), i.e. the live cover-compensated PlayConfig
  // value, not the app preference or playback-scaled skin traversal value.
  const float configuredHispeed =
      builtInTraversal_ ? builtInTraversal_->configuredHispeed : 0.0F;
  switch (*id) {
  case 10:
    return {.value = javaDoubleToInt(configuredHispeed * 100.0F),
            .supported = true};
  case 310:
    return {.value = javaDoubleToInt(configuredHispeed), .supported = true};
  case 311:
    return {.value = javaDoubleToInt(configuredHispeed * 100.0F) % 100,
            .supported = true};
  default:
    break;
  }
  const auto durationValue = [&]() -> std::optional<std::int64_t> {
    if (*id != 312 && *id != 313 && (*id < 1312 || *id > 1327)) {
      return std::nullopt;
    }
    if (!builtInTraversal_ ||
        !std::isfinite(
            static_cast<double>(builtInTraversal_->configuredHispeed)) ||
        builtInTraversal_->configuredHispeed < 0.0F) {
      return std::nullopt;
    }
    const auto durationFor = [&](double bpm, bool cover, bool green)
        -> std::optional<std::int64_t> {
      const auto value = gameplay_hispeed::liveDurationValue(
          bpm, builtInTraversal_->configuredHispeed,
          snapshot->authority.laneCoverPercent, cover,
          // createDurationLanecoverProperty is independent of scroll; 312/313
          // below use LaneRenderer.currentduration, which is not.
          1.0);
      if (!value) {
        return std::nullopt;
      }
      return javaMathRoundToInt(*value * (green ? 0.6 : 1.0));
    };
    if (*id == 312 || *id == 313) {
      // LaneRenderer.currentduration rounds its current speed/cover product.
      // ValueType.duration_green then applies its integer `* 3 / 5`
      // conversion afterwards.
      const auto current = gameplay_hispeed::liveDurationMilliseconds(
          snapshot->authority.currentBpm, builtInTraversal_->configuredHispeed,
          snapshot->authority.laneCoverPercent,
          snapshot->authority.laneCoverEnabled,
          snapshot->authority.currentScrollRate);
      if (!current) {
        return std::nullopt;
      }
      return *id == 312 ? *current
                         : gameplay_hispeed::durationToGreenNumber(*current);
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
  case 12:
    // PlayerConfig defaults notes-display timing to zero. Aso does not yet
    // expose a per-profile adjustment, so preserve that upstream default.
    return {.value = 0, .supported = true};
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
  // NUMBER_POINT has mode-specific score-point arithmetic in
  // ScoreDataProperty. Keep the existing EX-score publication until the
  // canonical upstream Mode identity is carried through this bridge.
  case 100:
  case 101:
  case 171:
    return {.value = snapshot->score, .supported = true};
  case 57:
    return {.value = javaDoubleToInt(
                static_cast<double>(snapshot->configuration.masterVolume) *
                100.0),
            .supported = true};
  case 58:
    return {.value = javaDoubleToInt(
                static_cast<double>(snapshot->configuration.keysoundVolume) *
                100.0),
            .supported = true};
  case 59:
    return {.value = javaDoubleToInt(
                static_cast<double>(snapshot->configuration.bgmVolume) *
                100.0),
            .supported = true};
  case 72:
    return {.value = static_cast<std::int64_t>(
                context_.chartModel.staticMetadata.totalNotes) *
                    2,
            .supported = true};
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
  case 174:
    return {.value = snapshot->authority.maximumCombo, .supported = true};
  case 102:
  case 103: {
    const auto [integer, fractional] = scoreRateParts(scoreRate(
        snapshot->score, snapshot->authority.stagePassedNotes));
    return {.value = *id == 102 ? integer : fractional, .supported = true};
  }
  case 104:
    return {.value = snapshot->combo, .supported = true};
  case 115:
  case 116:
  case 155:
  case 156: {
    const auto [integer, fractional] = scoreRateParts(scoreRate(
        snapshot->score, context_.chartModel.staticMetadata.totalNotes));
    return {.value = (*id == 115 || *id == 155) ? integer : fractional,
            .supported = true};
  }
  case 122:
  case 123:
  case 135:
  case 136:
  case 157:
  case 158: {
    const auto [integer, fractional] = scoreRateParts(scoreRate(
        targetScore(*snapshot), context_.chartModel.staticMetadata.totalNotes));
    const bool integerPart = *id == 122 || *id == 135 || *id == 157;
    return {.value = integerPart ? integer : fractional, .supported = true};
  }
  case 108:
  case 128:
  case 153:
    // IntegerPropertyFactory's diff_exscore, diff_exscore2, and
    // diff_targetscore all delegate to createDiffRivalScoreProperty():
    // current EX score minus ScoreDataProperty's live rival score. LITONE12
    // uses 108 for Ghost and 153 for its graph difference.
    return {.value = static_cast<std::int64_t>(snapshot->score) -
                    snapshot->authority.pacemakerStatus.targetScore,
            .supported = true};
  case 150:
    // NUMBER_HIGHSCORE delegates to createHighScoreProperty(), which reads
    // ScoreDataProperty.getBestScore(): the persisted final EX score.
    return {.value = static_cast<std::int64_t>(snapshot->authority.bestScore),
            .supported = true};
  case 170:
    return {.value = static_cast<std::int64_t>(snapshot->authority.bestScore),
            .supported = true};
  case 152:
  case 172:
    // NUMBER_DIFF_HIGHSCORE delegates to createDiffHighScoreProperty(),
    // which compares against ScoreDataProperty.getNowBestScore(), including
    // its persisted-ghost progression when available.
    return {.value = static_cast<std::int64_t>(snapshot->score) -
                    bestScoreAtPassedNotes(*snapshot),
            .supported = true};
  case 154:
    return {.value = beatorajaNextRank(
                *snapshot, context_.chartModel.staticMetadata.totalNotes),
            .supported = true};
  case 183:
  case 184: {
    const auto [integer, fractional] = scoreRateParts(scoreRate(
        snapshot->authority.bestScore,
        context_.chartModel.staticMetadata.totalNotes));
    return {.value = *id == 183 ? integer : fractional, .supported = true};
  }
  case 121:
  case 151:
    return {.value = targetScore(*snapshot), .supported = true};
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
  case 423:
  case 424: {
    const bool fast = *id == 423;
    return {.value = capturedJudgeFastSlowCount(*snapshot, Great, fast) +
                         capturedJudgeFastSlowCount(*snapshot, Good, fast) +
                         capturedJudgeFastSlowCount(*snapshot, Bad, fast) +
                         capturedJudgeFastSlowCount(*snapshot, Poor, fast) +
                         capturedJudgeFastSlowCount(*snapshot, Kpoor, fast),
            .supported = true};
  }
  case 425:
    return {.value = snapshot->authority.comboBreak, .supported = true};
  case 426:
    return {.value = capturedJudgeCount(*snapshot, Poor) +
                         capturedJudgeCount(*snapshot, Kpoor),
            .supported = true};
  case 400:
    return {.value = context_.chartModel.staticMetadata.judgeRank,
            .supported = true};
  case 525:
    // IntegerPropertyFactory's judge_duration1 reads
    // JudgeManager.getRecentJudgeTiming().  Beatoraja stores an early input
    // as positive mfast; AsoBMaShow's Judge.Diff (captured in
    // fastSlowMicros) stores that same input as negative.  Convert both its
    // sign and unit here. C++ signed division has Java's
    // truncation-toward-zero rule.
    return {.value = -static_cast<std::int64_t>(snapshot->fastSlowMicros) /
                         1'000,
            .supported = true};
  case 526:
  case 527:
    return {.value = 0, .supported = true};
  case 165:
    // Gameplay rendering starts after BMSResource has completed its audio/BGA
    // load. The equivalent resource progress is therefore 100%; Unknown and
    // Loading retain the pre-completion value of zero.
    return {.value = snapshot->authority.loadingState ==
                             PlayfieldLoadingState::Loaded
                         ? 100
                         : 0,
            .supported = true};
  default:
    break;
  }
  // IntegerPropertyFactory has a 0..65535 input domain. For an official
  // value that has no Aso gameplay counterpart yet, preserve Beatoraja's
  // common unavailable-state sentinel instead of rejecting the whole skin.
  if (*id >= 0 && *id <= 65'535) {
    return {.value = std::numeric_limits<int>::min(), .supported = true};
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
  // BMSPlayer updates ScoreDataProperty with JudgeManager.getPastNotes(),
  // independently of whether a pacemaker target was configured.
  const int playedNotes = snapshot->authority.stagePassedNotes;
  const auto currentFullRate = scoreRate(snapshot->score, totalNotes);
  const auto currentRate = scoreRate(snapshot->score, playedNotes);
  const auto bestFullRate =
      scoreRate(snapshot->authority.bestScore, totalNotes);
  const auto targetFullRate = scoreRate(targetScore(*snapshot), totalNotes);
  const auto referenceCurrentRate = [&](int score) {
    return totalNotes == 0 ? 0.0 : scoreRate(score, totalNotes);
  };
  if (domain == SkinFloatPropertyDomain::FloatValue) {
    const double floatMinimum =
        static_cast<double>(std::numeric_limits<float>::min());
    switch (*id) {
    case 310:
      // Pinned from FloatPropertyFactory.FloatType.hispeed. The source reads
      // the live LaneRenderer value, not the preference or skin traversal.
      return {.value = static_cast<double>(
                  builtInTraversal_ ? builtInTraversal_->configuredHispeed
                                    : 0.0F),
              .supported = true};
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
  case 17:
    return {.value = snapshot->configuration.masterVolume, .supported = true};
  case 18:
    return {.value = snapshot->configuration.keysoundVolume,
            .supported = true};
  case 19:
    return {.value = snapshot->configuration.bgmVolume, .supported = true};
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
    return {.value = referenceCurrentRate(bestScoreAtPassedNotes(*snapshot)),
            .supported = true};
  case 113:
    return {.value = bestFullRate, .supported = true};
  case 114:
    return {.value = referenceCurrentRate(
                snapshot->authority.pacemakerStatus.targetScore),
            .supported = true};
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
  if (*id == 6 || *id == 101) {
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
  case 2:
    return {.value = state()->authority.playerName, .supported = true};
  case 50:
    if (context_.model != nullptr) {
      return {.value = context_.model->model.header.name, .supported = true};
    }
    break;
  case 51:
    if (context_.model != nullptr) {
      return {.value = context_.model->model.header.author, .supported = true};
    }
    break;
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
  case 1030:
    return {.value = context_.chartModel.chartMd5, .supported = true};
  case 1031:
    return {.value = context_.chartModel.chartSha256, .supported = true};
  case 13:
    return {.value = text.genre, .supported = true};
  case 14:
    return {.value = text.artist, .supported = true};
  case 15:
    return {.value = text.subartist, .supported = true};
  default:
    break;
  }
  if (*id >= 150 && *id <= 159) {
    const std::size_t stage = static_cast<std::size_t>(*id - 150);
    if (stage < state()->authority.courseStageTitles.size()) {
      return {.value = state()->authority.courseStageTitles[stage],
              .supported = true};
    }
    static constexpr std::string_view empty;
    return {.value = empty, .supported = true};
  }
  // These StringPropertyFactory families intentionally return an empty string
  // outside their owning scenes (selection, result, key config, or practice).
  // A gameplay skin may still declare them, so preserve the upstream empty
  // value instead of turning the destination into an app-specific failure.
  if (*id == 1 || *id == 3 || *id == 30 || *id == 50 ||
      *id == 51 || *id == 60 || *id == 61 || *id == 62 || *id == 86 ||
      *id == 1000 || *id == 1001 || *id == 1002 || *id == 1010 ||
      *id == 1020 || *id == 1021 || *id == 1030 || *id == 1031 ||
      (*id >= 40 && *id <= 49) || (*id >= 100 && *id <= 129) ||
      (*id >= 200 && *id <= 219) ||
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
    return skinStateTimestampMicros(*snapshot, lane.*field);
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
  case 40:
    return snapshot->sceneStartMicros != kPlayfieldTimestampOff &&
                   skinStateClockMicros(*snapshot) >= 0
               ? 0
               : INT64_MIN;
  case 41:
    return snapshot->clock.playTimer.active &&
                   snapshot->clock.playTimer.elapsedMillisExact
               ? (snapshot->sceneStartMicros == kPlayfieldTimestampOff
                      ? snapshot->clock.playTimer.startMicros
                      : skinStateTimestampMicros(
                            *snapshot,
                            saturatingVisualTimestampForGameplayTimestamp(
                                *snapshot,
                                snapshot->clock.playTimer.startMicros)))
               : INT64_MIN;
  case 46:
  case 446:
    return skinStateTimestampMicros(*snapshot,
                                    snapshot->lastJudgeVisualMicros);
  case 48:
    return fullComboTimerStartMicros_;
  case 143:
    return endOfNoteTimerStartMicros_;
  case 2:
    return fadeoutTimerStartMicros_;
  case 908:
    return musicEndTimerStartMicros_;
  case 44:
  case 70:
  case 71:
  case 72:
  case 73:
  case 74:
  case 75:
  case 76:
  case 77:
  case 78:
  case 79:
  case 348:
  case 349:
  case 350:
  case 351:
  case 352: {
    const auto found = pinnedSwitchTimerStarts_.find(*id);
    return found == pinnedSwitchTimerStarts_.end() ? INT64_MIN
                                                    : found->second;
  }
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
