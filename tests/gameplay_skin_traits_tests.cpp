#include "skin/GameplaySkinTraits.h"
#include "skin/SkinTargetTraits.h"
#include "music_select_skin_ledger_evidence.h"

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

void testPinnedBeatorajaGameplayTraitMapping() {
  const auto ten = skin::gameplaySkinTraitForSkinType(3);
  require(ten.has_value() && ten->keyMode == 10 && ten->label == "10K",
          "Beatoraja play10 type maps to 10K");

  const auto double24 = skin::gameplaySkinTraitForKeyMode(48);
  require(double24.has_value() && double24->skinType == 17 &&
              double24->label == "24K Double",
          "Beatoraja 24K Double maps to a 48-key chart");

  require(!skin::gameplaySkinTraitForSkinType(5).has_value(),
          "non-gameplay skin types are not gameplay traits");
  require(!skin::gameplaySkinTraitForKeyMode(14)->label.empty(),
          "every supported gameplay key mode has a visible label");
}

void testResultTargetsAreFirstClassTraits() {
  const auto result = skin::skinTargetTraitForType(7);
  require(result.has_value() && result->kind == skin::SkinTargetKind::Result &&
              result->label == "Result",
          "Beatoraja result type maps to the Result target");
  const auto course = skin::skinTargetTraitForType(15);
  require(course.has_value() &&
              course->kind == skin::SkinTargetKind::CourseResult &&
              course->label == "Course Result",
          "Beatoraja course-result type maps to Course Result target");
}

void testMusicSelectTargetIsFirstClassAndDefaultsToBuiltIn() {
  const auto target = skin::skinTargetTraitForType(5);
  require(target.has_value() &&
              target->kind == skin::SkinTargetKind::MusicSelect &&
              target->keyMode == 0 && target->label == "Music Select",
          "Beatoraja type 5 maps to the Music Select target");
}

} // namespace

int main(int argc, char **argv) {
  testPinnedBeatorajaGameplayTraitMapping();
  testResultTargetsAreFirstClassTraits();
  testMusicSelectTargetIsFirstClassAndDefaultsToBuiltIn();
  return music_select_skin_ledger_evidence::finish(
      argc, argv, "gameplay_skin_traits_tests", 0,
      {"select.target.music-select"}, "gameplay skin trait test failures",
      "gameplay skin trait tests passed");
}
