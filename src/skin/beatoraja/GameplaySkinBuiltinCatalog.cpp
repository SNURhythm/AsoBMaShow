#include "GameplaySkinBuiltinCatalog.h"

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
  for (const char *selector :
       {"judge_1p_perfect", "judge_1p_early", "judge_1p_late"}) {
    add(entries, boolean, selector);
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

  // FloatPropertyFactory.RateType provides built-in writers for the three
  // Config.AudioConfig fields and BMSPlayer's practice-menu viewport.
  // Lane-cover mutation remains unavailable through this binding surface.
  const SkinBindingType floatWriter{.kind = SkinBindingKind::FloatWriter};
  for (const int selector : {17, 18, 19, 20}) {
    add(entries, floatWriter, selector);
  }
  for (const char *selector : {"mastervolume", "keyvolume", "bgmvolume",
                               "practice_position"}) {
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

  // Timers are not a gap: TimerPropertyFactory and PlaySkinStateBridge both
  // define every nonnegative ID, including an exact inactive fallback. No
  // other built-in string writer is executable in the pinned factory.
  return entries;
}

} // namespace

bool isPinnedBeatorajaBooleanPropertyId(int selector) noexcept {
  return std::binary_search(kPinnedBooleanPropertyIds.begin(),
                            kPinnedBooleanPropertyIds.end(), selector);
}

SkinBuiltinBindingCatalogView gameplaySkinBuiltinCatalog() {
  static const auto entries = makeCatalog();
  static constexpr auto ranges = std::to_array<SkinBuiltinBindingCatalogRange>({
      // IntegerPropertyFactory owns two independent 65,536-entry caches.
      // An integer binding is legal throughout either factory's input domain;
      // the runtime supplies the upstream sentinel where Aso has no matching
      // gameplay state yet. Do not turn an otherwise loadable skin into a
      // catalog error merely because its selector is not one of our currently
      // rendered values.
      {.type = {.kind = SkinBindingKind::IntegerProperty,
                .integerDomain = SkinIntegerPropertyDomain::IntegerValue},
       .first = 0,
       .last = 65'535},
      {.type = {.kind = SkinBindingKind::IntegerProperty,
                .integerDomain = SkinIntegerPropertyDomain::ImageIndex},
       .first = 0,
       .last = 65'535},
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
  return SkinBuiltinBindingCatalogView(entries, ranges);
}

} // namespace skin
