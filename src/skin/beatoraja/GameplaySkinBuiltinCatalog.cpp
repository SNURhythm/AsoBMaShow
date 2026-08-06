#include "GameplaySkinBuiltinCatalog.h"

#include <array>
#include <limits>
#include <string>
#include <vector>

namespace skin {
namespace {

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
  entries.reserve(240);

  const SkinBindingType boolean{.kind = SkinBindingKind::BooleanProperty};
  for (const int selector : std::to_array(
           {42,  43,  80,  81,  84,   150, 151,  152,  153,  154,
            155, 170, 171, 172, 173,  176, 177,  180,  181,  182,
            183, 184, 190, 191, 194,  195, 241, 271,  272,  273,
            1080, 1240, 1242,
            1243, 2243, 2244, 2245})) {
    add(entries, boolean, selector);
  }
  for (int selector = 230; selector <= 240; ++selector) {
    add(entries, boolean, selector);
  }
  for (const char *selector :
       {"judge_1p_perfect", "judge_1p_early", "judge_1p_late"}) {
    add(entries, boolean, selector);
  }

  constexpr auto integerSelectors = std::to_array(
      {14,  71,  74,  75,  90,  91,  92,  96,  101, 102, 103, 105, 107,
       110, 111, 112, 113, 114, 152, 153, 160, 171, 313, 350, 351, 352,
       353, 407, 410, 411, 412, 413, 414, 415, 416, 417, 418, 419, 420,
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
