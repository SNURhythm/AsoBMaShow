#include "GameplaySkinBuiltinCatalog.h"

#include "BeatorajaBooleanPropertyNames.h"
#include "BeatorajaIntegerPropertyNames.h"
#include "BeatorajaStringPropertyNames.h"

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <vector>

namespace skin {
namespace {

// Pinned from BooleanPropertyFactory.BooleanType at beatoraja
// c2ed5db1a46145ed10790c3872f717e95b59db9d, plus its two
// BooleanPropertyPattern practice ranges. Keep this exact, rather than using
// a permissive numeric range: BooleanPropertyFactory returns null for every
// other ID below its 65,536 cache boundary.
constexpr auto kPinnedBooleanPropertyIds = std::to_array<int>({
    1,    2,    3,    5,    21,   22,   23,   32,   33,   40,   41,
    42,   43,   50,   51,   60,   61,   62,   80,   81,   82,   84,
    90,   91,   100,  101,  102,  103,  104,  105,  118,  119,  121,
    125,  126,  127,  128,  129,  130,  131,  150,  151,  152,  153,
    154,  155,  160,  161,  162,  163,  164,  170,  171,  172,  173,
    174,  175,  176,  177,  178,  179,  180,  181,  182,  183,  184,
    190,  191,  192,  193,  194,  195,  196,  197,  198,  200,  201,
    202,  203,  204,  205,  206,  207,  220,  221,  222,  223,  224,
    225,  226,  227,  230,  231,  232,  233,  234,  235,  236,  237,
    238,  239,  240,  241,  261,  270,  271,  272,  273,  280,  281,
    282,  283,  289,  290,  300,  301,  302,  303,  304,  305,  306,
    307,  320,  321,  322,  323,  324,  325,  326,  327,  330,  331,
    332,  335,  336,  340,  341,  342,  343,  344,  345,  346,  347,
    352,  353,  354,  361,  400,  603,  604,  606,  608,  624,  625,
    1002, 1003, 1004, 1005, 1006, 1007, 1008, 1010, 1011, 1012, 1013,
    1014, 1015, 1016, 1017, 1030, 1031, 1046, 1080, 1100, 1101, 1102,
    1103, 1104, 1128, 1129, 1130, 1131, 1160, 1161, 1177, 1196, 1197,
    1198, 1199, 1200, 1201, 1202, 1203, 1204, 1205, 1206, 1207, 1208,
    1240, 1242, 1243, 1262, 1263, 1330, 1331, 1332, 1335, 1336, 1362,
    1363, 2241, 2242, 2243, 2244, 2245, 2246, 3000, 3001, 3002, 3003,
    3004, 3005, 3006, 3007, 3008, 3009, 3010, 3011, 3012, 3013, 3014,
    3015, 3020, 3021, 3022, 3023, 3024, 3025, 3026, 3027, 3028, 3029,
    3030, 3031, 3032, 3033, 3034, 3035,
});

void add(std::vector<SkinBuiltinBindingCatalogEntry> &entries,
         SkinBindingType type, int selector) {
  entries.push_back(
      {.type = type, .selector = SkinBuiltinPropertySelector{selector}});
}

void add(std::vector<SkinBuiltinBindingCatalogEntry> &entries,
         SkinBindingType type, const char *selector) {
  entries.push_back(
      {.type = type,
       .selector = SkinBuiltinPropertySelector{std::string(selector)}});
}

void add(std::vector<SkinBuiltinBindingCatalogEntry> &entries,
         SkinBindingType type, std::string selector) {
  entries.push_back(
      {.type = type,
       .selector = SkinBuiltinPropertySelector{std::move(selector)}});
}

std::optional<int> numberedPropertySelector(std::string_view name,
                                            std::string_view prefix,
                                            int firstId) noexcept {
  if (!name.starts_with(prefix) || name.size() == prefix.size()) {
    return std::nullopt;
  }
  std::string_view number = name.substr(prefix.size());
  if (number.starts_with('+')) number.remove_prefix(1);
  if (number.empty()) return std::nullopt;
  int parsed = 0;
  for (const char character : number) {
    if (character < '0' || character > '9' ||
        parsed > (std::numeric_limits<int>::max() - (character - '0')) / 10) {
      return std::nullopt;
    }
    parsed = parsed * 10 + (character - '0');
  }
  return parsed >= 1 && parsed <= 10
             ? std::optional<int>(firstId + parsed - 1)
             : std::nullopt;
}

std::optional<int> irClearIntegerPropertySelector(
    std::string_view name) noexcept {
  // IntegerPropertyFactory's three IR clear-statistic patterns share their
  // skin-facing name arrays but use non-contiguous numeric IDs.
  constexpr std::array<std::string_view, 11> clearTypes{{
      "noplay", "failed", "assist", "lightassist", "easy", "normal",
      "hard", "exhard", "fullcombo", "perfect", "max",
  }};
  constexpr std::array<int, 11> clearCounts{{
      202, 210, 204, 206, 212, 214, 216, 208, 218, 222, 224,
  }};
  constexpr std::array<int, 11> clearRates{{
      203, 211, 205, 207, 213, 215, 217, 209, 219, 223, 225,
  }};
  constexpr std::array<int, 11> clearRateAfterDots{{
      230, 234, 231, 232, 235, 236, 237, 233, 238, 239, 240,
  }};
  constexpr std::string_view prefix = "ir_player_";
  const auto match = [&](std::string_view suffix,
                         const std::array<int, 11> &ids) -> std::optional<int> {
    for (std::size_t index = 0; index < clearTypes.size(); ++index) {
      const std::size_t clearTypeOffset = prefix.size();
      if (name.size() == prefix.size() + clearTypes[index].size() +
                             suffix.size() &&
          name.starts_with(prefix) && name.ends_with(suffix) &&
          name.substr(clearTypeOffset, clearTypes[index].size()) ==
              clearTypes[index]) {
        return ids[index];
      }
    }
    return std::nullopt;
  };
  if (const auto id = match("", clearCounts)) return id;
  if (const auto id = match("_rate", clearRates)) return id;
  return match("_rate_afterdot", clearRateAfterDots);
}

std::vector<SkinBuiltinBindingCatalogEntry> makeCatalog() {
  std::vector<SkinBuiltinBindingCatalogEntry> entries;
  entries.reserve(600);

  const SkinBindingType boolean{.kind = SkinBindingKind::BooleanProperty};
  for (const int selector : kPinnedBooleanPropertyIds) {
    add(entries, boolean, selector);
    // BooleanPropertyFactory applies Math.abs(optionid) and negates the
    // resolved property for each negative numeric selector.
    add(entries, boolean, -selector);
  }

  constexpr auto integerValueSelectors = std::to_array(
      {10,  14,  71,  74,  75,  90,  91,  92,  96,  101, 102, 103, 105, 106, 107,
       110, 111, 112, 113, 114, 152, 153, 160, 171, 310, 311, 312, 313, 350,
       351, 352,
       353, 360, 361, 362, 363, 364, 365, 368, 407, 410, 411, 412, 413, 414,
       415, 416, 417, 418, 419, 420,
       421, 422, 425, 427, 525, 526, 527, 1163, 1164});
  const SkinBindingType integerValue{
      .kind = SkinBindingKind::IntegerProperty,
      .integerDomain = SkinIntegerPropertyDomain::IntegerValue};
  for (const int selector : integerValueSelectors) {
    add(entries, integerValue, selector);
  }
  for (const auto &[name, id] : beatorajaIntegerValueProperties()) {
    add(entries, integerValue, std::string(name));
    add(entries, integerValue, id);
  }
  add(entries, integerValue, "nowbpm");
  // Pinned IntegerPropertyFactory exposes the lane-cover family through
  // ValueType, not through getImageIndexProperty.
  for (const int selector : {161, 162, 163, 164, 314, 315, 316}) {
    add(entries, integerValue, selector);
  }
  // ValueType.getProperty accepts this complete contiguous family and creates
  // the LaneRenderer duration function from its ID bits.
  for (int selector = 1312; selector <= 1327; ++selector) {
    add(entries, integerValue, selector);
  }

  // Pinned IntegerPropertyFactory.getImageIndexProperty.  This domain is
  // intentionally separate from ValueType: selector 90, for example, means
  // max BPM in the latter and favorite-chart state in the former.
  const SkinBindingType imageIndex{
      .kind = SkinBindingKind::IntegerProperty,
      .integerDomain = SkinIntegerPropertyDomain::ImageIndex};
  for (const int selector : {10,  11,  12,  40,  42,  43,  54,  55,  61,
                             62,  63,  72,  75,  78,  89,  90,  301, 303,
                             305, 306, 308, 321, 322, 323, 324, 330, 331,
                             332, 340, 341, 342, 343, 350, 351, 352, 353,
                             360, 361, 370, 371, 400, 450, 451, 452, 453,
                             454, 455, 456, 457, 458, 459, 460, 461, 462,
                             463, 464, 465, 466, 469}) {
    add(entries, imageIndex, selector);
  }
  for (int selector = 500; selector <= 519; ++selector) {
    add(entries, imageIndex, selector);
  }
  for (int selector = 1510; selector <= 1599; ++selector) {
    add(entries, imageIndex, selector);
  }
  for (int selector = 1610; selector <= 1699; ++selector) {
    add(entries, imageIndex, selector);
  }
  for (const auto &[name, id] : beatorajaImageIndexProperties()) {
    add(entries, imageIndex, std::string(name));
    add(entries, imageIndex, id);
  }
  for (int index = 1; index <= 10; ++index) {
    add(entries, imageIndex, "playertype_ranking" + std::to_string(index));
    add(entries, imageIndex, "cleartype_ranking" + std::to_string(index));
  }

  constexpr auto rateSelectors = std::to_array(
      {1,  4,  5,  6,  7,  8,  17, 18, 19, 20, 101, 102, 103,
       105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115,
       140, 141, 142, 143, 144, 145, 147});
  constexpr auto rateNames = std::to_array({
      "musicselect_position", "lanecover", "lanecover2", "music_progress",
      "skinselect_position", "ranking_position", "mastervolume", "keyvolume",
      "bgmvolume", "practice_position", "music_progress_bar", "load_progress",
      "level", "level_beginner", "level_normal", "level_hyper",
      "level_another", "level_insane", "scorerate", "scorerate_final",
      "bestscorerate_now", "bestscorerate", "targetscorerate_now",
      "targetscorerate", "rate_pgreat", "rate_great", "rate_good",
      "rate_bad", "rate_poor", "rate_maxcombo", "rate_exscore",
  });
  const SkinBindingType rate{.kind = SkinBindingKind::FloatProperty,
                             .floatDomain = SkinFloatPropertyDomain::Rate};
  for (const int selector : rateSelectors) {
    add(entries, rate, selector);
  }
  for (const char *selector : rateNames) {
    add(entries, rate, selector);
  }

  // These are exactly the RateType entries constructed with a FloatWriter.
  // Lane-cover mutation remains unavailable because its RateType has no
  // writer in the pinned source.
  const SkinBindingType floatWriter{.kind = SkinBindingKind::FloatWriter};
  for (const int selector : {1, 7, 8, 17, 18, 19, 20}) {
    add(entries, floatWriter, selector);
  }
  for (const char *selector : {
           "musicselect_position", "skinselect_position", "ranking_position",
           "mastervolume", "keyvolume", "bgmvolume", "practice_position"}) {
    add(entries, floatWriter, selector);
  }

  // getFloatProperty is FloatType + its pattern table + RateType fallback.
  const SkinBindingType floatValue{
      .kind = SkinBindingKind::FloatProperty,
      .floatDomain = SkinFloatPropertyDomain::FloatValue};
  for (const int selector : rateSelectors) {
    add(entries, floatValue, selector);
  }
  for (const char *selector : rateNames) {
    add(entries, floatValue, selector);
  }
  for (const int selector : {85,  86,  87,  88,  89,  122, 135, 155,
                             157, 165, 183, 203, 205, 207, 209, 211,
                             213, 215, 217, 219, 223, 225, 227, 229,
                             285, 286, 287, 288, 289, 310, 360, 362,
                             367, 368, 372, 374, 376, 1102, 1107, 1115}) {
    add(entries, floatValue, selector);
  }
  for (const char *selector : {
           "score_rate", "total_rate", "score_rate2", "duration_average",
           "timing_average", "timign_stddev", "perfect_rate", "great_rate",
           "good_rate", "bad_rate", "poor_rate", "rival_perfect_rate",
           "rival_great_rate", "rival_good_rate", "rival_bad_rate",
           "rival_poor_rate", "best_rate", "rival_rate", "target_rate",
           "target_rate2", "hispeed", "groovegauge_1p",
           "chart_averagedensity", "chart_enddensity", "chart_peakdensity",
           "chart_totalgauge", "loading_progress", "ir_totalclearrate",
           "ir_totalfullcomborate", "ir_player_noplay_rate",
           "ir_player_failed_rate", "ir_player_assist_rate",
           "ir_player_lightassist_rate", "ir_player_easy_rate",
           "ir_player_normal_rate", "ir_player_hard_rate",
           "ir_player_exhard_rate", "ir_player_fullcombo_rate",
           "ir_player_perfect_rate", "ir_player_max_rate",
       }) {
    add(entries, floatValue, selector);
  }

  const SkinBindingType string{.kind = SkinBindingKind::StringProperty};
  constexpr auto stringSelectors = std::to_array(
      {1,  2,  3,  10, 11, 12, 13, 14, 15, 16, 30, 50, 51,
       60, 61, 62, 86, 1000, 1001, 1002, 1003, 1010, 1020, 1021,
       1030, 1031});
  constexpr auto stringNames = std::to_array({
      "rival", "player", "target", "title", "subtitle", "fulltitle",
      "genre", "artist", "subartist", "fullartist", "searchword",
      "skinname", "skinauthor", "mode", "sort", "difficulty",
      "chartreplication", "directory", "tablename", "tablelevel",
      "tablefull", "version", "irname", "irUserName", "songhashmd5",
      "songhashsha256"});
  for (std::size_t index = 0; index < stringSelectors.size(); ++index) {
    add(entries, string, stringSelectors[index]);
    add(entries, string, stringNames[index]);
  }
  const auto addIndexedStrings = [&](int first, int count,
                                     std::string_view prefix,
                                     std::string_view suffix = {}) {
    for (int offset = 0; offset < count; ++offset) {
      add(entries, string, first + offset);
      add(entries, string, std::string(prefix) + std::to_string(offset + 1) +
                              std::string(suffix));
    }
  };
  addIndexedStrings(40, 10, "key");
  for (int index = 11; index <= 54; ++index) {
    add(entries, string, 240 + (index - 11));
    add(entries, string, "key" + std::to_string(index));
  }
  addIndexedStrings(100, 10, "skincategory");
  addIndexedStrings(110, 10, "skinitem");
  addIndexedStrings(120, 10, "rankingname");
  addIndexedStrings(150, 10, "coursetitle");
  addIndexedStrings(200, 10, "targetnamep");
  addIndexedStrings(210, 10, "targetnamen");
  addIndexedStrings(1040, 16, "practice_item");
  addIndexedStrings(1060, 16, "practice_item", "_label");
  addIndexedStrings(1080, 16, "practice_item", "_value");
  for (int index = 1; index <= 16; ++index) {
    add(entries, string, "practice_item_label" + std::to_string(index));
    add(entries, string, "practice_item_value" + std::to_string(index));
  }

  // StringPropertyFactory.StringType.searchword is the only pinned numeric
  // StringWriter. In BMSPlayer its MusicSelector-only body is an intentional
  // no-op, but the non-null writer still makes implicit Text fields editable.
  add(entries, SkinBindingType{.kind = SkinBindingKind::StringWriter}, 30);
  add(entries, SkinBindingType{.kind = SkinBindingKind::StringWriter},
      "searchword");

  // Timers are not a gap: TimerPropertyFactory and PlaySkinStateBridge both
  // define every nonnegative ID, including an exact inactive fallback. No
  // other built-in string writer is executable in the pinned factory.
  return entries;
}

std::optional<int>
practiceItemBooleanPropertySelector(std::string_view name) noexcept {
  constexpr std::string_view prefix = "practice_item";
  constexpr std::string_view selectedSuffix = "_selected";
  if (!name.starts_with(prefix)) return std::nullopt;

  bool selected = false;
  std::string_view indexText = name.substr(prefix.size());
  if (indexText.ends_with(selectedSuffix)) {
    selected = true;
    indexText.remove_suffix(selectedSuffix.size());
  }
  if (indexText.empty()) return std::nullopt;
  if (indexText.front() == '+') indexText.remove_prefix(1);
  if (indexText.empty()) return std::nullopt;

  // BooleanPropertyFactory uses Integer.parseInt. Only 1 through 16 are
  // accepted by its subsequent bounds check, so cap while parsing to retain
  // every accepted spelling (including leading zeroes and a leading +) without
  // risking an overflow in this noexcept lookup path.
  int index = 0;
  for (const char character : indexText) {
    if (character < '0' || character > '9') return std::nullopt;
    const int digit = character - '0';
    if (index > (16 - digit) / 10) return std::nullopt;
    index = index * 10 + digit;
  }
  if (index < 1 || index > 16) return std::nullopt;
  return (selected ? 3020 : 3000) + index - 1;
}

bool numberedEventName(std::string_view name, std::string_view prefix,
                       int count) noexcept {
  if (!name.starts_with(prefix)) return false;
  std::string_view indexText = name.substr(prefix.size());
  if (indexText.starts_with('+')) indexText.remove_prefix(1);
  if (indexText.empty()) return false;
  int index = 0;
  for (const char character : indexText) {
    if (character < '0' || character > '9') return false;
    const int digit = character - '0';
    if (index > (count - digit) / 10) return false;
    index = index * 10 + digit;
  }
  return index >= 1 && index <= count;
}

bool isPinnedBeatorajaEventName(std::string_view name) noexcept {
  // EventFactory.EventType names at the pinned compatibility commit. The two
  // numbered EventPattern families below are resolved before EventType there.
  static constexpr auto names = std::to_array<std::string_view>({
      "difficulty", "mode", "sort", "songbar_sort", "keyconfig",
      "skinconfig", "play", "autoplay", "practice", "open_document",
      "gauge1p", "option1p", "option2p", "optiondp", "hsfix",
      "hispeed1p", "duration1p", "hispeedautoadjust", "replay1",
      "replay2", "replay3", "replay4", "open_ir", "update_folder",
      "open_with_explorer", "open_download_site", "bga", "bgaexpand",
      "notesdisplaytiming", "notesdisplaytimingautoadjust", "target",
      "gaugeautoshift", "bottomshiftablegauge", "rival",
      "favorite_chart", "favorite_song", "lnmode", "autosavereplay1",
      "autosavereplay2", "autosavereplay3", "autosavereplay4",
      "lanecover", "lift", "hidden", "judgealgorithm", "guidese",
      "chartreplicationmode", "extranotedepth", "minemode", "scrollmode",
      "longnotemode", "seventonine_pattern", "seventonine_type",
      "constant",
  });
  return std::ranges::find(names, name) != names.end() ||
         numberedEventName(name, "keyassign", 54) ||
         numberedEventName(name, "practice_item", 16);
}

bool matchesBeatorajaBuiltinName(SkinBindingType type,
                                 std::string_view name) noexcept {
  if (type.kind == SkinBindingKind::BooleanProperty) {
    return beatorajaBooleanPropertySelector(name).has_value();
  }
  if (type.kind == SkinBindingKind::IntegerProperty) {
    return type.integerDomain == SkinIntegerPropertyDomain::IntegerValue
               ? beatorajaIntegerValuePropertySelector(name).has_value()
               : beatorajaImageIndexPropertySelector(name).has_value();
  }
  if (type.kind == SkinBindingKind::Event) {
    return isPinnedBeatorajaEventName(name);
  }
  if (type.kind == SkinBindingKind::StringWriter) {
    return name == "searchword";
  }
  return type.kind == SkinBindingKind::StringProperty &&
         beatorajaStringPropertySelector(name).has_value();
}

} // namespace

std::span<const BeatorajaNamedIntegerValueProperty>
beatorajaIntegerValueProperties() noexcept {
  // IntegerPropertyFactory has a namespace of its own. In particular,
  // `score_rate` is ID 102 here, whereas FloatPropertyFactory uses 1102 for
  // the same source name. Keep the named table separate from the Float and
  // String factories so a Lua skin receives the exact factory it requested.
  static constexpr auto aliases = std::to_array<BeatorajaNamedIntegerValueProperty>({
      {"hispeed_lr2", 10}, {"notesdisplaytiming", 12},
      {"lanecover1", 14}, {"lift1", 314}, {"hidden1", 315},
      {"lanecover2", 316},
      {"playtime_total_hour", 17}, {"playtime_total_minute", 18},
      {"playtime_totla_saecond", 19}, {"current_fps", 20},
      {"currenttime_year", 21}, {"currenttime_month", 22},
      {"currenttime_day", 23}, {"currenttime_hour", 24},
      {"currenttime_minute", 25}, {"currenttime_saecond", 26},
      {"boottime_hour", 27}, {"boottime_minute", 28},
      {"boottime_second", 29},
      {"player_playcount", 30}, {"player_clearcount", 31},
      {"player_failcount", 32}, {"player_perfect", 33},
      {"player_great", 34}, {"player_good", 35}, {"player_bad", 36},
      {"player_poor", 37}, {"player_notes", 333},
      {"volume_system", 57}, {"volume_key", 58},
      {"volume_background", 59},
      {"score", 71}, {"maxscore", 72}, {"totalnotes", 74},
      {"maxcombo", 75}, {"misscount", 76}, {"playcount", 77},
      {"clearcount", 78}, {"failcount", 79}, {"playlevel", 96},
      {"folder_level_beginner", 45}, {"folder_level_normal", 46},
      {"folder_level_hyper", 47}, {"folder_level_another", 48},
      {"folder_level_insane", 49},
      {"maxbpm", 90}, {"minbpm", 91}, {"mainbpm", 92},
      {"point", 100}, {"score2", 101}, {"score_rate", 102},
      {"score_rate_afterdot", 103}, {"maxcombo2", 105},
      {"totalnotes2", 106},
      {"groovegauge", 107}, {"groovegauge_afterdot", 407},
      {"diff_exscore", 108}, {"total_rate", 115},
      {"total_rate_afterdot", 116}, {"target_score", 121},
      {"target_score_rate", 122}, {"target_score_rate_afterdot", 123},
      {"diff_exscore2", 128}, {"target_total_rate", 135},
      {"target_total_rate_afterdot", 136},
      {"highscore", 150}, {"target_score2", 151},
      {"diff_highscore", 152}, {"diff_targetscore", 153},
      {"diff_nextrank", 154}, {"score_rate2", 155},
      {"score_rate_afterdot2", 156}, {"target_score_rate2", 157},
      {"target_score_rate_afterdot2", 158},
      {"nowbpm", 160}, {"playtime_minute", 161},
      {"playtime_second", 162}, {"timeleft_minute", 163},
      {"timeleft_second", 164}, {"loading_progress", 165},
      {"highscore2", 170}, {"score3", 171},
      {"diff_highscore2", 172}, {"target_maxcombo", 173},
      {"maxcombo3", 174}, {"diff_maxcombo", 175},
      {"target_misscount", 176}, {"misscount2", 177},
      {"diff_misscount", 178}, {"ir_rank", 179},
      {"ir_totalplayer", 180}, {"ir_prevrank", 182},
      {"best_rate", 183}, {"best_rate_afterdot", 184},
      {"ir_totalplayer2", 200}, {"ir_update_waiting", 220},
      {"ir_totalclear", 226}, {"ir_totalclearrate", 227},
      {"ir_totalclearrate_afterdot", 241}, {"ir_totalfullcombo", 228},
      {"ir_totalfullcomborate", 229},
      {"ir_totalfullcomborate_afterdot", 242},
      {"lastplay_timestamp", 243}, {"lastplay_year", 244},
      {"lastplay_month", 245}, {"lastplay_day", 246},
      {"lastplay_hour", 247}, {"lastplay_minute", 248},
      {"lastplay_second", 249}, {"rival_score", 271},
      {"folder_totalsongs", 300}, {"hispeed", 310},
      {"hispeed_afterdot", 311}, {"duration", 312},
      {"duration_green", 313}, {"chart_totalnote_n", 350},
      {"chart_totalnote_ln", 351}, {"chart_totalnote_s", 352},
      {"chart_totalnote_ls", 353}, {"chart_averagedensity", 364},
      {"chart_averagedensity_afterdot", 365}, {"chart_enddensity", 362},
      {"chart_enddensity_peak", 363}, {"chart_peakdensity", 360},
      {"chart_peakdensity_afterdot", 361}, {"chart_totalgauge", 368},
      {"clear", 370}, {"target_clear", 371},
      {"duration_average", 372}, {"duration_average_afterdot", 373},
      {"timing_average", 374}, {"timing_average_afterdot", 375},
      {"timing_stddev", 376}, {"timing_atddev_afterdot", 377},
      {"judgerank", 400}, {"miss", 420}, {"early_miss", 421},
      {"late_miss", 422}, {"totalearly", 423}, {"totallate", 424},
      {"combobreak", 425}, {"poor_plus_miss", 426},
      {"bad_plus_poor_plus_miss", 427},
      {"judge_duration1", 525}, {"judge_duration2", 526},
      {"judge_duration3", 527}, {"chartlength_minute", 1163},
      {"chartlength_second", 1164},
  });
  return aliases;
}

std::optional<int>
beatorajaIntegerValuePropertySelector(std::string_view name) noexcept {
  for (const auto &[alias, id] : beatorajaIntegerValueProperties()) {
    if (name == alias) return id;
  }
  if (const auto id = numberedPropertySelector(name, "ranking_exscore", 380)) {
    return id;
  }
  if (const auto id = numberedPropertySelector(name, "ranking_index", 390)) {
    return id;
  }
  return irClearIntegerPropertySelector(name);
}

std::span<const BeatorajaNamedBooleanProperty>
beatorajaBooleanProperties() noexcept {
  // Keep this exact BooleanType.name() table separate from other property
  // factories. Several names intentionally share a different numeric ID in
  // IntegerPropertyFactory or FloatPropertyFactory.
  static constexpr auto properties = std::to_array<BeatorajaNamedBooleanProperty>({
      {"table_song", 1008}, {"randomselectbar", 1030},
      {"randomcoursebar", 1031}, {"playablebar", 5},
      {"not_compare_rival", 624}, {"compare_rival", 625},
      {"select_bar_not_played", 100}, {"disable_save_score", 60},
      {"enable_save_score", 61}, {"no_save_clear", 62},
      {"bgaoff", 40}, {"bgaon", 41}, {"gauge_groove", 42},
      {"gauge_hard", 43}, {"autoplay_on", 33}, {"autoplay_off", 32},
      {"replay_off", 82}, {"replay_playing", 84},
      {"state_practice", 1080}, {"now_loading", 80}, {"loaded", 81},
      {"song_no_text", 174}, {"song_text", 175},
      {"chart_no_ln", 172}, {"chart_ln", 173},
      {"song_no_bga", 170}, {"song_bga", 171},
      {"chart_no_randomsequence", 178}, {"chart_randomsequence", 179},
      {"chart_no_bpmchange", 176}, {"chart_bpmchange", 177},
      {"chart_bpmstop", 1177}, {"chart_difficulty_0", 150},
      {"chart_difficulty_1", 151}, {"chart_difficulty_2", 152},
      {"chart_difficulty_3", 153}, {"chart_difficulty_4", 154},
      {"chart_difficulty_5", 155}, {"chart_judge_veryhard", 180},
      {"chart_judge_hard", 181}, {"chart_judge_normal", 182},
      {"chart_judge_easy", 183}, {"chart_judge_veryeasy", 184},
      {"chart_7key", 160}, {"chart_5key", 161}, {"chart_14key", 162},
      {"chart_10key", 163}, {"chart_9key", 164},
      {"select_bar_failed", 101}, {"select_bar_assist_easy", 1100},
      {"select_bar_light_assist_easy", 1101}, {"select_bar_easy", 102},
      {"select_bar_normal", 103}, {"select_bar_hard", 104},
      {"select_bar_exhard", 1102}, {"select_bar_fullcombo", 105},
      {"select_bar_perfect", 1103}, {"select_bar_max", 1104},
      {"replaydata_exist_1", 197}, {"replaydata_exist_2", 1197},
      {"replaydata_exist_3", 1200}, {"replaydata_exist_4", 1203},
      {"replaydata_no_1", 196}, {"replaydata_no_2", 1196},
      {"replaydata_no_3", 1199}, {"replaydata_no_4", 1202},
      {"replaydata_saved_1", 198}, {"replaydata_saved_2", 1198},
      {"replaydata_saved_3", 1201}, {"replaydata_saved_4", 1204},
      {"select_replaydata_1", 1205}, {"select_replaydata_2", 1206},
      {"select_replaydata_3", 1207}, {"select_replaydata_4", 1208},
      {"select_panel1", 21}, {"select_panel2", 22}, {"select_panel3", 23},
      {"select_somgbar", 2}, {"select_folderbar", 1},
      {"select_coursebar", 3}, {"course_class", 1002},
      {"course_mirror", 1003}, {"course_random", 1004},
      {"course_nospeed", 1005}, {"course_nogood", 1006},
      {"course_nogreat", 1007}, {"course_gauge_lr2", 1010},
      {"course_gauge_5keys", 1011}, {"course_gauge_7keys", 1012},
      {"course_gauge_9keys", 1013}, {"course_gauge_24keys", 1014},
      {"course_ln", 1015}, {"course_cn", 1016}, {"course_hcn", 1017},
      {"course_stage1", 280}, {"course_stage2", 281},
      {"course_stage3", 282}, {"course_stage4", 283},
      {"course_stage_final", 289}, {"mode_course", 290},
      {"stagefile", 191}, {"no_stagefile", 190}, {"backbmp", 195},
      {"no_backbmp", 194}, {"banner", 193}, {"no_banner", 192},
      {"judge_1p_perfect", 241}, {"judge_1p_early", 1242},
      {"judge_1p_late", 1243}, {"judge_2p_perfect", 261},
      {"judge_2p_early", 1262}, {"judge_2p_late", 1263},
      {"judge_3p_perfect", 361}, {"judge_3p_early", 1362},
      {"judge_3p_late", 1363}, {"judge_perfect_exist", 2241},
      {"judge_great_exist", 2242}, {"judge_good_exist", 2243},
      {"judge_bad_exist", 2244}, {"judge_poor_exist", 2245},
      {"judge_miss_exist", 2246}, {"lanecover1_changing", 270},
      {"lanecover1_on", 271}, {"lift1_on", 272}, {"hidden1_on", 273},
      {"border_or_more_1p", 1240}, {"gauge_1p_0_10", 230},
      {"gauge_1p_10_20", 231}, {"gauge_1p_20_30", 232},
      {"gauge_1p_30_40", 233}, {"gauge_1p_40_50", 234},
      {"gauge_1p_50_60", 235}, {"gauge_1p_60_70", 236},
      {"gauge_1p_70_80", 237}, {"gauge_1p_80_90", 238},
      {"gauge_1p_90_100", 239}, {"gauge_1p_100", 240},
      {"rank_1p_aaa", 200}, {"rank_1p_aa", 201}, {"rank_1p_a", 202},
      {"rank_1p_b", 203}, {"rank_1p_c", 204}, {"rank_1p_d", 205},
      {"rank_1p_e", 206}, {"rank_1p_f", 207},
      {"rank_result_1p_aaa", 300}, {"rank_result_1p_aa", 301},
      {"rank_result_1p_a", 302}, {"rank_result_1p_b", 303},
      {"rank_result_1p_c", 304}, {"rank_result_1p_d", 305},
      {"rank_result_1p_e", 306}, {"rank_result_1p_f", 307},
      {"rank_now_1p_aaa", 340}, {"rank_now_1p_aa", 341},
      {"rank_now_1p_a", 342}, {"rank_now_1p_b", 343},
      {"rank_now_1p_c", 344}, {"rank_now_1p_d", 345},
      {"rank_now_1p_e", 346}, {"rank_now_1p_f", 347},
      {"rank_best_1p_aaa", 320}, {"rank_best_1p_aa", 321},
      {"rank_best_1p_a", 322}, {"rank_best_1p_b", 323},
      {"rank_best_1p_c", 324}, {"rank_best_1p_d", 325},
      {"rank_best_1p_e", 326}, {"rank_best_1p_f", 327},
      {"rank_aaa", 220}, {"rank_aa", 221}, {"rank_a", 222},
      {"rank_b", 223}, {"rank_c", 224}, {"rank_d", 225},
      {"rank_e", 226}, {"rank_f", 227}, {"update_score", 330},
      {"draw_score", 1330}, {"update_maxcombo", 331},
      {"draw_maxcombo", 1331}, {"update_misscount", 332},
      {"draw_misscount", 1332}, {"update_scorerank", 335},
      {"draw_scorerank", 1335}, {"update_target", 336},
      {"draw_target", 1336}, {"result_clear", 90}, {"result_fail", 91},
      {"result_1pwin", 352}, {"result_2pwin", 353}, {"result_draw", 354},
      {"ir_offline", 50}, {"ir_online", 51}, {"ir_no_player", 603},
      {"ir_failed", 604}, {"ir_busy", 608}, {"ir_waiting", 606},
      {"chart_24key", 1160}, {"chart_48key", 1161}, {"gauge_ex", 1046},
      {"trophy_gauge_easy", 121}, {"trophy_gauge_normal", 118},
      {"trophy_gauge_hard", 119}, {"trophy_gauge_exhard", 125},
      {"trophy_option_normal", 126}, {"trophy_option_mirror", 127},
      {"trophy_option_random", 128}, {"trophy_option_rrandom", 1128},
      {"trophy_option_srandom", 129}, {"trophy_option_spiral", 1129},
      {"trophy_option_hrandom", 130}, {"trophy_option_allscr", 131},
      {"trophy_option_exrandom", 1130}, {"trophy_option_exsrandom", 1131},
      {"constant", 400},
  });
  return properties;
}

std::optional<int>
beatorajaBooleanPropertySelector(std::string_view name) noexcept {
  bool negated = false;
  while (name.starts_with('!')) {
    negated = !negated;
    name.remove_prefix(1);
  }
  std::optional<int> id = practiceItemBooleanPropertySelector(name);
  if (!id) {
    for (const auto &[alias, value] : beatorajaBooleanProperties()) {
      if (name == alias) {
        id = value;
        break;
      }
    }
  }
  return id ? std::optional<int>(negated ? -*id : *id) : std::nullopt;
}

std::span<const BeatorajaNamedImageIndexProperty>
beatorajaImageIndexProperties() noexcept {
  static constexpr auto aliases = std::to_array<BeatorajaNamedImageIndexProperty>({
      {"showjudgearea", 303}, {"markprocessednote", 305},
      {"bpmguide", 306}, {"customjudge", 301}, {"lnmode", 308},
      {"notesdisplaytimingautoadjust", 75}, {"gaugeautoshift", 78},
      {"bottomshiftablegauge", 341}, {"bga", 72}, {"difficulty", 10},
      {"mode", 11}, {"sort", 12}, {"gaugetype_1p", 40},
      {"option_1p", 42}, {"option_2p", 43}, {"option_dp", 54},
      {"hsfix", 55}, {"option_target1_1p", 61},
      {"option_target1_2p", 62}, {"option_target1_dp", 63},
      {"hispeedautoadjust", 342}, {"favorite_song", 89},
      {"favorite_chart", 90}, {"autosave_replay1", 321},
      {"autosave_replay2", 322}, {"autosave_replay3", 323},
      {"autosave_replay4", 324}, {"lanecover", 330}, {"lift", 331},
      {"hidden", 332}, {"judgealgorithm", 340}, {"guidese", 343},
      {"extranotedepth", 350}, {"minemode", 351}, {"scrollmode", 352},
      {"longnotemode", 353}, {"seventonine_pattern", 360},
      {"seventonine_type", 361}, {"cleartype", 370},
      {"cleartype_target", 371}, {"constant", 400},
      {"pattern_1p_1", 450}, {"pattern_1p_2", 451},
      {"pattern_1p_3", 452}, {"pattern_1p_4", 453},
      {"pattern_1p_5", 454}, {"pattern_1p_6", 455},
      {"pattern_1p_7", 456}, {"pattern_1p_8", 457},
      {"pattern_1p_9", 458}, {"pattern_1p_SCR", 459},
      {"pattern_2p_1", 460}, {"pattern_2p_2", 461},
      {"pattern_2p_3", 462}, {"pattern_2p_4", 463},
      {"pattern_2p_5", 464}, {"pattern_2p_6", 465},
      {"pattern_2p_7", 466}, {"pattern_2p_SCR", 469},
      {"assist_constant", 302}, {"assist_legacy", 304},
      {"assist_nomine", 307},
  });
  return aliases;
}

std::optional<int>
beatorajaImageIndexPropertySelector(std::string_view name) noexcept {
  for (const auto &[alias, id] : beatorajaImageIndexProperties()) {
    if (name == alias) return id;
  }
  if (const auto id =
          numberedPropertySelector(name, "playertype_ranking", 380)) {
    return id;
  }
  return numberedPropertySelector(name, "cleartype_ranking", 390);
}

bool isPinnedBeatorajaBooleanPropertyId(int selector) noexcept {
  return std::binary_search(kPinnedBooleanPropertyIds.begin(),
                            kPinnedBooleanPropertyIds.end(), selector);
}

SkinBuiltinBindingCatalogView gameplaySkinBuiltinCatalog() {
  static const auto entries = makeCatalog();
  static constexpr auto ranges = std::to_array<SkinBuiltinBindingCatalogRange>({
      {.type = {.kind = SkinBindingKind::TimerProperty},
       .first = 0,
       .last = std::numeric_limits<int>::max()},
      // EventFactory.getEvent(int) returns an EventPattern/EventType value
      // when present and otherwise creates a two-argument MainState dispatch
      // event. Every signed integer is therefore a valid numeric event source.
      {.type = {.kind = SkinBindingKind::Event},
       .first = std::numeric_limits<int>::min(),
       .last = std::numeric_limits<int>::max()},
  });
  return SkinBuiltinBindingCatalogView(entries, ranges,
                                       matchesBeatorajaBuiltinName);
}

} // namespace skin
