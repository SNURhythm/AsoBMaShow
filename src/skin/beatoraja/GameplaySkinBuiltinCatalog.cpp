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

  constexpr auto integerSelectors = std::to_array(
      {14,  71,  74,  75,  90,  91,  92,  96,  101, 102, 103, 105, 107,
       110, 111, 112, 113, 114, 152, 153, 160, 171, 313, 350, 351, 352,
       353, 360, 361, 362, 363, 364, 365, 368, 407, 410, 411, 412, 413, 414,
       415, 416, 417, 418, 419, 420,
       421, 422, 425, 427, 525, 526, 527, 1163, 1164});
  for (const auto domain : {SkinIntegerPropertyDomain::IntegerValue,
                            SkinIntegerPropertyDomain::ImageIndex}) {
    const SkinBindingType integer{.kind = SkinBindingKind::IntegerProperty,
                                  .integerDomain = domain};
    for (const int selector : integerSelectors) {
      add(entries, integer, selector);
    }
    for (int selector = 500; selector <= 519; ++selector) {
      add(entries, integer, selector);
    }
    add(entries, integer, "nowbpm");
  }
  // Pinned IntegerPropertyFactory exposes the lane-cover family through
  // ValueType, not through getImageIndexProperty.
  const SkinBindingType integerValue{
      .kind = SkinBindingKind::IntegerProperty,
      .integerDomain = SkinIntegerPropertyDomain::IntegerValue};
  for (const int selector : {161, 162, 163, 164, 314, 315, 316}) {
    add(entries, integerValue, selector);
  }

  const SkinBindingType floating{.kind = SkinBindingKind::FloatProperty,
                                 .floatDomain = SkinFloatPropertyDomain::Rate};
  add(entries, floating, 4);
  add(entries, floating, 5);
  add(entries, floating, 6);
  for (int selector = 110; selector <= 115; ++selector) {
    add(entries, floating, selector);
  }
  add(entries, floating, 102);
  add(entries, floating, "lanecover");
  add(entries, floating, "lanecover2");

  const SkinBindingType string{.kind = SkinBindingKind::StringProperty};
  constexpr auto stringSelectors =
      std::to_array({10, 11, 12, 13, 14, 15, 16, 1003});
  constexpr auto stringNames = std::to_array({
      "title", "subtitle", "fulltitle", "genre", "artist", "subartist",
      "fullartist", "tablefull"});
  for (std::size_t index = 0; index < stringSelectors.size(); ++index) {
    add(entries, string, stringSelectors[index]);
    add(entries, string, stringNames[index]);
  }

  const SkinBindingType event{.kind = SkinBindingKind::Event};
  for (int selector = 301; selector <= 308; ++selector) {
    add(entries, event, selector);
  }
  // The bridge can dispatch declared custom events 1000-1999 from safe call
  // sites such as image actions. The typed catalog is also used for a custom
  // event's own action, however, where admitting that range would permit an
  // unguarded self/cyclic dispatch. Keep the context-free catalog closed until
  // validation distinguishes those binding roles or the bridge bounds cycles.

  // Timers are not a gap: TimerPropertyFactory and PlaySkinStateBridge both
  // define every nonnegative ID, including an exact inactive fallback. No
  // built-in float or string writer is executable yet.
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
      {.type = {.kind = SkinBindingKind::TimerProperty},
       .first = 0,
       .last = std::numeric_limits<int>::max()},
  });
  return SkinBuiltinBindingCatalogView(entries, ranges);
}

} // namespace skin
